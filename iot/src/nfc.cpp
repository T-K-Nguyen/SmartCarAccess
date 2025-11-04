// Minimal PN532 bring-up with simple polling (no provisioning)
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "../include/nfc.h"
#include "../include/provisioning.h"
// FreeRTOS for task scheduling
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
  const int I2C_SDA_PIN = 11;
  const int I2C_SCL_PIN = 12;
  const int PN532_IRQ_PIN = -1;
  const int PN532_RST_PIN = -1;
  Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RST_PIN);

  bool s_nfcReady = false;
  // simple last-tag cache
  uint8_t s_lastUid[7];
  uint8_t s_lastUidLen = 0;
  volatile bool s_uidReady = false;

  // polling task handle
  static TaskHandle_t s_task = nullptr;

  void TaskNFC(void* pv) {
    uint8_t uid[7];
    uint8_t uidLen = 0;
    // de-dup printing
    uint8_t lastPrintUid[7] = {0};
    uint8_t lastPrintLen = 0;
    uint32_t lastPrintAt = 0;
    const uint32_t cooldownMs = 1500;
    for (;;) {
      if (!s_nfcReady) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
      bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      if (found) {
        bool same = (uidLen == lastPrintLen) && (memcmp(uid, lastPrintUid, uidLen) == 0);
        bool cool = (millis() - lastPrintAt) >= cooldownMs;
        if (!same || cool) {
          Serial.print("[NFC] Tag UID: ");
          for (uint8_t i = 0; i < uidLen; ++i) {
            if (uid[i] < 0x10) Serial.print("0");
            Serial.print(uid[i], HEX);
            if (i + 1 < uidLen) Serial.print(":");
          }
          Serial.println();
          memcpy(lastPrintUid, uid, uidLen);
          lastPrintLen = uidLen;
          lastPrintAt = millis();
        }
        // cache for external consumers
        if (uidLen <= sizeof(s_lastUid)) {
          memcpy(s_lastUid, uid, uidLen);
          s_lastUidLen = uidLen;
          s_uidReady = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
  }
}

namespace NFCMod {
  void begin() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(1000);
    Wire.setClock(100000);
    delay(50);

    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
      Serial.println("[NFC] PN532 not found. Check wiring and address (default 0x24).");
      s_nfcReady = false;
      return;
    }

    Serial.printf("[NFC] PN532 OK. IC: 0x%02X, Ver: %u.%u\n",
                  (unsigned)((versiondata >> 24) & 0xFF),
                  (unsigned)((versiondata >> 16) & 0xFF),
                  (unsigned)((versiondata >> 8) & 0xFF));
    // Configure SAM (required for reading passive targets/APDUs later)
    nfc.SAMConfig();
    s_nfcReady = true;
  }

  void startTask() {
    if (!s_task) {
      xTaskCreate(TaskNFC, "NFC Poll", 4096, nullptr, 2, &s_task);
    }
  }

  bool isReady() { return s_nfcReady; }

  // Return the most recently seen tag UID once
  bool getLastTag(uint8_t* uid, uint8_t* uidLen) {
    if (!uid || !uidLen) return false;
    if (!s_uidReady) return false;
    uint8_t n = s_lastUidLen; if (n > 7) n = 7;
    memcpy(uid, s_lastUid, n);
    *uidLen = n;
    s_uidReady = false;
    return true;
  }

  // Build SELECT AID APDU
  static void buildSelectAid(const uint8_t* aid, size_t aidLen, uint8_t* out, uint8_t* outLen) {
    uint8_t idx = 0;
    out[idx++] = 0x00; // CLA
    out[idx++] = 0xA4; // INS
    out[idx++] = 0x04; // P1 (by AID)
    out[idx++] = 0x00; // P2
    out[idx++] = (uint8_t)aidLen; // Lc
    for (size_t i = 0; i < aidLen; ++i) out[idx++] = aid[i];
    out[idx++] = 0x00; // Le
    *outLen = idx;
  }

  // Build GET_INFO APDU (INS 0xCA) with vehicleId (<=8) + nonce(16)
  static void buildGetInfo(const uint8_t* veh, size_t vehLen, const uint8_t* nonce, size_t nonceLen, uint8_t* out, uint8_t* outLen) {
    uint8_t idx = 0;
    out[idx++] = 0x00; // CLA
    out[idx++] = 0xCA; // INS (custom)
    out[idx++] = 0x00; // P1
    out[idx++] = 0x00; // P2
    out[idx++] = (uint8_t)(vehLen + nonceLen); // Lc
    for (size_t i = 0; i < vehLen; ++i) out[idx++] = veh[i];
    for (size_t i = 0; i < nonceLen; ++i) out[idx++] = nonce[i];
    *outLen = idx;
  }

  static bool apduOk(const uint8_t* resp, uint8_t rlen) {
    return rlen >= 2 && resp[rlen-2] == 0x90 && resp[rlen-1] == 0x00;
  }

  bool runProvisioningSM(const uint8_t* aid, size_t aidLen, uint32_t overallTimeoutMs, ProvData* out, char* errBuf, size_t errBufLen) {
    if (!aid || aidLen == 0 || aidLen > 16 || !out) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "bad args");
      return false;
    }

    uint32_t tStart = millis();
    auto timeLeft = [&]() -> uint32_t {
      uint32_t elapsed = millis() - tStart;
      return (elapsed >= overallTimeoutMs) ? 0 : (overallTimeoutMs - elapsed);
    };

    // S1: INIT_NFC_FOR_PROVISION — suspend polling, re-init PN532
  bool suspended = false;
  if (s_task) { vTaskSuspend(s_task); suspended = true; }
    // Keep I2C params stable
    Wire.setTimeOut(1000);
    Wire.setClock(100000);
    vTaskDelay(pdMS_TO_TICKS(10));
    nfc.begin();
    uint32_t versiondata = 0;
    for (int i = 0; i < 2 && !versiondata; ++i) {
      versiondata = nfc.getFirmwareVersion();
      if (!versiondata) vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (!versiondata) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "PN532 not found");
      nfc.SAMConfig();
      if (s_task && suspended) vTaskResume(s_task);
      return false;
    }
    nfc.setPassiveActivationRetries(0xFF);
    nfc.SAMConfig();

    // S2: WAIT_FOR_PHONE_TAP — loop until phone present or timeout
    {
      uint8_t uid[10]; uint8_t uidLen = 0; bool present = false;
      while (timeLeft() > 0) {
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen)) { present = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
  if (!present) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "no phone"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
    }

    // S3: ISODEP_ACTIVATION — handled implicitly by inDataExchange after tag presence

    // S4: SEND_SELECT_AID
    {
      uint8_t apdu[32]; uint8_t alen = 0; buildSelectAid(aid, aidLen, apdu, &alen);
      uint8_t resp[255]; uint8_t rlen = sizeof(resp);
      bool ok = nfc.inDataExchange(apdu, alen, resp, &rlen);
      if ((!ok || !apduOk(resp, rlen)) && timeLeft() > 0) {
        // retry once
        vTaskDelay(pdMS_TO_TICKS(50)); rlen = sizeof(resp);
        ok = nfc.inDataExchange(apdu, alen, resp, &rlen);
      }
  if (!ok || !apduOk(resp, rlen)) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "SELECT AID failed"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
    }

    // S5: SEND_CHALLENGE — vehicle fingerprint (8) + nonce (16)
    uint8_t veh[8]; size_t vehLen = 0; {
      uint8_t fpr[32]; size_t F = sizeof(fpr);
      if (Provisioning::getDevicePubKeyFingerprint(fpr, &F) && F >= 8) { memcpy(veh, fpr, 8); vehLen = 8; }
    }
    uint8_t nonce[16]; Provisioning::randomBytes(nonce, sizeof(nonce));
    {
      uint8_t apdu[64]; uint8_t alen = 0; buildGetInfo(vehLen?veh:nullptr, vehLen, nonce, sizeof(nonce), apdu, &alen);
      uint8_t resp[255]; uint8_t rlen = sizeof(resp);
  if (!nfc.inDataExchange(apdu, alen, resp, &rlen)) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "GET_INFO xchg fail"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
  if (!apduOk(resp, rlen)) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "GET_INFO SW not ok"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }

  // S6: RECEIVE_CREDENTIALS — parse payload (rlen includes SW)
  if (rlen < 2 + 1 + 65 + 2) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "RAPDU too short"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
  uint8_t datalen = rlen - 2; const uint8_t* p = resp; size_t idx = 0;
  uint8_t rawKeyIdLen = p[idx++];
  uint8_t copyKeyIdLen = rawKeyIdLen;
  if (copyKeyIdLen >= sizeof(out->keyId)) copyKeyIdLen = sizeof(out->keyId) - 1;
  memcpy(out->keyId, p + idx, copyKeyIdLen); out->keyId[copyKeyIdLen] = '\0'; idx += rawKeyIdLen;
  if (idx + 65 + 2 > datalen) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "layout error"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
      memcpy(out->pubKey65, p + idx, 65); idx += 65;
      out->certLen = (uint16_t)(p[idx] | (p[idx+1] << 8)); idx += 2;
  if (idx + out->certLen > datalen || out->certLen > sizeof(out->cert)) { if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "cert too big"); nfc.SAMConfig(); if (s_task && suspended) vTaskResume(s_task); return false; }
      memcpy(out->cert, p + idx, out->certLen);
    }

    // Success up to S6
    if (s_task && suspended) vTaskResume(s_task);
    return true;
  }
  bool runHceProvisioningOnce(const uint8_t* aid, size_t aidLen, uint32_t timeoutMs) {
    (void)aid; (void)aidLen; (void)timeoutMs; return false;
  }
}
