/*
 * XIAO nRF52840 Sense - Asymmetric Debounced Keyword Spotting ("on" / "off")
 * ---------------------------------------------------------------------------
 * Fixes two specific bugs on top of the previous debounce version:
 *
 *   1) SLUGGISH "OFF": "off" is a short, sharp word - by the time 3 frames
 *      agree, the word is often over. Fix: per-label consecutive-frame
 *      requirements. "off" needs only 1 frame; "on" (prone to ambient-noise
 *      ghost triggers) needs 4.
 *
 *   2) DELAYED GHOST "ON" (4-5s after a real "off"): the sliding feature
 *      window inside run_classifier_continuous() can carry residual context
 *      across a trigger/cooldown boundary. Fix: explicitly call the
 *      documented run_classifier_init() to hard-flush that static feature
 *      matrix at the exact moment cooldown expires, so classification
 *      resumes on genuinely fresh audio context rather than stale history.
 *      (Also: run_classifier_init() was never called in setup() at all in
 *      the previous version - the Edge Impulse SDK docs require it before
 *      any call to run_classifier_continuous(). That's fixed here too.)
 *
 * Classification continues to run DURING cooldown (not skipped) so the
 * sliding window stays populated with live audio - skipping it would make
 * the stale-window problem worse, not better.
 */

#include <PDM.h>
#include <kavi-cs-project-1_inferencing.h>

#define INDICATOR_LED D6

inline void ledOn()  { digitalWrite(INDICATOR_LED, HIGH); }
inline void ledOff() { digitalWrite(INDICATOR_LED, LOW);  }

// ---- Asymmetric tunables ----
static const float   CONFIDENCE_THRESHOLD_ON  = 0.93f; // higher bar - "on" ghost-triggers on ambient noise
static const float   CONFIDENCE_THRESHOLD_OFF = 0.85f; // lower bar - "off" is short/sharp, needs to fire fast
static const uint8_t CONSECUTIVE_FRAMES_ON    = 4;      // slow to confirm, resists false positives
static const uint8_t CONSECUTIVE_FRAMES_OFF   = 1;      // near-instant, matches the word's short duration
static const unsigned long COOLDOWN_MS        = 1800;

// ---- LED state tracking ----
enum LedState { LED_STATE_UNKNOWN, LED_STATE_ON, LED_STATE_OFF };
static LedState currentLedState = LED_STATE_UNKNOWN;

// ---- Debounce state ----
static const char*   lastCandidateLabel = nullptr;
static uint8_t        consecutiveCount  = 0;
static unsigned long  cooldownUntil     = 0;
static bool            wasInCooldown    = false; // tracks the cooldown edge for window-flush timing

typedef struct {
  int16_t *buffer;
  uint8_t  buf_ready;
  uint32_t buf_count;
  uint32_t n_samples;
} inference_t;

static inference_t inference;
static signed short sampleBuffer[2048];
static bool debug_nn = false;

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  pinMode(INDICATOR_LED, OUTPUT);
  ledOff();
  currentLedState = LED_STATE_OFF;

  Serial.println("Edge Impulse keyword spotting - 'on' / 'off' (asymmetric debounce)");

  if (microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE) == false) {
    Serial.println("ERROR: Failed to start microphone / allocate inference buffer");
    return;
  }

  // REQUIRED before any run_classifier_continuous() call - this was missing
  // in the previous version. It allocates/clears the static sliding-window
  // feature matrix that continuous classification depends on.
  run_classifier_init();
}

