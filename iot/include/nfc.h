#pragma once

namespace NFCMod {
  // Initialize PN532 and SPI bus
  void begin();
  // Start a FreeRTOS task that polls for tags and prints UIDs
  void startTask();
  // Suspend/resume polling task for provisioning window; keeps the reader in provision mode
  void setProvisionHold(bool enabled);

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

  // Check if detected tag during provisioning is actually our phone (non-blocking)
  bool checkForProvisioningPhone(const uint8_t* aid, size_t aidLen);

  // Light reset of PN532 between provisioning attempts to avoid stale ISO-DEP sessions
  void resetForProvisionRetry();
}
