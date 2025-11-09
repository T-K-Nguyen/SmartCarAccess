#include <Arduino.h>
#include <Preferences.h>
#include <PN532.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include "provisioning_phase.h"

namespace {
  Preferences prefs;
  const char* kNs = "prov";
  const char* kKeyPem = "ec_priv";
  const char* kKeyId  = "key_id";

  mbedtls_pk_context keypair;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool keysReady = false;

  // Small hex helper
  String toHex(const uint8_t* b, size_t n) {
    String s; s.reserve(n*2);
    for (size_t i=0;i<n;i++){ char buf[3]; sprintf(buf, "%02X", b[i]); s += buf; }
    return s;
  }

  bool ensureKeypair() {
    prefs.begin(kNs, false);
    String privPem = prefs.getString(kKeyPem, "");
    prefs.end();

    mbedtls_pk_init(&keypair);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const char *pers = "provA";
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char*)pers, strlen(pers));

    if (privPem.length() > 0) {
      if (mbedtls_pk_parse_key(&keypair,
            (const unsigned char*)privPem.c_str(), privPem.length()+1,
            nullptr, 0) == 0) {
        Serial.println("[PhaseA] Loaded existing ECC keypair");
        return true;
      }
    }
    if (mbedtls_pk_setup(&keypair, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(keypair), mbedtls_ctr_drbg_random, &drbg) != 0) return false;

    static unsigned char buf[1600]; memset(buf, 0, sizeof(buf));
    if (mbedtls_pk_write_key_pem(&keypair, buf, sizeof(buf)) == 0) {
      prefs.begin(kNs, false);
      prefs.putString(kKeyPem, (char*)buf);
      prefs.end();
      Serial.println("[PhaseA] Generated ECC keypair and saved to NVS");
    }
    return true;
  }
}

namespace ProvisioningPhase {

void begin() {
  keysReady = ensureKeypair();
  if (!keysReady) Serial.println("[PhaseA] ECC init failed (continuing without keys)");
}

bool isProvisioned() {
  prefs.begin(kNs, true);
  bool ok = prefs.isKey(kKeyId);
  prefs.end();
  return ok;
}

bool storeKeyIdHexIfEmpty(const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0) return false;
  if (isProvisioned()) return false;
  String hex = toHex(bytes, len);
  prefs.begin(kNs, false);
  bool ok = prefs.putString(kKeyId, hex) > 0;
  prefs.end();
  if (ok) Serial.printf("[PhaseA] Stored phone keyId as hex: %s\n", hex.c_str());
  return ok;
}

bool storeKeyIdAsciiIfEmpty(const char* ascii) {
  if (!ascii || !*ascii) return false;
  if (isProvisioned()) return false;
  prefs.begin(kNs, false);
  bool ok = prefs.putString(kKeyId, ascii) > 0;
  prefs.end();
  if (ok) Serial.printf("[PhaseA] Stored phone keyId (ascii): %s\n", ascii);
  return ok;
}

bool storeKeyIdAsciiForce(const char* ascii) {
  if (!ascii || !*ascii) return false;
  prefs.begin(kNs, false);
  bool ok = prefs.putString(kKeyId, ascii) > 0;
  prefs.end();
  if (ok) Serial.printf("[PhaseA] Force-stored phone keyId (ascii): %s\n", ascii);
  return ok;
}

void clearAll() {
  prefs.begin(kNs, false);
  prefs.remove(kKeyPem);
  prefs.remove(kKeyId);
  prefs.end();
}

// Minimal helper wrapping current working flow
bool runOnceWithHce(PN532& nfc, const uint8_t* aid, size_t aidLen, uint32_t waitMs) {
  if (!aid || aidLen == 0 || aidLen > 16) return false;
  uint32_t start = millis();

  // Wait for any target
  while (millis() - start < waitMs) {
    if (nfc.inListPassiveTarget()) break;
    delay(80);
  }
  if (millis() - start >= waitMs) {
    Serial.println("[PhaseA] No card during provisioning window");
    return false;
  }

  // Build SELECT AID
  uint8_t apdu[32]; uint8_t idx=0;
  apdu[idx++] = 0x00; apdu[idx++] = 0xA4; apdu[idx++] = 0x04; apdu[idx++] = 0x00;
  apdu[idx++] = (uint8_t)aidLen;
  for (size_t i=0;i<aidLen;i++) apdu[idx++] = aid[i];
  apdu[idx++] = 0x00; // Le

  uint8_t resp[64]; uint8_t rlen = sizeof(resp);
  if (!nfc.inDataExchange(apdu, idx, resp, &rlen)) {
    Serial.println("[PhaseA] SELECT failed");
    return false;
  }
  if (rlen < 2) {
    Serial.println("[PhaseA] SELECT response too short");
    return false;
  }
  uint8_t sw1 = resp[rlen-2], sw2 = resp[rlen-1];
  int payloadLen = rlen - 2;
  Serial.printf("[PhaseA] SELECT SW=%02X%02X payloadLen=%d\n", sw1, sw2, payloadLen);

  const uint8_t* payload = resp;
  size_t idLen = (payloadLen > 0) ? payloadLen : 0;

  // Try secondary GET UID like current demo
  if (sw1 == 0x90 && sw2 == 0x00) {
    uint8_t getUid[] = {0x00, 0xB0, 0x00, 0x00, 0x04};
    rlen = sizeof(resp);
    if (nfc.inDataExchange(getUid, sizeof(getUid), resp, &rlen) && rlen >= 2) {
      uint8_t sw1u = resp[rlen-2], sw2u = resp[rlen-1];
      int uidLen = rlen - 2;
      if (uidLen > 0 && sw1u == 0x90 && sw2u == 0x00) {
        payload = resp; idLen = uidLen;
        Serial.println("[PhaseA] Using UID APDU payload as keyId");
      }
    }
  }

  if (idLen == 0) {
    Serial.println("[PhaseA] No payload to store as keyId");
    return false;
  }
  return storeKeyIdHexIfEmpty(payload, idLen);
}

} // namespace
