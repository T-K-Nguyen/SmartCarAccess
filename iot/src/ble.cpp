#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include "ble.h"
#include "provisioning.h"

namespace {
  // Device name and services
  const char* kDeviceName = "ESP-Smart-Car-ECU";

  // Admin service (preserved for NFC admin operations)
  const char* kAdminServiceUUID   = "9a9b9c9d-0000-4000-8000-9a9b9c9d0000";
  const char* kAdminModeCharUUID  = "9a9b9c9d-0001-4000-8000-9a9b9c9d0001"; // 0=normal,1=enroll,2=remove
  const char* kAdminCmdCharUUID   = "9a9b9c9d-0002-4000-8000-9a9b9c9d0002"; // write-only small commands
  const char* kAdminInfoCharUUID  = "9a9b9c9d-0003-4000-8000-9a9b9c9d0003"; // read/notify info

  // AUTH service (Phase B)
  const char* kAuthServiceUUID    = "d0d0d0d0-0000-4000-8000-d0d0d0d00000";
  const char* kCharClientHelloUUID= "d0d0d0d0-0001-4000-8000-d0d0d0d00001"; // Write: client eph pubkey + signature
  const char* kCharServerHelloUUID= "d0d0d0d0-0002-4000-8000-d0d0d0d00002"; // Read/Notify: server eph pubkey + signature
  const char* kCharClientCfmUUID  = "d0d0d0d0-0003-4000-8000-d0d0d0d00003"; // Write: client HMAC proof
  const char* kCharServerCfmUUID  = "d0d0d0d0-0004-4000-8000-d0d0d0d00004"; // Read/Notify: server HMAC proof

  bool s_bleStarted = false;
  bool s_sessionReady = false;

  // Admin state
  BLEMod::AdminMode s_adminMode = BLEMod::ADMIN_NORMAL;
  NimBLECharacteristic* g_cAdminMode = nullptr;
  NimBLECharacteristic* g_cAdminInfo = nullptr;

  // AUTH chars
  NimBLECharacteristic* g_cSrvHello = nullptr;
  NimBLECharacteristic* g_cSrvCfm = nullptr;

  // Ephemeral key and peer info
  mbedtls_ecp_group s_grp;
  mbedtls_mpi s_ephemeral_d;            // our ephemeral priv
  mbedtls_ecp_point s_ephemeral_Q;      // our ephemeral pub
  uint8_t s_srvPubBuf[65]; size_t s_srvPubLen = 0; // uncompressed 65 bytes
  uint8_t s_cliPubBuf[65]; size_t s_cliPubLen = 0;
  bool s_haveClientPub = false;

  // RNG for ephemeral generation
  mbedtls_entropy_context s_entropy;
  mbedtls_ctr_drbg_context s_drbg;

  // Derived keys
  uint8_t s_sessKeyEnc[32];
  uint8_t s_sessKeyMac[32];

