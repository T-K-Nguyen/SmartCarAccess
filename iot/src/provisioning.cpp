#include <Arduino.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include <Preferences.h>
#include "../include/nfc.h"
#include "../include/provisioning.h"

namespace {
  mbedtls_pk_context keypair;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  Preferences prefs;
  bool s_ready = false;

  const char *kPrefsNs = "prov";
  const char *kKeyPem = "ec_priv";
  const char *kCertPem = "cert";
  const char *kTagUid = "tag";   // legacy single-tag storage
  const char *kTagsBlob = "tags"; // multi-tag blob storage

  static const uint8_t MAX_TAGS = 8;
  static const uint8_t MAX_UID_LEN = 8; // typical 4/7 bytes; allow up to 8

  struct TagList {
    uint8_t count;
    uint8_t len[MAX_TAGS];
    uint8_t uid[MAX_TAGS][MAX_UID_LEN];
  };

  void printUidHex(const uint8_t* uid, uint8_t len) {
    for (uint8_t i = 0; i < len; ++i) {
      if (uid[i] < 0x10) Serial.print("0");
      Serial.print(uid[i], HEX);
      if (i + 1 < len) Serial.print(":");
    }
  }

  bool loadTagList(TagList &out) {
    memset(&out, 0, sizeof(out));
    prefs.begin(kPrefsNs, true);
    // Try multi-tag blob first
    uint8_t buf[1 + MAX_TAGS * (1 + MAX_UID_LEN)];
    size_t n = prefs.getBytes(kTagsBlob, buf, sizeof(buf));
    if (n > 0) {
      prefs.end();
      uint8_t idx = 0;
      uint8_t count = buf[idx++];
      if (count > MAX_TAGS) count = MAX_TAGS;
      out.count = count;
      for (uint8_t i = 0; i < count && idx < n; ++i) {
        uint8_t L = buf[idx++];
        if (L > MAX_UID_LEN) L = MAX_UID_LEN;
        out.len[i] = L;
        for (uint8_t j = 0; j < L && idx < n; ++j) {
          out.uid[i][j] = buf[idx++];
        }
      }
      return true;
    }
    // Fallback to legacy single "tag"
    uint8_t legacy[16];
    size_t legacyLen = prefs.getBytes(kTagUid, legacy, sizeof(legacy));
    prefs.end();
    if (legacyLen > 0 && legacyLen <= MAX_UID_LEN) {
      out.count = 1;
      out.len[0] = (uint8_t)legacyLen;
      memcpy(out.uid[0], legacy, legacyLen);
      return true;
    }
    return false;
  }

  void saveTagList(const TagList &in) {
    uint8_t buf[1 + MAX_TAGS * (1 + MAX_UID_LEN)];
    uint8_t idx = 0;
    uint8_t count = in.count > MAX_TAGS ? MAX_TAGS : in.count;
    buf[idx++] = count;
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t L = in.len[i] > MAX_UID_LEN ? MAX_UID_LEN : in.len[i];
      buf[idx++] = L;
      memcpy(&buf[idx], in.uid[i], L);
      idx += L;
    }
    prefs.begin(kPrefsNs, false);
    prefs.putBytes(kTagsBlob, buf, idx);
    // Remove legacy key to avoid confusion
    prefs.remove(kTagUid);
    prefs.end();
  }

  String s_deviceCert;

  // helper to generate or load ECC keypair
  bool ensureKeypair() {
    prefs.begin(kPrefsNs, false);
    String privPem = prefs.getString(kKeyPem, "");
    prefs.end();

    mbedtls_pk_init(&keypair);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const char *pers = "prov";
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char*)pers, strlen(pers));

    if (privPem.length() > 0) {
      // load existing key
      if (mbedtls_pk_parse_key(&keypair,
            (const unsigned char*)privPem.c_str(), privPem.length()+1,
            nullptr, 0) == 0) {
        Serial.println("[Prov] Loaded existing ECC keypair");
        return true;
      }
    }

    // generate new keypair
    if (mbedtls_pk_setup(&keypair,
          mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
      return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
          mbedtls_pk_ec(keypair), mbedtls_ctr_drbg_random, &drbg) != 0)
      return false;

    // export PEM (use static buffer to avoid large stack allocation)
    static unsigned char buf[1600];
    memset(buf, 0, sizeof(buf));
    if (mbedtls_pk_write_key_pem(&keypair, buf, sizeof(buf)) == 0) {
      prefs.begin(kPrefsNs, false);
      prefs.putString(kKeyPem, (char*)buf);
      prefs.end();
      Serial.println("[Prov] Generated new ECC keypair and saved to NVS");
    }
    return true;
  }
}

