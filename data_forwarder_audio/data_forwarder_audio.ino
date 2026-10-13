#include <PDM.h>

static const char PDM_CHANNELS  = 1;
static const int  PDM_FREQUENCY = 16000;

static short sampleBuffer[512];
volatile int samplesRead = 0;

void onPDMdata(void) {
  int bytesAvailable = PDM.available();
  PDM.read(sampleBuffer, bytesAvailable);
  samplesRead = bytesAvailable / 2;
}

void setup() {
  Serial.begin(1152000);
  
  // Wait up to 3 seconds for Serial
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000));

  PDM.onReceive(onPDMdata);

  // Initialize PDM mic
  if (!PDM.begin(PDM_CHANNELS, PDM_FREQUENCY)) {
    while (1) {
      Serial.println("-10000"); // Error indicator for forwarder
      delay(500);
    }
  }

  PDM.setGain(20);
}

void loop() {
  if (samplesRead) {
    int n = samplesRead;
    samplesRead = 0;

    for (int i = 0; i < n; i++) {
      Serial.println(sampleBuffer[i]);
    }
  }
}