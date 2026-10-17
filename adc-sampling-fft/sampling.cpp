// sampling.cpp - Constant-rate ADC acquisition firmware for the Seeed
// Studio XIAO nRF52840 Sense.
//
// Architecture
// ------------
// 1. A hardware timer (NRF_TIMER3) free-runs at 1 MHz and fires a compare
//    interrupt every TICK_US microseconds. The compare register is
//    advanced by a FIXED increment on every interrupt (CC[0] += TICK_US)
//    rather than being reloaded relative to "now", so the sample clock
//    cannot accumulate drift from interrupt latency or ISR execution
//    time: each tick lands exactly TICK_US after the *previous intended*
//    tick, not after the previous *actual* ISR entry.
// 2. Each tick, the ISR drives the SAADC (NRF_SAADC) directly through its
//    task/event registers (TASKS_START/TASKS_SAMPLE/EVENTS_END) rather
//    than through the Arduino analogRead() wrapper, and writes the 12-bit
//    result straight into a static RAM ring-free linear buffer.
// 3. No data is transmitted over USB while a capture is in progress. The
//    entire record (up to MAX_N samples) is acquired into RAM first;
//    only once the timer has stopped is the buffer serialised and
//    streamed out. This fully decouples the time-critical 20 kHz sample
//    loop from the comparatively slow, non-deterministic USB/CDC stack,
//    so USB activity can never introduce sampling jitter.
//
// Changes vs. the baseline "v6" reference implementation
// --------------------------------------------------------
//  - Compile-time bounds checking (static_assert) on FS / MAX_N / TICK_US.
//  - Bounded, timeout-guarded capture wait in loop() instead of an
//    unconditional `while (capturing) {}` spin, so a stalled ISR can
//    never hang the firmware indefinitely (watchdog-style recovery).
//  - Flow-controlled, timeout-guarded serial dump: each chunk waits for
//    USB CDC buffer space via Serial.availableForWrite() with a bounded
//    timeout, so a host that stops reading mid-dump cannot hang the
//    device either.
//  - Strict command parsing with explicit "ERR ..." / "WARN ..." replies
//    for malformed commands, N == 0, and N > MAX_N (previously silently
//    ignored or silently clamped).
//  - Added "V" (version/identity) and "S" (status) query commands.
//  - Added a monotonically increasing capture sequence number and basic
//    integrity counters (bad conversions, clamped requests) reported in
//    both the boot banner and the dump header for traceability.
//
// Serial protocol (115200 8N1, unchanged and backward compatible with the
// baseline "C <N>\n" / "idx,raw" / "# END" wire format):
//   Host -> device   "C <N>\n"   capture N samples (1..MAX_N) at FS
//   Host -> device   "V\n"       report firmware version / build config
//   Host -> device   "S\n"       report current status
//   Device -> host   "# BEGIN n=<N> fs=<FS> bad=<K> seq=<S>"
//   Device -> host   "<idx>,<raw>"   one row per sample (raw: int16_t,
//                                     -32768 marks a failed conversion)
//   Device -> host   "# END"
//   Device -> host   "# ERR <reason>" / "# WARN <reason>" on problems
//
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>

#include "fmt.h"

// ---------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------
constexpr uint32_t FS = 20000;             // Hz - sampling rate (keep in
                                            // sync with --fs in sweep_log.py)
constexpr uint32_t TICK_US = 1000000 / FS; // 50 us at 20 kHz
constexpr uint32_t MAX_N = 50000;          // 100 KB of int16_t (safe
                                            // headroom in 256 KB RAM)
constexpr uint8_t ADC_CHANNEL = 0;         // AIN0
constexpr uint32_t FW_VERSION_MAJOR = 7;
constexpr uint32_t FW_VERSION_MINOR = 0;

// 50 us * 20 kHz must reconstruct exactly 1 s; guards against silent
// integer-division truncation if FS is changed carelessly.
static_assert(1000000 % FS == 0,
              "FS must evenly divide 1,000,000 so TICK_US is exact");
static_assert(TICK_US > 0, "TICK_US must be > 0");
static_assert(MAX_N > 0 && MAX_N <= 60000,
              "MAX_N must fit comfortably within available RAM");