  // Utilities
  void hkdf_sha256(const uint8_t* salt, size_t saltLen,
                   const uint8_t* ikm, size_t ikmLen,
                   const uint8_t* info, size_t infoLen,
                   uint8_t* out, size_t outLen) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t prk[32];
    // Extract: PRK = HMAC(salt, IKM)
    mbedtls_md_hmac(md, salt, saltLen, ikm, ikmLen, prk);
    // Expand: OKM = T(1)..
    uint8_t T[32]; size_t Tlen = 0; size_t offset = 0; uint8_t counter = 1;
    while (offset < outLen) {
      mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
      mbedtls_md_setup(&ctx, md, 1);
      mbedtls_md_hmac_starts(&ctx, prk, sizeof(prk));
      if (Tlen) mbedtls_md_hmac_update(&ctx, T, Tlen);
      if (info && infoLen) mbedtls_md_hmac_update(&ctx, info, infoLen);
      mbedtls_md_hmac_update(&ctx, &counter, 1);
      mbedtls_md_hmac_finish(&ctx, T);
      mbedtls_md_free(&ctx);
      size_t toCopy = (outLen - offset) < sizeof(T) ? (outLen - offset) : sizeof(T);
      memcpy(out + offset, T, toCopy);
      offset += toCopy; Tlen = sizeof(T); counter++;
    }
  }

  bool gen_ephemeral() {
    mbedtls_ecp_group_init(&s_grp);
    mbedtls_mpi_init(&s_ephemeral_d);
    mbedtls_ecp_point_init(&s_ephemeral_Q);
    if (mbedtls_ecp_group_load(&s_grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;
    int rc = mbedtls_ecp_gen_keypair(&s_grp, &s_ephemeral_d, &s_ephemeral_Q,
                                     mbedtls_ctr_drbg_random, &s_drbg);
    if (rc != 0) return false;
    // Export uncompressed Q
    size_t olen = 0;
    rc = mbedtls_ecp_point_write_binary(&s_grp, &s_ephemeral_Q,
      MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, s_srvPubBuf, sizeof(s_srvPubBuf));
    if (rc != 0 || olen == 0) return false;
    s_srvPubLen = olen;
    return true;
  }

  bool parse_client_pub(const uint8_t* buf, size_t len, mbedtls_ecp_point &Qout) {
    if (!buf || len == 0) return false;
    return mbedtls_ecp_point_read_binary(&s_grp, &Qout, buf, len) == 0;
  }

  void reset_auth_state() {
    s_sessionReady = false;
    s_haveClientPub = false;
    s_srvPubLen = 0; s_cliPubLen = 0;
    memset(s_sessKeyEnc, 0, sizeof(s_sessKeyEnc));
    memset(s_sessKeyMac, 0, sizeof(s_sessKeyMac));
  }

  // Admin mode characteristic: read/write 1 byte
  class AdminModeCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      uint8_t m = static_cast<uint8_t>(v[0]);
      if (m > 2) m = 0;
      s_adminMode = static_cast<BLEMod::AdminMode>(m);
      Serial.printf("[BLE-Admin] Mode set to %u\n", (unsigned)m);
      if (g_cAdminMode) g_cAdminMode->setValue(&m, 1);
    }
  };

  // Admin command characteristic
  class AdminCmdCallbacks : public NimBLECharacteristicCallbacks {
    NimBLECharacteristic* _info;
   public:
    explicit AdminCmdCallbacks(NimBLECharacteristic* info): _info(info) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      uint8_t cmd = static_cast<uint8_t>(v[0]);
      switch (cmd) {
        case 0x01:
          Provisioning::clearKeys(); _info->setValue("CLEARED_KEYS"); break;
        case 0x02:
          Provisioning::clearAll(); _info->setValue("CLEARED_ALL"); break;
        case 0x10:
          _info->setValue("LISTED_TAGS"); Provisioning::listAuthorizedTags(); break;
        default:
          _info->setValue("UNKNOWN_CMD"); break;
      }
    }
  };

  // AUTH: client hello write callback
  class ClientHelloCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.size() < 1 + 2) return;
      const uint8_t* p = (const uint8_t*)v.data(); size_t n = v.size();
      uint8_t ver = p[0]; if (ver != 1) { Serial.println("[AUTH] Bad version"); return; }
      size_t idx = 1;
      uint16_t pubLen = p[idx] | (p[idx+1] << 8); idx += 2;
      if (idx + pubLen + 2 > n || pubLen > sizeof(s_cliPubBuf)) { Serial.println("[AUTH] Bad pubLen"); return; }
      memcpy(s_cliPubBuf, p + idx, pubLen); s_cliPubLen = pubLen; idx += pubLen;
      uint16_t sigLen = p[idx] | (p[idx+1] << 8); idx += 2;
      if (idx + sigLen > n) { Serial.println("[AUTH] Bad sigLen"); return; }
      const uint8_t* sig = p + idx;

      if (!Provisioning::hasPhonePublicKey()) {
        Serial.println("[AUTH] No phone public key stored; cannot verify.");
        return;
      }
      if (!Provisioning::verifyWithPhoneKey(s_cliPubBuf, s_cliPubLen, sig, sigLen)) {
        Serial.println("[AUTH] Signature verify failed.");
        return;
      }
      s_haveClientPub = true;
      Serial.println("[AUTH] Client ephem verified.");

      // Generate our ephemeral, if needed
      if (s_srvPubLen == 0 && !gen_ephemeral()) {
        Serial.println("[AUTH] Failed to generate ephemeral.");
        return;
      }

      // Sign our ephemeral pub with device long-term key
      uint8_t sigOut[80]; size_t sigOutLen = 0;
      if (!Provisioning::signWithDeviceKey(s_srvPubBuf, s_srvPubLen, sigOut, &sigOutLen)) {
        Serial.println("[AUTH] Failed to sign server ephem.");
        return;
      }

      // Build ServerHello: [ver][pubLenLE][pub][sigLenLE][sig]
      std::string resp; resp.reserve(1 + 2 + s_srvPubLen + 2 + sigOutLen);
      resp.push_back((char)1);
      uint16_t pl = (uint16_t)s_srvPubLen; resp.push_back((char)(pl & 0xFF)); resp.push_back((char)(pl >> 8));
      resp.append((const char*)s_srvPubBuf, s_srvPubLen);
      uint16_t sl = (uint16_t)sigOutLen; resp.push_back((char)(sl & 0xFF)); resp.push_back((char)(sl >> 8));
      resp.append((const char*)sigOut, sigOutLen);

      if (g_cSrvHello) { g_cSrvHello->setValue(resp); g_cSrvHello->notify(); }

      // Compute shared secret via ECDH
      mbedtls_ecp_point Qcli; mbedtls_ecp_point_init(&Qcli);
      if (!parse_client_pub(s_cliPubBuf, s_cliPubLen, Qcli)) {
        mbedtls_ecp_point_free(&Qcli);
        Serial.println("[AUTH] Failed to parse client pub."); return;
      }
      mbedtls_mpi z; mbedtls_mpi_init(&z);
  int rc = mbedtls_ecdh_compute_shared(&s_grp, &z, &Qcli, &s_ephemeral_d,
               mbedtls_ctr_drbg_random, &s_drbg);
      mbedtls_ecp_point_free(&Qcli);
      if (rc != 0) { mbedtls_mpi_free(&z); Serial.println("[AUTH] ECDH failed."); return; }
      uint8_t S[32]; size_t olen = mbedtls_mpi_size(&z); memset(S, 0, sizeof(S));
      mbedtls_mpi_write_binary(&z, S + (sizeof(S) - olen), olen);
      mbedtls_mpi_free(&z);

      // HKDF derive keys: info = "BLE-AUTH" || cliPub || srvPub
      const char *label = "BLE-AUTH";
      uint8_t info[8 + 65 + 65]; size_t infoLen = 0;
      memcpy(info + infoLen, label, 8); infoLen += 8;
      memcpy(info + infoLen, s_cliPubBuf, s_cliPubLen); infoLen += s_cliPubLen;
      memcpy(info + infoLen, s_srvPubBuf, s_srvPubLen); infoLen += s_srvPubLen;
      hkdf_sha256(nullptr, 0, S, sizeof(S), info, infoLen, s_sessKeyEnc, sizeof(s_sessKeyEnc));
      // Expand again for mac with different label
      const char *label2 = "BLE-AUTH-MAC";
      uint8_t info2[12 + 65 + 65]; size_t info2Len = 0;
      memcpy(info2 + info2Len, label2, 12); info2Len += 12;
      memcpy(info2 + info2Len, s_cliPubBuf, s_cliPubLen); info2Len += s_cliPubLen;
      memcpy(info2 + info2Len, s_srvPubBuf, s_srvPubLen); info2Len += s_srvPubLen;
      hkdf_sha256(nullptr, 0, S, sizeof(S), info2, info2Len, s_sessKeyMac, sizeof(s_sessKeyMac));

      Serial.println("[AUTH] Derived session keys.");
    }
  };

  // AUTH: client confirm
  class ClientConfirmCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.size() == 0 || !s_haveClientPub || s_srvPubLen == 0) return;
      // Expect HMAC-SHA256 over label||cliPub||srvPub
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      const char *label = "AUTH-PROOF"; uint8_t msg[10 + 65 + 65]; size_t mlen = 0;
      memcpy(msg + mlen, label, 10); mlen += 10;
      memcpy(msg + mlen, s_cliPubBuf, s_cliPubLen); mlen += s_cliPubLen;
      memcpy(msg + mlen, s_srvPubBuf, s_srvPubLen); mlen += s_srvPubLen;
      uint8_t tag[32];
      mbedtls_md_hmac(md, s_sessKeyMac, sizeof(s_sessKeyMac), msg, mlen, tag);
      if (v.size() != sizeof(tag) || memcmp(v.data(), tag, sizeof(tag)) != 0) {
        Serial.println("[AUTH] Client HMAC mismatch.");
        return;
      }
      // Send our confirm: HMAC over label2||srvPub||cliPub
      const char *label2 = "AUTH-OK"; uint8_t msg2[8 + 65 + 65]; size_t m2 = 0;
      memcpy(msg2 + m2, label2, 8); m2 += 8;
      memcpy(msg2 + m2, s_srvPubBuf, s_srvPubLen); m2 += s_srvPubLen;
      memcpy(msg2 + m2, s_cliPubBuf, s_cliPubLen); m2 += s_cliPubLen;
      uint8_t tag2[32]; mbedtls_md_hmac(md, s_sessKeyMac, sizeof(s_sessKeyMac), msg2, m2, tag2);
      if (g_cSrvCfm) { g_cSrvCfm->setValue(tag2, sizeof(tag2)); g_cSrvCfm->notify(); }
      s_sessionReady = true;
      Serial.println("[AUTH] Session established.");
    }
  };
}