namespace Provisioning {

void begin() {
  if (ensureKeypair()) s_ready = true;
  else Serial.println("[Prov] Failed to init ECC keypair");
}

bool isProvisioned() {
  prefs.begin(kPrefsNs, true);
  bool ok = prefs.isKey(kCertPem);
  prefs.end();
  return ok;
}

void printInfo() {
  if (!s_ready) return;
  // Print device public key fingerprint (SHA-256 of DER pubkey)
  unsigned char pubbuf[800];
  memset(pubbuf, 0, sizeof(pubbuf));
  mbedtls_pk_write_pubkey_pem(&keypair, pubbuf, sizeof(pubbuf));
  // Compute SHA-256 on PEM as a quick fingerprint (ok for debug)
  unsigned char hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, pubbuf, strlen((char*)pubbuf));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  Serial.print("[Prov] Device PubKey FPR: ");
  for (int i = 0; i < 32; ++i) {
    if (hash[i] < 0x10) Serial.print("0");
    Serial.print(hash[i], HEX);
    if (i + 1 < 32) Serial.print(":");
  }
  Serial.println();
  Serial.println("[Prov] ECC key loaded. Cert stored: ");
  prefs.begin(kPrefsNs, true);
  String cert = prefs.getString(kCertPem, "(none)");
  prefs.end();
  Serial.println(cert);
  TagList tl;
  if (loadTagList(tl) && tl.count > 0) {
    for (uint8_t i = 0; i < tl.count; ++i) {
      Serial.print("[Prov] Authorized Tag #"); Serial.print(i+1); Serial.print(": ");
      printUidHex(tl.uid[i], tl.len[i]);
      Serial.println();
    }
  } else {
    Serial.println("[Prov] Authorized Tags: (none)");
  }
}

// This simulates provisioning via NFC: ECU sends its public key,
// expects to receive phone cert in response, and saves it.
void runNfcProvisioning() {
  if (!NFCMod::isReady() || !s_ready) {
    Serial.println("[Prov] NFC or ECC not ready");
    return;
  }

  // Compose small provisioning message (public key PEM truncated)
  static unsigned char pubbuf[800];
  memset(pubbuf, 0, sizeof(pubbuf));
  mbedtls_pk_write_pubkey_pem(&keypair, pubbuf, sizeof(pubbuf));

  Serial.println("[Prov] Bring phone close to provision ECU...");

  // Normally here you would use PN532 to push NDEF with pubkey
  // For now, just log and wait for tag UID as trigger
  // Wait until a tag is seen or timeout (10s)
  unsigned long t0 = millis();
  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool gotTag = false;
  while (millis() - t0 < 10000) {
    if (NFCMod::getLastTag(uid, &uidLen)) {
      Serial.print("[Prov] Captured tag UID: ");
      for (uint8_t i = 0; i < uidLen; ++i) {
        if (uid[i] < 0x10) Serial.print("0");
        Serial.print(uid[i], HEX);
        if (i + 1 < uidLen) Serial.print(":");
      }
      Serial.println();
      gotTag = true;
      break;
    }
    delay(100);
  }

  if (gotTag) {
    // In a real flow, read phone cert from NDEF, validate, and store
    prefs.begin(kPrefsNs, false);
    prefs.putString(kCertPem, "DummyPhoneCertBase64==");
    prefs.end();
    // Add this tag to the authorized list (avoid duplicates)
    TagList tl; loadTagList(tl);
    bool exists = false;
    for (uint8_t i = 0; i < tl.count; ++i) {
      if (tl.len[i] == uidLen && memcmp(tl.uid[i], uid, uidLen) == 0) { exists = true; break; }
    }
    if (!exists && tl.count < MAX_TAGS && uidLen <= MAX_UID_LEN) {
      tl.len[tl.count] = uidLen;
      memcpy(tl.uid[tl.count], uid, uidLen);
      tl.count++;
      saveTagList(tl);
      Serial.print("[Prov] Stored new authorized tag: ");
      printUidHex(uid, uidLen);
      Serial.println();
    }
    Serial.println("[Prov] Provisioning complete; phone cert saved.");
  } else {
    Serial.println("[Prov] No NFC tag detected during provisioning window; skipping save.");
  }
}

