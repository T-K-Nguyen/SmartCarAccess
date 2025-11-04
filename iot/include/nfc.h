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

  // Provisioning state-machine helper (S1-S6): handles NFC-side session and APDUs, returns creds.
  struct ProvData {
    char keyId[64];
    uint8_t pubKey65[65];
    uint16_t certLen;
    uint8_t cert[512]; // max cert blob we accept via NFC
  };

  // Runs S1..S6: suspend polling, (re)init PN532, wait for phone, SELECT AID, send challenge, receive creds.
  // On success, fills out and returns true; on failure, resumes polling and returns false.
  bool runProvisioningSM(const uint8_t* aid, size_t aidLen, uint32_t overallTimeoutMs, ProvData* out, char* errBuf = nullptr, size_t errBufLen = 0);

  // Legacy placeholder (no-op now)
  bool runHceProvisioningOnce(const uint8_t* aid, size_t aidLen, uint32_t timeoutMs = 8000);
}