void loop() {
  bool recorded = microphone_inference_record();
  if (!recorded) {
    Serial.println("ERROR: Failed to record audio slice");
    return;
  }

  unsigned long now = millis();
  bool inCooldown = (now < cooldownUntil);

  // --- Hard window flush exactly at the cooldown-exit edge ---
  // Fires once, the instant cooldown ends, so classification resumes on a
  // genuinely fresh feature window instead of residual pre-cooldown context.
  if (wasInCooldown && !inCooldown) {
    run_classifier_init();
    lastCandidateLabel = nullptr;
    consecutiveCount = 0;
    Serial.println("# Cooldown expired - sliding window flushed for fresh context");
  }
  wasInCooldown = inCooldown;

  // Classification still runs during cooldown - keeps the sliding window
  // populated with live audio rather than going stale while we're "deaf".
  signal_t signal;
  signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
  signal.get_data     = &microphone_audio_signal_get_data;

  ei_impulse_result_t result = { 0 };

  EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
  if (r != EI_IMPULSE_OK) {
    Serial.print("ERROR: run_classifier_continuous failed (");
    Serial.print(r);
    Serial.println(")");
    return;
  }

  if (inCooldown) {
    // Still locked out - keep debounce counters clean but don't act.
    lastCandidateLabel = nullptr;
    consecutiveCount = 0;
    return;
  }

  float on_score  = 0.0f;
  float off_score = 0.0f;

  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    const char *label = result.classification[ix].label;
    float value = result.classification[ix].value;

    if (strcmp(label, "on") == 0)  on_score  = value;
    if (strcmp(label, "off") == 0) off_score = value;
  }

  // --- Determine this frame's winning candidate using PER-LABEL thresholds ---
  const char* candidate = nullptr;
  if (on_score > CONFIDENCE_THRESHOLD_ON && on_score >= off_score) {
    candidate = "on";
  } else if (off_score > CONFIDENCE_THRESHOLD_OFF && off_score > on_score) {
    candidate = "off";
  }

  // --- Consecutive-frame tracking ---
  if (candidate != nullptr && lastCandidateLabel != nullptr &&
      strcmp(candidate, lastCandidateLabel) == 0) {
    consecutiveCount++;
  } else if (candidate != nullptr) {
    lastCandidateLabel = candidate;
    consecutiveCount = 1;
  } else {
    lastCandidateLabel = nullptr;
    consecutiveCount = 0;
  }

  // --- Act only once confirmed, using PER-LABEL frame requirements ---
  if (candidate != nullptr) {
    uint8_t requiredFrames = (strcmp(candidate, "on") == 0)
                                ? CONSECUTIVE_FRAMES_ON
                                : CONSECUTIVE_FRAMES_OFF;

    if (consecutiveCount >= requiredFrames) {
      if (strcmp(candidate, "on") == 0 && currentLedState != LED_STATE_ON) {
        ledOn();
        currentLedState = LED_STATE_ON;
        cooldownUntil = now + COOLDOWN_MS;
        Serial.print("Detected 'on' (confirmed x");
        Serial.print(consecutiveCount);
        Serial.print(", score=");
        Serial.print(on_score, 2);
        Serial.println(") -> LED ON");
      } else if (strcmp(candidate, "off") == 0 && currentLedState != LED_STATE_OFF) {
        ledOff();
        currentLedState = LED_STATE_OFF;
        cooldownUntil = now + COOLDOWN_MS;
        Serial.print("Detected 'off' (confirmed x");
        Serial.print(consecutiveCount);
        Serial.print(", score=");
        Serial.print(off_score, 2);
        Serial.println(") -> LED OFF");
      }

      // Reset debounce state after acting so the next command needs fresh confirmation
      lastCandidateLabel = nullptr;
      consecutiveCount = 0;
    }
  }
}

static void pdm_data_ready_inference_callback(void) {
  int bytesAvailable = PDM.available();
  int bytesRead = PDM.read((char *)&sampleBuffer[0], bytesAvailable);

  if (inference.buf_ready == 0) {
    for (int i = 0; i < (bytesRead >> 1); i++) {
      inference.buffer[inference.buf_count++] = sampleBuffer[i];

      if (inference.buf_count >= inference.n_samples) {
        inference.buf_count = 0;
        inference.buf_ready = 1;
        break;
      }
    }
  }
}

static bool microphone_inference_start(uint32_t n_samples) {
  inference.buffer = (int16_t *)malloc(n_samples * sizeof(int16_t));
  if (inference.buffer == NULL) {
    return false;
  }

  inference.buf_count = 0;
  inference.n_samples = n_samples;
  inference.buf_ready  = 0;

  PDM.onReceive(&pdm_data_ready_inference_callback);
  PDM.setBufferSize(4096);

  if (!PDM.begin(1, EI_CLASSIFIER_FREQUENCY)) {
    Serial.println("ERROR: Failed to start PDM!");
    return false;
  }

  PDM.setGain(40);

  return true;
}

static bool microphone_inference_record(void) {
  inference.buf_ready  = 0;
  inference.buf_count  = 0;

  while (inference.buf_ready == 0) {
    delay(1);
  }

  return true;
}

static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr) {
  numpy::int16_to_float(&inference.buffer[offset], out_ptr, length);
  return 0;
}