bool storeAuthorizedTag(const uint8_t* uid, uint8_t len) {
  if (!uid || len == 0 || len > 16) return false;
  TagList tl; loadTagList(tl);
  if (len > MAX_UID_LEN) return false;
  // check duplicate
  for (uint8_t i = 0; i < tl.count; ++i) {
    if (tl.len[i] == len && memcmp(tl.uid[i], uid, len) == 0) return true;
  }
  if (tl.count >= MAX_TAGS) return false;
  tl.len[tl.count] = len;
  memcpy(tl.uid[tl.count], uid, len);
  tl.count++;
  saveTagList(tl);
  return true;
}

bool isTagAuthorized(const uint8_t* uid, uint8_t len) {
  if (!uid || len == 0 || len > 16) return false;
  TagList tl; if (!loadTagList(tl) || tl.count == 0) return false;
  for (uint8_t i = 0; i < tl.count; ++i) {
    if (tl.len[i] == len && memcmp(tl.uid[i], uid, len) == 0) return true;
  }
  return false;
}

bool hasAuthorizedTag() {
  TagList tl; return loadTagList(tl) && tl.count > 0;
}

bool addAuthorizedTag(const uint8_t* uid, uint8_t len) {
  return storeAuthorizedTag(uid, len);
}

bool removeAuthorizedTag(const uint8_t* uid, uint8_t len) {
  if (!uid || len == 0 || len > MAX_UID_LEN) return false;
  TagList tl; if (!loadTagList(tl) || tl.count == 0) return false;
  uint8_t out = 0;
  for (uint8_t i = 0; i < tl.count; ++i) {
    if (!(tl.len[i] == len && memcmp(tl.uid[i], uid, len) == 0)) {
      // keep
      tl.len[out] = tl.len[i];
      memcpy(tl.uid[out], tl.uid[i], tl.len[i]);
      out++;
    }
  }
  tl.count = out;
  saveTagList(tl);
  return true;
}

void listAuthorizedTags() {
  TagList tl; if (!loadTagList(tl) || tl.count == 0) {
    Serial.println("[Prov] Authorized Tags: (none)");
    return;
  }
  for (uint8_t i = 0; i < tl.count; ++i) {
    Serial.print("[Prov] Authorized Tag #"); Serial.print(i+1); Serial.print(": ");
    printUidHex(tl.uid[i], tl.len[i]);
    Serial.println();
  }
}

void clearKeys() {
  prefs.begin(kPrefsNs, false);
  prefs.remove(kKeyPem);
  prefs.remove(kCertPem);
  prefs.end();
  Serial.println("[Prov] Cleared device keypair and cert.");
}

void clearAll() {
  prefs.begin(kPrefsNs, false);
  prefs.remove(kKeyPem);
  prefs.remove(kCertPem);
  prefs.remove(kTagsBlob);
  prefs.remove(kTagUid); // legacy
  prefs.end();
  Serial.println("[Prov] Cleared all provisioning data (keys, cert, tags).");
}

}
