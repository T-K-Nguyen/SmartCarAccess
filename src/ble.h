#pragma once

namespace BLEMod {
  // Initialize BLE (GATT server, advertising)
  void begin();

  // Status helpers
  bool isStarted();
  const char* deviceName();
}
