// Minimal main that delegates to the modular NfcSession
#include <Arduino.h>
#include "nfc_session.h"

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  // UART2 on ESP32-S3: RX=44, TX=43 as used previously
  NfcSession::begin(Serial2, 44, 43, 115200);
}

void loop() {
  NfcSession::tick();
}