#include <nrf_timer.h>

// ---- Configuration ----
#define SAMPLE_RATE_HZ   10000   
#define BUFFER_SIZE      2048    
#define ADC_PIN          A0
#define ADC_RESOLUTION_BITS 12
#define SYNC_WORD        0xAA55

static volatile uint16_t bufferA[BUFFER_SIZE];
static volatile uint16_t bufferB[BUFFER_SIZE];

static volatile uint16_t *activeBuffer = bufferA;   
static volatile uint16_t *readyBuffer  = nullptr;   
static volatile uint16_t  writeIndex   = 0;
static volatile bool      bufferReady  = false;
static volatile uint32_t  droppedBlocks = 0; 

static volatile int16_t saadc_val = 0;

// -------------------------------------------------------------------
// Fast Hardware Timer ISR
// -------------------------------------------------------------------
static void timer4_isr(void) {
  if (NRF_TIMER4->EVENTS_COMPARE[0] != 0) {
    NRF_TIMER4->EVENTS_COMPARE[0] = 0;

    // Fast direct hardware SAADC sample trigger (prevents USB/mbed crash)
    NRF_SAADC->TASKS_SAMPLE = 1;
    while (!NRF_SAADC->EVENTS_END);
    NRF_SAADC->EVENTS_END = 0;

    uint16_t sample = (saadc_val < 0) ? 0 : (uint16_t)saadc_val;
    activeBuffer[writeIndex++] = sample;

    if (writeIndex >= BUFFER_SIZE) {
      writeIndex = 0;

      if (!bufferReady) {
        readyBuffer  = activeBuffer;
        activeBuffer = (activeBuffer == bufferA) ? bufferB : bufferA;
        bufferReady  = true;
      } else {
        droppedBlocks++;
      }
    }
  }
}

// -------------------------------------------------------------------
// Hardware timer setup
// -------------------------------------------------------------------
static void setupSamplingTimer(uint32_t sampleRateHz) {
  NRF_TIMER4->TASKS_STOP  = 1;
  NRF_TIMER4->TASKS_CLEAR = 1;

  NRF_TIMER4->MODE     = TIMER_MODE_MODE_Timer;
  NRF_TIMER4->BITMODE  = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER4->PRESCALER = 4;  // 1 MHz tick

  NRF_TIMER4->CC[0] = 1000000UL / sampleRateHz;

  NRF_TIMER4->SHORTS = (TIMER_SHORTS_COMPARE0_CLEAR_Enabled
                         << TIMER_SHORTS_COMPARE0_CLEAR_Pos);

  NRF_TIMER4->INTENSET = (TIMER_INTENSET_COMPARE0_Enabled
                           << TIMER_INTENSET_COMPARE0_Pos);

  NVIC_SetVector(TIMER4_IRQn, (uint32_t)timer4_isr);
  NVIC_SetPriority(TIMER4_IRQn, 2);
  NVIC_ClearPendingIRQ(TIMER4_IRQn);
  NVIC_EnableIRQ(TIMER4_IRQn);

  NRF_TIMER4->TASKS_START = 1;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; } 

  analogReadResolution(ADC_RESOLUTION_BITS);
  
  // Initialize SAADC hardware once and map output buffer
  analogRead(ADC_PIN);
  NRF_SAADC->RESULT.PTR = (uint32_t)&saadc_val;
  NRF_SAADC->RESULT.MAXCNT = 1;

  setupSamplingTimer(SAMPLE_RATE_HZ);
}

void loop() {
  if (bufferReady) {
    const uint16_t *toSend = (const uint16_t *)readyBuffer;

    bufferReady = false;

    uint16_t header[2] = { SYNC_WORD, BUFFER_SIZE };
    Serial.write((uint8_t *)header, sizeof(header));
    Serial.write((uint8_t *)toSend, BUFFER_SIZE * sizeof(uint16_t));
  }
}