namespace BLEMod {
  void begin() {
    NimBLEDevice::init(kDeviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(185);

    // Init RNG for ephemeral keys
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char* pers = "ble-auth";
    mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                          (const unsigned char*)pers, strlen(pers));

    NimBLEServer *pServer = NimBLEDevice::createServer();

    // AUTH service
    NimBLEService* pAuth = pServer->createService(kAuthServiceUUID);
    NimBLECharacteristic* cCliHello = pAuth->createCharacteristic(
      kCharClientHelloUUID, NIMBLE_PROPERTY::WRITE);
    static ClientHelloCallbacks cliHelloCb; cCliHello->setCallbacks(&cliHelloCb);
    g_cSrvHello = pAuth->createCharacteristic(
      kCharServerHelloUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* cCliCfm = pAuth->createCharacteristic(
      kCharClientCfmUUID, NIMBLE_PROPERTY::WRITE);
    static ClientConfirmCallbacks cliCfmCb; cCliCfm->setCallbacks(&cliCfmCb);
    g_cSrvCfm = pAuth->createCharacteristic(
      kCharServerCfmUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    pAuth->start();

    // Admin service (unchanged)
    NimBLEService* pAdmin = pServer->createService(kAdminServiceUUID);
    NimBLECharacteristic* cAdminMode = pAdmin->createCharacteristic(
      kAdminModeCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    static AdminModeCallbacks adminModeCb; cAdminMode->setCallbacks(&adminModeCb);
    uint8_t initAdmin = (uint8_t)s_adminMode; cAdminMode->setValue(&initAdmin, 1); g_cAdminMode = cAdminMode;
    NimBLECharacteristic* cAdminInfo = pAdmin->createCharacteristic(
      kAdminInfoCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    cAdminInfo->setValue("READY"); g_cAdminInfo = cAdminInfo;
    NimBLECharacteristic* cAdminCmd = pAdmin->createCharacteristic(
      kAdminCmdCharUUID, NIMBLE_PROPERTY::WRITE);
    static AdminCmdCallbacks adminCmdCb(cAdminInfo); cAdminCmd->setCallbacks(&adminCmdCb);
    pAdmin->start();

    // Advertising
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData; advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanResp; scanResp.setName(kDeviceName); pAdvertising->setScanResponseData(scanResp);
    pAdvertising->addServiceUUID(kAuthServiceUUID);
    pAdvertising->addServiceUUID(kAdminServiceUUID);
    pAdvertising->start();

    Serial.printf("[BLE] Advertising started: %s\n", kDeviceName);
    s_bleStarted = true;
  }

  bool isStarted() { return s_bleStarted; }
  const char* deviceName() { return kDeviceName; }
  bool isSessionReady() { return s_sessionReady; }

  BLEMod::AdminMode getAdminMode() { return s_adminMode; }
  void setAdminMode(BLEMod::AdminMode mode) {
    s_adminMode = mode; uint8_t m = (uint8_t)mode; if (g_cAdminMode) g_cAdminMode->setValue(&m, 1);
  }

  void adminNotify(const char* msg) {
    if (g_cAdminInfo && msg) { g_cAdminInfo->setValue(msg); g_cAdminInfo->notify(); }
  }
}
