#include "ccc_mailbox.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstring>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

namespace {
  Preferences prefs;
  const char* kNs = "ccc_dk";

  CCCMailbox::CCC_Mailbox g_mailbox{};
  bool g_loaded = false;
  mbedtls_entropy_context g_vsign_entropy;
  mbedtls_ctr_drbg_context g_vsign_drbg;
  bool g_vsign_rng_ready = false;

  bool isValidPub(const uint8_t* pub65) {
    return pub65 && pub65[0] == 0x04;
  }

  void tokenKey(uint8_t slotIndex, char* out, size_t outLen) {
    snprintf(out, outLen, "tok_%u", (unsigned)slotIndex);
  }

  void clearLegacyProvisioning() {
    prefs.begin("prov", false);
    prefs.clear();
    prefs.end();
  }

  void generateVehicleId(char out[9]) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    out[0] = 'V';
    out[1] = 'N';
    for (size_t i = 2; i < 8; ++i) {
      uint32_t r = esp_random();
      out[i] = kAlphabet[r % (sizeof(kAlphabet) - 1)];
    }
    out[8] = '\0';
  }

  bool generateVehicleKeypair(uint8_t outPub[65], uint8_t outPriv[32]) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);

    const char* pers = "ccc_vpub";
    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers));
    if (rc != 0) {
      mbedtls_ecp_point_free(&q);
      mbedtls_mpi_free(&d);
      mbedtls_ecp_group_free(&grp);
      mbedtls_ctr_drbg_free(&drbg);
      mbedtls_entropy_free(&entropy);
      return false;
    }

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) {
      mbedtls_ecp_point_free(&q);
      mbedtls_mpi_free(&d);
      mbedtls_ecp_group_free(&grp);
      mbedtls_ctr_drbg_free(&drbg);
      mbedtls_entropy_free(&entropy);
      return false;
    }

    rc = mbedtls_ecp_gen_keypair(&grp, &d, &q, mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      mbedtls_ecp_point_free(&q);
      mbedtls_mpi_free(&d);
      mbedtls_ecp_group_free(&grp);
      mbedtls_ctr_drbg_free(&drbg);
      mbedtls_entropy_free(&entropy);
      return false;
    }

    size_t olen = 0;
    rc = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                        &olen, outPub, 65);
    if (rc == 0) {
      rc = mbedtls_mpi_write_binary(&d, outPriv, 32);
    }

    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);

    return rc == 0 && olen == 65;
  }

  bool validateVehicleKeypair(const uint8_t pub[65], const uint8_t priv[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_point qCalc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_point_init(&qCalc);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&d, priv, 32);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &q, pub, 65);
    if (rc == 0) {
      rc = mbedtls_ecp_mul(&grp, &qCalc, &d, &grp.G, nullptr, nullptr);
    }

    unsigned char qBuf[65];
    size_t qLen = 0;
    if (rc == 0) {
      rc = mbedtls_ecp_point_write_binary(&grp, &qCalc, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &qLen, qBuf, sizeof(qBuf));
    }

    bool ok = (rc == 0 && qLen == 65 && memcmp(qBuf, pub, 65) == 0);

    mbedtls_ecp_point_free(&qCalc);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
  }

  void loadMailbox() {
    memset(&g_mailbox, 0, sizeof(g_mailbox));
    prefs.begin(kNs, true);

    String vId = prefs.getString("v_id", "");
    if (vId.length() == 8) {
      memcpy(g_mailbox.vehicle_id, vId.c_str(), 8);
      g_mailbox.vehicle_id[8] = '\0';
    }

    size_t vPubLen = prefs.getBytesLength("v_pub");
    if (vPubLen == 65) {
      prefs.getBytes("v_pub", g_mailbox.vehicle_pub, 65);
      g_mailbox.vehicle_pub_valid = isValidPub(g_mailbox.vehicle_pub);
    }

    size_t vPrivLen = prefs.getBytesLength("v_priv");
    if (vPrivLen == 32) {
      prefs.getBytes("v_priv", g_mailbox.vehicle_priv, 32);
      g_mailbox.vehicle_priv_valid = true;
    }

    size_t epPubLen = prefs.getBytesLength("ep_pub");
    if (epPubLen == 65) {
      prefs.getBytes("ep_pub", g_mailbox.endpoint_pub, 65);
      g_mailbox.endpoint_pub_valid = isValidPub(g_mailbox.endpoint_pub);
    }

    g_mailbox.signaling_bitmap = prefs.getUShort("sig_bmp", 0);
    g_mailbox.slot_bitmap = prefs.getUChar("slot_bmp", 0);

    for (uint8_t i = 0; i < 8; ++i) {
      char key[8];
      tokenKey(i, key, sizeof(key));
      size_t tokLen = prefs.getBytesLength(key);
      if (tokLen == 32) {
        prefs.getBytes(key, g_mailbox.immobilizer_tokens[i], 32);
        g_mailbox.token_valid[i] = true;
      }
    }

    prefs.end();
  }

  void persistRoot() {
    prefs.begin(kNs, false);
    prefs.putString("v_id", g_mailbox.vehicle_id);
    prefs.putBytes("v_pub", g_mailbox.vehicle_pub, 65);
    prefs.putBytes("v_priv", g_mailbox.vehicle_priv, 32);
    prefs.putUShort("sig_bmp", g_mailbox.signaling_bitmap);
    prefs.putUChar("slot_bmp", g_mailbox.slot_bitmap);
    prefs.end();
  }
}

