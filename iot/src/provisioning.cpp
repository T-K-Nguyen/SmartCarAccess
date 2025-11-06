#include <Arduino.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include <Preferences.h>
#include <mbedtls/x509_crt.h>
#include "nfc.h"
#include "provisioning.h"

namespace {
  mbedtls_pk_context keypair;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  Preferences prefs;
  bool s_ready = false;

  const char *kPrefsNs = "prov";
  const char *kKeyPem = "ec_priv";
  const char *kCertPem = "cert";       // device's own certificate (optional)
  const char *kKeyId   = "key_id";     // phone key identifier (optional)
  const char *kPhonePubRaw = "phone_pub_raw"; // phone public key raw (65 bytes)
  const char *kCertChain = "phone_cert_chain";   // phone certificate chain (optional)
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
    // Try multi-tag blob with header first: 'TAG1' + count + entries
    uint8_t buf[4 + 1 + MAX_TAGS * (1 + MAX_UID_LEN)];
    size_t n = 0;
    if (prefs.isKey(kTagsBlob)) {
      n = prefs.getBytes(kTagsBlob, buf, sizeof(buf));
    }
    if (n > 0) {
      prefs.end();
      uint8_t idx = 0;
      bool hasHeader = (n >= 5 && buf[0] == 'T' && buf[1] == 'A' && buf[2] == 'G' && buf[3] == '1');
      if (hasHeader) idx += 4; // skip header
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
    size_t legacyLen = 0;
    if (prefs.isKey(kTagUid)) {
      legacyLen = prefs.getBytes(kTagUid, legacy, sizeof(legacy));
    }
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
    uint8_t buf[4 + 1 + MAX_TAGS * (1 + MAX_UID_LEN)];
    uint8_t idx = 0;
    // Add header 'TAG1'
    buf[idx++] = 'T'; buf[idx++] = 'A'; buf[idx++] = 'G'; buf[idx++] = '1';
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
  String cert;
  if (prefs.isKey(kCertPem)) {
    cert = prefs.getString(kCertPem, "");
  }
  String keyId;
  if (prefs.isKey(kKeyId)) {
    keyId = prefs.getString(kKeyId, "");
  }
  prefs.end();
  Serial.println(cert.length() ? cert : String("(none)"));
  Serial.print("[Prov] phone keyId: "); Serial.println(keyId.length() ? keyId : String("(no keyId)"));
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

void runNfcProvisioning() {
  // S0 — SYSTEM_IDLE: entry when admin requests provisioning
  if (!NFCMod::isReady() || !s_ready) {
    Serial.println("[Prov] NFC or ECC not ready");
    return;
  }

  // AID must match the Android HCE service (7 bytes for Android compatibility)
  static const uint8_t HCE_AID[] = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0F };
  const uint32_t overallTimeoutMs = 15000;

  Serial.println("[Prov] Admin requested provisioning. Bring phone close...");
  
  // Debug: Print memory status and AID we're using
  Serial.printf("[Prov] Free heap: %d bytes\n", esp_get_free_heap_size());
  Serial.printf("[Prov] Using AID (len=%d): ", sizeof(HCE_AID));
  for (int i = 0; i < sizeof(HCE_AID); i++) {
    Serial.printf("%02X ", HCE_AID[i]);
  }
  Serial.println();

  // Keep reader in provisioning mode until success
  NFCMod::setProvisionHold(true);
  
  // Give NFC polling task time to release mutex and suspend properly
  Serial.println("[Prov] Waiting for NFC task to suspend...");
  vTaskDelay(pdMS_TO_TICKS(500)); // Longer delay to ensure proper suspension
  
  for (;;) {
    // Run NFC-side state machine S1..S6
    NFCMod::ProvData pd{};
    char err[64] = {0};
    if (!NFCMod::runProvisioningSM(HCE_AID, sizeof(HCE_AID), overallTimeoutMs, &pd, err, sizeof err)) {
      // Stay in provisioning mode; continue waiting for phone
      if (err[0]) {
        Serial.print("[Prov] NFC session failed: "); Serial.println(err);
      } else {
        Serial.println("[Prov] NFC session retry...");
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    // S7 — VERIFY_DATA
  //  - basic checks: pubkey format, lengths
  if (pd.pubKey65[0] != 0x04) {
    Serial.println("[Prov] Invalid phone public key format (expected uncompressed 65 bytes). Remove phone and re-tap.");
    NFCMod::resetForProvisionRetry();
    vTaskDelay(pdMS_TO_TICKS(150));
    // verification failed; continue waiting
    continue;
  }
  // Reject duplicate keyId if already stored
  {
    char existing[64]; size_t L = sizeof(existing);
    if (getPhoneKeyId(existing, &L)) {
      if (strncmp(existing, pd.keyId, sizeof(existing)) == 0) {
        Serial.println("[Prov] Duplicate keyId detected; aborting.");
        // Ensure polling resumes if we abort early
        NFCMod::setProvisionHold(false);
        return;
      }
    }
  }

  // TODO: Optional chain validation / replay protection via signature

  // S8 — COMMIT_TO_FLASH (attempt atomic-ish commit; rollback on failure)
    bool committed = false;
    do {
      if (!setPhonePublicKeyRaw65(pd.pubKey65, 65)) break;
      if (pd.keyId[0]) {
        if (!setPhoneKeyId(pd.keyId)) { clearKeys(); break; }
      }
      if (pd.certLen > 0) {
        // ensure string termination for PEM
        char *buf = (char*)malloc(pd.certLen + 1);
        if (!buf) { clearKeys(); break; }
        memcpy(buf, pd.cert, pd.certLen); buf[pd.certLen] = '\0';
        bool ok = setPhoneCertChain(buf);
        free(buf);
        if (!ok) { clearKeys(); break; }
      }
      committed = true;
    } while(false);

    if (!committed) {
      Serial.println("[Prov] Commit to flash failed; rolled back. Remove phone and re-tap...");
      NFCMod::resetForProvisionRetry();
      vTaskDelay(pdMS_TO_TICKS(150));
      continue;
    }

    // S9 — SUCCESS
    Serial.print("[Prov] Provisioned keyId='"); Serial.print(pd.keyId); Serial.print("'\n");
    Serial.println("[Prov] Provisioning success. Polling resumed.");
    break; // exit loop on success
  }
  NFCMod::setProvisionHold(false);
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
// Add Phase B crypto helper implementations
namespace Provisioning {

bool signWithDeviceKey(const uint8_t* data, size_t dataLen, uint8_t* sigOut, size_t* sigLen) {
  if (!s_ready || !data || dataLen == 0 || !sigOut || !sigLen) return false;
  // Hash data with SHA-256
  unsigned char hash[32];
  mbedtls_sha256_context c; mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, data, dataLen);
  mbedtls_sha256_finish(&c, hash);
  mbedtls_sha256_free(&c);

  size_t outLen = 0;
  int rc = mbedtls_pk_sign(&keypair, MBEDTLS_MD_SHA256,
                           hash, sizeof(hash),
                           sigOut, &outLen,
                           mbedtls_ctr_drbg_random, &drbg);
  if (rc != 0) return false;
  *sigLen = outLen;
  return true;
}

static bool loadPhonePk(mbedtls_pk_context &pk) {
  // Preferred: load raw 65-byte public key
  prefs.begin(kPrefsNs, true);
  size_t n = prefs.getBytesLength(kPhonePubRaw);
  bool hasRaw = n == 65;
  String chainStr;
  if (!hasRaw && prefs.isKey(kCertChain)) chainStr = prefs.getString(kCertChain, "");
  prefs.end();

  mbedtls_pk_init(&pk);
  if (hasRaw) {
    uint8_t raw[65];
    prefs.begin(kPrefsNs, true);
    prefs.getBytes(kPhonePubRaw, raw, sizeof(raw));
    prefs.end();
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
      mbedtls_pk_free(&pk); return false;
    }
    mbedtls_ecp_keypair* eckey = mbedtls_pk_ec(pk);
    if (mbedtls_ecp_group_load(&eckey->grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
      mbedtls_pk_free(&pk); return false;
    }
    if (mbedtls_ecp_point_read_binary(&eckey->grp, &eckey->Q, raw, sizeof(raw)) != 0) {
      mbedtls_pk_free(&pk); return false;
    }
    return true;
  }

  // Fallback: parse X509 cert chain and take leaf public key
  if (chainStr.length() > 0) {
    mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
    int rc = mbedtls_x509_crt_parse(&crt,
      (const unsigned char*)chainStr.c_str(), chainStr.length()+1);
    if (rc == 0 && crt.pk.pk_info) {
      // Copy the pk into our context
      if (mbedtls_pk_setup(&pk, crt.pk.pk_info) == 0) {
        // Note: shallow copy is not safe; instead export pub and re-import
        unsigned char buf[800]; memset(buf, 0, sizeof(buf));
        if (mbedtls_pk_write_pubkey_pem(&crt.pk, buf, sizeof(buf)) == 0 &&
            mbedtls_pk_parse_public_key(&pk, buf, strlen((char*)buf)+1) == 0) {
          mbedtls_x509_crt_free(&crt);
          return true;
        }
      }
    }
    mbedtls_x509_crt_free(&crt);
  }
  mbedtls_pk_free(&pk);
  return false;
}

bool verifyWithPhoneKey(const uint8_t* data, size_t dataLen, const uint8_t* sig, size_t sigLen) {
  if (!data || dataLen == 0 || !sig || sigLen == 0) return false;
  mbedtls_pk_context pk;
  if (!loadPhonePk(pk)) return false;

  unsigned char hash[32];
  mbedtls_sha256_context c; mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, data, dataLen);
  mbedtls_sha256_finish(&c, hash);
  mbedtls_sha256_free(&c);

  int rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                             hash, sizeof(hash), sig, sigLen);
  mbedtls_pk_free(&pk);
  return rc == 0;
}

bool hasPhonePublicKey() {
  prefs.begin(kPrefsNs, true);
  bool ok = (prefs.getBytesLength(kPhonePubRaw) == 65) || prefs.isKey(kCertChain);
  prefs.end();
  return ok;
}

bool setPhonePublicKey(const char* pem) {
  // For compatibility, store PEM into phone_cert_chain if caller chooses to use this API
  if (!pem) return false;
  size_t L = strlen(pem);
  if (L < 32) return false; // sanity
  prefs.begin(kPrefsNs, false);
  bool ok = prefs.putString(kCertChain, pem) > 0;
  prefs.end();
  if (ok) Serial.println("[Prov] Stored phone public key (PEM) into cert chain slot in NVS.");
  return ok;
}

bool setPhonePublicKeyRaw65(const uint8_t* uncompressed65, size_t len) {
  if (!uncompressed65 || len != 65 || uncompressed65[0] != 0x04) return false;
  prefs.begin(kPrefsNs, false);
  bool ok = prefs.putBytes(kPhonePubRaw, uncompressed65, len) == len;
  prefs.end();
  if (ok) Serial.println("[Prov] Stored phone public key (raw 65 bytes) in NVS.");
  return ok;
}

bool setPhoneKeyId(const char* keyId) {
  if (!keyId) return false;
  prefs.begin(kPrefsNs, false);
  bool ok = prefs.putString(kKeyId, keyId) > 0;
  prefs.end();
  if (ok) Serial.println("[Prov] Stored phone keyId in NVS.");
  return ok;
}

bool getPhoneKeyId(char* out, size_t* inoutLen) {
  if (!out || !inoutLen || *inoutLen == 0) return false;
  prefs.begin(kPrefsNs, true);
  String id = prefs.getString(kKeyId, "");
  prefs.end();
  size_t L = id.length();
  if (L == 0 || L + 1 > *inoutLen) return false;
  memcpy(out, id.c_str(), L + 1);
  *inoutLen = L;
  return true;
}

bool setPhoneCertChain(const char* certPem) {
  if (!certPem) return false;
  prefs.begin(kPrefsNs, false);
  bool ok = prefs.putString(kCertChain, certPem) > 0;
  prefs.end();
  if (ok) Serial.println("[Prov] Stored phone cert chain in NVS.");
  return ok;
}

bool hasPhoneCertChain() {
  prefs.begin(kPrefsNs, true);
  bool ok = prefs.isKey(kCertChain);
  prefs.end();
  return ok;
}

bool getDevicePubKeyFingerprint(uint8_t* out, size_t* inoutLen) {
  if (!out || !inoutLen || *inoutLen < 32) return false;
  if (!s_ready) return false;
  // Compute SHA-256 on PEM public key (demo fingerprint)
  unsigned char pubbuf[800]; memset(pubbuf, 0, sizeof(pubbuf));
  if (mbedtls_pk_write_pubkey_pem(&keypair, pubbuf, sizeof(pubbuf)) != 0) return false;
  unsigned char hash[32];
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, pubbuf, strlen((char*)pubbuf));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  memcpy(out, hash, 32);
  *inoutLen = 32;
  return true;
}

void randomBytes(uint8_t* out, size_t len) {
  if (!out || len == 0) return;
  // Use DRBG seeded in ensureKeypair
  mbedtls_ctr_drbg_random(&drbg, out, len);
}

}