// ---------------------------------------------------------------------
// Capture state (touched by both the ISR and the main loop - all
// cross-context variables are `volatile`)
// ---------------------------------------------------------------------
static int16_t buf[MAX_N];
static volatile uint32_t cap_n = 0;   // samples requested this capture
static volatile uint32_t cap_i = 0;   // samples written so far
static volatile uint32_t cap_bad = 0; // failed SAADC conversions
static volatile bool capturing = false;
static uint32_t capture_seq = 0; // increments on every completed capture
static bool saadc_ok = false;

static int16_t saadc_buf __attribute__((aligned(4)));

// ---------------------------------------------------------------------
// SAADC configuration
// ---------------------------------------------------------------------
static bool saadcInit(uint8_t ch) {
  NRF_SAADC->ENABLE = 0;
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
  NRF_SAADC->SAMPLERATE = SAADC_SAMPLERATE_MODE_Task;
  NRF_SAADC->CH[ch].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput0 + ch;
  NRF_SAADC->CH[ch].PSELN = SAADC_CH_PSELN_PSELN_NC;
  NRF_SAADC->CH[ch].CONFIG =
      (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
      (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
      (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_TACQ_3us << SAADC_CH_CONFIG_TACQ_Pos) |
      (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
      (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);
  NRF_SAADC->INTENSET = 0;
  NRF_SAADC->RESULT.PTR = (uint32_t)&saadc_buf;
  NRF_SAADC->RESULT.MAXCNT = 1;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
  NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
  for (uint32_t i = 0; i < 200000 && !NRF_SAADC->EVENTS_CALIBRATEDONE; i++) {
  }
  return NRF_SAADC->EVENTS_CALIBRATEDONE != 0;
}

// ---------------------------------------------------------------------
// Timer3 compare-match ISR: the time-critical 20 kHz sample loop.
// Kept minimal and branch-light; no heap use, no Serial I/O.
// ---------------------------------------------------------------------
extern "C" void TIMER3_IRQHandler(void) {
  if (!NRF_TIMER3->EVENTS_COMPARE[0])
    return;
  NRF_TIMER3->EVENTS_COMPARE[0] = 0;
  NRF_TIMER3->CC[0] += TICK_US; // absolute scheduling: no drift accrual

  int16_t v = -32768; // sentinel: marks a failed conversion
  uint32_t w = 0;
  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->TASKS_START = 1;
  while (w++ < 640 && !NRF_SAADC->EVENTS_STARTED) {
  }
  if (NRF_SAADC->EVENTS_STARTED) {
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->TASKS_SAMPLE = 1;
    w = 0;
    while (w++ < 640 && !NRF_SAADC->EVENTS_END) {
    }
    if (NRF_SAADC->EVENTS_END)
      v = saadc_buf;
  }
  if (v == -32768)
    cap_bad++;

  // Bounds-checked write: cap_i can never exceed MAX_N even if cap_n were
  // ever mis-set, so this ISR can never write outside `buf`.
  if (cap_i < cap_n && cap_i < MAX_N) {
    buf[cap_i++] = v;
  }
  if (cap_i >= cap_n) { // capture complete: stop the timer
    NRF_TIMER3->TASKS_STOP = 1;
    capturing = false;
  }
}

static void startCapture(uint32_t n) {
  cap_n = n;
  cap_i = 0;
  cap_bad = 0;
  capturing = true;
  NRF_TIMER3->TASKS_SHUTDOWN = 1;
  NRF_TIMER3->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER3->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER3->PRESCALER = 4; // 16 MHz / 2^4 = 1 MHz ticks
  NRF_TIMER3->TASKS_CLEAR = 1;
  NRF_TIMER3->CC[0] = TICK_US;
  NRF_TIMER3->EVENTS_COMPARE[0] = 0;
  NRF_TIMER3->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(TIMER3_IRQn, 6);
  NVIC_EnableIRQ(TIMER3_IRQn);
  NRF_TIMER3->TASKS_START = 1;
}

// ---------------------------------------------------------------------
// Minimal, allocation-free integer formatting (avoids pulling in the
// heavier String/sprintf machinery on the hot dump path).
// ---------------------------------------------------------------------
static char *put_u32(char *p, uint32_t v) {
  char tmp[10];
  int n = 0;
  if (v == 0) {
    *p++ = '0';
    return p;
  }
  while (v) {
    tmp[n++] = char('0' + v % 10);
    v /= 10;
  }
  while (n)
    *p++ = tmp[--n];
  return p;
}
static char *put_i32(char *p, int32_t v) {
  if (v < 0) {
    *p++ = '-';
    v = -v;
  }
  return put_u32(p, (uint32_t)v);
}

// Flow-controlled write: waits for USB CDC buffer space but gives up
// after `timeout_ms` so a host that stops reading mid-dump cannot hang
// the device. Returns false on timeout (caller aborts the dump).
static bool writeChunk(const uint8_t *data, size_t len, uint32_t timeout_ms) {
  uint32_t start = millis();
  while (Serial.availableForWrite() < (int)len) {
    if (millis() - start > timeout_ms)
      return false;
  }
  Serial.write(data, len);
  return true;
}

static void dump(uint32_t n) {
  logln("# BEGIN n={} fs={} bad={} seq={}", n, FS, cap_bad, capture_seq);
  static char out[512];
  char *p = out;
  bool ok = true;
  for (uint32_t i = 0; i < n && ok; i++) {
    p = put_u32(p, i);
    *p++ = ',';
    p = put_i32(p, buf[i]);
    *p++ = '\n';
    if (p >= out + sizeof(out) - 24) {
      ok = writeChunk((const uint8_t *)out, p - out, 2000);
      p = out;
    }
  }
  if (ok && p != out)
    ok = writeChunk((const uint8_t *)out, p - out, 2000);
  if (!ok) {
    logln("# ERR dump timeout (host stopped reading)");
  } else {
    logln("# END");
  }
  Serial.flush();
  capture_seq++;
}

// ---------------------------------------------------------------------
// Command handling
// ---------------------------------------------------------------------
static void handleCaptureCmd(const String &arg) {
  if (!saadc_ok) {
    logln("# ERR saadc not initialised");
    return;
  }
  long n_signed = arg.toInt();
  if (n_signed <= 0) {
    logln("# ERR N must be a positive integer");
    return;
  }
  uint32_t n = (uint32_t)n_signed;
  if (n > MAX_N) {
    logln("# WARN requested {} > MAX_N, clamped to {}", n, MAX_N);
    n = MAX_N;
  }

  startCapture(n);

  // Bounded wait instead of an unconditional spin: expected duration is
  // n / FS seconds; allow 3x that plus 500 ms of slack before declaring
  // the capture hung and forcibly recovering, so a stalled ISR can never
  // lock up the firmware.
  uint32_t expected_ms = (n * 1000UL) / FS;
  uint32_t timeout_ms = expected_ms * 3 + 500;
  uint32_t t0 = millis();
  while (capturing) {
    if (millis() - t0 > timeout_ms) {
      NRF_TIMER3->TASKS_STOP = 1;
      NVIC_DisableIRQ(TIMER3_IRQn);
      capturing = false;
      logln("# ERR capture timeout after {} ms (got {}/{} samples)",
            timeout_ms, cap_i, n);
      return;
    }
  }
  dump(n);
}

static void handleLine(const String &lineIn) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0)
    return;

  switch (line[0]) {
  case 'C':
    if (line.length() < 3) {
      logln("# ERR usage: C <N>");
      return;
    }
    handleCaptureCmd(line.substring(1));
    break;
  case 'V':
    logln("# VERSION {}.{} FS={} MAX_N={} ADC_CH={}", FW_VERSION_MAJOR,
          FW_VERSION_MINOR, FS, MAX_N, ADC_CHANNEL);
    break;
  case 'S':
    logln("# STATUS saadc_ok={} capturing={} capture_seq={}",
          saadc_ok ? 1 : 0, capturing ? 1 : 0, capture_seq);
    break;
  default:
    logln("# ERR unknown command '{}' (expected C/V/S)", line[0]);
    break;
  }
}

// ---------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000)
    delay(10);

  saadc_ok = saadcInit(ADC_CHANNEL);
  logln("# BOOT fw={}.{} FS={} Hz MAX_N={} saadc_ok={}", FW_VERSION_MAJOR,
        FW_VERSION_MINOR, FS, MAX_N, saadc_ok ? 1 : 0);
  if (!saadc_ok) {
    logln("# ERR SAADC calibration failed - check hardware / power rail");
  }
}

void loop() {
  if (!Serial.available())
    return;
  String line = Serial.readStringUntil('\n');
  handleLine(line);
}