namespace CCCMailbox {

bool begin() {
  if (g_loaded) return true;

  loadMailbox();

  bool invalidVehiclePair = g_mailbox.vehicle_pub_valid && g_mailbox.vehicle_priv_valid
      ? !validateVehicleKeypair(g_mailbox.vehicle_pub, g_mailbox.vehicle_priv)
      : false;
  bool missingRoot = (strlen(g_mailbox.vehicle_id) != 8)
      || !g_mailbox.vehicle_pub_valid
      || !g_mailbox.vehicle_priv_valid
      || invalidVehiclePair;
  if (missingRoot) {
    if (g_mailbox.vehicle_pub_valid && !g_mailbox.vehicle_priv_valid) {
      Serial.println("[CCC] Vehicle private key missing; rekeying vehicle identity and clearing provisioned data");
    }
    clearLegacyProvisioning();
    clearAll();
    generateVehicleId(g_mailbox.vehicle_id);
    if (!generateVehicleKeypair(g_mailbox.vehicle_pub, g_mailbox.vehicle_priv)) {
      Serial.println("[CCC] ERROR: Failed to generate vehicle keypair");
      return false;
    }
    g_mailbox.vehicle_pub_valid = true;
    g_mailbox.vehicle_priv_valid = true;
    g_mailbox.signaling_bitmap = 0;
    g_mailbox.slot_bitmap = 0;
    persistRoot();
    Serial.printf("[CCC] Initialized mailbox (v_id=%s)\n", g_mailbox.vehicle_id);
  } else {
    Serial.printf("[CCC] Loaded mailbox (v_id=%s slot_bmp=0x%02X)\n",
                  g_mailbox.vehicle_id, g_mailbox.slot_bitmap);
  }

  g_loaded = true;

  mbedtls_entropy_init(&g_vsign_entropy);
  mbedtls_ctr_drbg_init(&g_vsign_drbg);
  const char* pers = "ccc_vsig";
  int seedRc = mbedtls_ctr_drbg_seed(&g_vsign_drbg, mbedtls_entropy_func, &g_vsign_entropy,
                                     (const unsigned char*)pers, strlen(pers));
  g_vsign_rng_ready = (seedRc == 0);
  if (!g_vsign_rng_ready) {
    Serial.printf("[CCC] WARNING: vehicle signing RNG seed failed rc=%d\n", seedRc);
  }

  return true;
}

const CCC_Mailbox& get() {
  return g_mailbox;
}

const char* vehicleId() {
  return g_mailbox.vehicle_id;
}

bool hasVehiclePub() {
  return g_mailbox.vehicle_pub_valid;
}

bool hasVehiclePriv() {
  return g_mailbox.vehicle_priv_valid;
}

bool getVehiclePub(uint8_t* out, size_t max) {
  if (!out || max < 65 || !g_mailbox.vehicle_pub_valid) return false;
  memcpy(out, g_mailbox.vehicle_pub, 65);
  return true;
}

bool signVehicleDataP256(const uint8_t* data, size_t dataLen,
                        uint8_t* sigDerOut, size_t sigDerMax,
                        size_t* sigDerLenOut) {
  if (!data || dataLen == 0 || !sigDerOut || sigDerMax == 0 || !sigDerLenOut) return false;
  if (!g_mailbox.vehicle_priv_valid) return false;
  if (!g_vsign_rng_ready) return false;

  unsigned char hash[32];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  mbedtls_sha256_update(&sha, data, dataLen);
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);

  mbedtls_ecdsa_context ecdsa;
  mbedtls_ecdsa_init(&ecdsa);
  int rc = 0;
  if (rc == 0) rc = mbedtls_ecp_group_load(&ecdsa.grp, MBEDTLS_ECP_DP_SECP256R1);
  if (rc == 0) rc = mbedtls_mpi_read_binary(&ecdsa.d, g_mailbox.vehicle_priv, 32);
  if (rc == 0) {
    rc = mbedtls_ecp_mul(&ecdsa.grp, &ecdsa.Q, &ecdsa.d, &ecdsa.grp.G,
                         mbedtls_ctr_drbg_random, &g_vsign_drbg);
  }

  size_t sigLen = 0;
  if (rc == 0) {
    rc = mbedtls_ecdsa_write_signature(&ecdsa, MBEDTLS_MD_SHA256,
                                       hash, sizeof(hash),
                                       sigDerOut, &sigLen,
                                       mbedtls_ctr_drbg_random, &g_vsign_drbg);
  }

  mbedtls_ecdsa_free(&ecdsa);

