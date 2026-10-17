/*
 * XIAO nRF52840 Sense - Hardware-Timer ADC Sampler (Sampling & FFT practical)
 * -----------------------------------------------------------------------
 * Board: Seeed Studio XIAO nRF52840 Sense
 * FQBN:  Seeeduino:mbed:xiaonRF52840Sense
 *
 * Samples A0 at a STRICT, hardware-timed rate using NRF_TIMER2 (NOT
 * delay()/micros() polling - jitter from those would corrupt both the
 * reconstructed waveform and the FFT). Streams samples to the host as
 * raw binary over Serial, double-buffered so the ISR never blocks on
 * Serial I/O.
 *
 * WIRING: signal generator -> protection network -> A0
 *   (see the earlier ADC input protection circuit: 1k series resistor,
 *   two 1N4007 clamp diodes to 3V3/GND, 100nF filter cap to GND)
 *
 * Signal under test: 0-3V sine, 1.5V DC offset, 10 Hz - 1 kHz.
 *
 * WIRE PROTOCOL (binary, little-endian):
 *   [uint16_t SYNC = 0xAA55] [uint16_t sample_count] [sample_count * uint16_t samples]
 */

// ---- Configuration ----
#define SAMPLE_RATE_HZ   10000   // must satisfy Nyquist for 1kHz signal (10x oversampled here)
#define BUFFER_SIZE      2048    // samples per block sent to host (matches Python num_samples)
#define ADC_PIN          A0
#define ADC_RESOLUTION_BITS 12

#define SYNC_WORD        0xAA55

// ---- Double buffers filled by the timer ISR ----
static volatile uint16_t bufferA[BUFFER_SIZE];
static volatile uint16_t bufferB[BUFFER_SIZE];

static volatile uint16_t *activeBuffer = bufferA;   // ISR writes here
static volatile uint16_t *readyBuffer  = nullptr;   // loop() reads/sends this
static volatile uint16_t  writeIndex   = 0;
static volatile bool      bufferReady  = false;
static volatile uint32_t  droppedBlocks = 0; // diagnostic: count of overruns (loop too slow)

// -------------------------------------------------------------------
// Hardware timer setup (NRF_TIMER2, 1MHz tick via PRESCALER=4)
// -------------------------------------------------------------------
static void setupSamplingTimer(uint32_t sampleRateHz) {
  NRF_TIMER2->TASKS_STOP  = 1;
  NRF_TIMER2->TASKS_CLEAR = 1;

  NRF_TIMER2->MODE     = TIMER_MODE_MODE_Timer;
  NRF_TIMER2->BITMODE  = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER2->PRESCALER = 4;  // 16 MHz / 2^4 = 1 MHz tick (1 us resolution)

  // Compare value in microseconds = 1,000,000 / sampleRateHz
  NRF_TIMER2->CC[0] = 1000000UL / sampleRateHz;

  // Auto-clear the counter on compare match -> free-running periodic interrupt
  NRF_TIMER2->SHORTS = (TIMER_SHORTS_COMPARE0_CLEAR_Enabled
                         << TIMER_SHORTS_COMPARE0_CLEAR_Pos);

  NRF_TIMER2->INTENSET = (TIMER_INTENSET_COMPARE0_Enabled
                           << TIMER_INTENSET_COMPARE0_Pos);

  NVIC_SetPriority(TIMER2_IRQn, 2);
  NVIC_ClearPendingIRQ(TIMER2_IRQn);
  NVIC_EnableIRQ(TIMER2_IRQn);

  NRF_TIMER2->TASKS_START = 1;
}

// -------------------------------------------------------------------
// Timer ISR: fires at exactly SAMPLE_RATE_HZ. Keep this FAST - no
// Serial calls, no malloc, no floating point beyond what analogRead
// itself does internally.
// -------------------------------------------------------------------
extern "C" void TIMER2_IRQHandler(void) {
  if (NRF_TIMER2->EVENTS_COMPARE[0] != 0) {
    NRF_TIMER2->EVENTS_COMPARE[0] = 0;

    uint16_t sample = analogRead(ADC_PIN); // 12-bit value, 0-4095

    activeBuffer[writeIndex++] = sample;

    if (writeIndex >= BUFFER_SIZE) {
      writeIndex = 0;

      if (!bufferReady) {
        // Previous block has been picked up by loop() already (or never
        // produced one yet) - safe to hand off the buffer we just filled.
        readyBuffer  = activeBuffer;
        activeBuffer = (activeBuffer == bufferA) ? bufferB : bufferA;
        bufferReady  = true;
      } else {
        // loop() hasn't consumed the last ready buffer yet. Overwriting
        // the same buffer in place rather than touching the other one,
        // which is still being read by loop(). This sample block is lost;
        // bump a diagnostic counter so you can see it in the data if it
        // ever happens.
        droppedBlocks++;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; } // native USB CDC - wait for host to open the port

  analogReadResolution(ADC_RESOLUTION_BITS);
  // NOTE: verify your core's actual ADC full-scale reference voltage
  // (commonly ~3.0-3.3V on nRF52840 Arduino cores depending on default
  // analogReference()). Calibrate ADC_REF_VOLTAGE in the Python script
  // against a multimeter reading for accurate voltage reconstruction
  // rather than assuming a datasheet-nominal value.

  setupSamplingTimer(SAMPLE_RATE_HZ);
}

void loop() {
  if (bufferReady) {
    const uint16_t *toSend = (const uint16_t *)readyBuffer;

    // Mark consumed BEFORE the (relatively slow) Serial.write() calls so
    // the ISR can hand off the next block as soon as it's ready, instead
    // of waiting for transmission to finish.
    bufferReady = false;

    uint16_t header[2] = { SYNC_WORD, BUFFER_SIZE };
    Serial.write((uint8_t *)header, sizeof(header));
    Serial.write((uint8_t *)toSend, BUFFER_SIZE * sizeof(uint16_t));
  }
}
