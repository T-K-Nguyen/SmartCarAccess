#pragma once

namespace NFCMod {
  // Initialize PN532 and I2C bus
  void begin();
  // Start a FreeRTOS task that polls for tags and prints UIDs
  void startTask();

  // Status helpers
  bool isReady();

  // Get the most recently seen tag UID (non-blocking).
  // Returns true if a new UID was captured since the last call and writes it to uid/uidLen (max 7 bytes).
  bool getLastTag(uint8_t* uid, uint8_t* uidLen);
}