  if (rc != 0 || sigLen == 0 || sigLen > sigDerMax) {
    return false;
  }

  *sigDerLenOut = sigLen;
  return true;
}

bool hasEndpointPub() {
  return g_mailbox.endpoint_pub_valid;
}

bool getEndpointPub(uint8_t* out, size_t max) {
  if (!out || max < 65 || !g_mailbox.endpoint_pub_valid) return false;
  memcpy(out, g_mailbox.endpoint_pub, 65);
  return true;
}

bool setEndpointPub(const uint8_t* pub65) {
  if (!pub65 || !isValidPub(pub65)) return false;
  memcpy(g_mailbox.endpoint_pub, pub65, 65);
  g_mailbox.endpoint_pub_valid = true;
  prefs.begin(kNs, false);
  bool ok = prefs.putBytes("ep_pub", pub65, 65) == 65;
  prefs.end();
  return ok;
}

bool clearEndpointPub() {
  memset(g_mailbox.endpoint_pub, 0, sizeof(g_mailbox.endpoint_pub));
  g_mailbox.endpoint_pub_valid = false;
  prefs.begin(kNs, false);
  bool ok = true;
  if (prefs.isKey("ep_pub")) ok = prefs.remove("ep_pub");
  prefs.end();
  return ok;
}

bool isSlotActive(uint8_t slotIndex) {
  if (slotIndex > 7) return false;
  return (g_mailbox.slot_bitmap & (1 << slotIndex)) != 0;
}

bool setSlotActive(uint8_t slotIndex, bool active) {
  if (slotIndex > 7) return false;
  if (active) {
    g_mailbox.slot_bitmap |= (1 << slotIndex);
  } else {
    g_mailbox.slot_bitmap &= ~(1 << slotIndex);
  }
  prefs.begin(kNs, false);
  bool ok = prefs.putUChar("slot_bmp", g_mailbox.slot_bitmap);
  prefs.end();
  return ok;
}

bool hasToken(uint8_t slotIndex) {
  if (slotIndex > 7) return false;
  return g_mailbox.token_valid[slotIndex];
}

bool getToken(uint8_t slotIndex, uint8_t out[32]) {
  if (slotIndex > 7 || !out || !g_mailbox.token_valid[slotIndex]) return false;
  memcpy(out, g_mailbox.immobilizer_tokens[slotIndex], 32);
  return true;
}

bool setToken(uint8_t slotIndex, const uint8_t tok[32]) {
  if (slotIndex > 7 || !tok) return false;
  memcpy(g_mailbox.immobilizer_tokens[slotIndex], tok, 32);
  g_mailbox.token_valid[slotIndex] = true;
  char key[8];
  tokenKey(slotIndex, key, sizeof(key));
  prefs.begin(kNs, false);
  bool ok = prefs.putBytes(key, tok, 32) == 32;
  prefs.end();
  return ok;
}

bool clearToken(uint8_t slotIndex) {
  if (slotIndex > 7) return false;
  memset(g_mailbox.immobilizer_tokens[slotIndex], 0, 32);
  g_mailbox.token_valid[slotIndex] = false;
  char key[8];
  tokenKey(slotIndex, key, sizeof(key));
  prefs.begin(kNs, false);
  bool ok = true;
  if (prefs.isKey(key)) ok = prefs.remove(key);
  prefs.end();
  return ok;
}

bool ensureToken(uint8_t slotIndex) {
  if (slotIndex > 7) return false;
  if (g_mailbox.token_valid[slotIndex]) return true;
  uint8_t tok[32];
  for (size_t i = 0; i < sizeof(tok); ++i) {
    tok[i] = (uint8_t)(esp_random() & 0xFF);
  }
  return setToken(slotIndex, tok);
}

uint16_t signalingBitmap() {
  return g_mailbox.signaling_bitmap;
}

bool setSignalingBitmap(uint16_t bitmap) {
  g_mailbox.signaling_bitmap = bitmap;
  prefs.begin(kNs, false);
  bool ok = prefs.putUShort("sig_bmp", bitmap);
  prefs.end();
  return ok;
}

bool setSignalingFlag(uint16_t mask, bool enabled) {
  uint16_t bitmap = g_mailbox.signaling_bitmap;
  if (enabled) bitmap |= mask;
  else bitmap &= ~mask;
  return setSignalingBitmap(bitmap);
}

void clearMailboxes() {
  clearEndpointPub();
  g_mailbox.signaling_bitmap = 0;
  g_mailbox.slot_bitmap = 0;
  prefs.begin(kNs, false);
  prefs.putUShort("sig_bmp", g_mailbox.signaling_bitmap);
  prefs.putUChar("slot_bmp", g_mailbox.slot_bitmap);
  prefs.end();

  for (uint8_t i = 0; i < 8; ++i) {
    clearToken(i);
  }
}

void clearAll() {
  prefs.begin(kNs, false);
  prefs.clear();
  prefs.end();
  memset(&g_mailbox, 0, sizeof(g_mailbox));
  g_loaded = false;
}

} // namespace CCCMailbox
