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

  // Run a single Android HCE provisioning attempt over ISO-DEP using APDUs.
  // Returns true on success after storing phone key info in Provisioning.
  // aid points to the application identifier to SELECT (e.g., {0xF0,0x01,0x02,0x03,0x04,0x05}).
  bool runHceProvisioningOnce(const uint8_t* aid, size_t aidLen, uint32_t timeoutMs = 8000);
}
