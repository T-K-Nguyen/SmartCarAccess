#pragma once
#include <Arduino.h>
#include <PN532_HSU.h>
#include <PN532.h>

namespace NfcSession {
  void begin(HardwareSerial& uart, int rxPin, int txPin, uint32_t baud = 115200);
  void tick();
}
