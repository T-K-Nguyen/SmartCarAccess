// Minimal main that delegates to the modular NfcSession and starts BLE services
#include <Arduino.h>
#include "nfc_session.h"
#include "ble/ble.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  while (!Serial) {
    delay(10);
  }
  // Start BLE (admin/auth/echo services)
  BLEMod::begin();
  Serial.println("[BLE] Services started.");

  // UART2 on ESP32-S3: RX=44, TX=43 for PN532 (HSU)
  NfcSession::begin(Serial2, 44, 43, 115200);
}

void loop() {
  NfcSession::tick();
}