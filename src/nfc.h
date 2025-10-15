#pragma once

namespace NFCMod {
  // Initialize PN532 and I2C bus
  void begin();
  // Start a FreeRTOS task that polls for tags and prints UIDs
  void startTask();

  // Status helpers
  bool isReady();
}
