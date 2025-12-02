#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include "ble/utils/crypto_utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ble/ble_auth.h"
#include "provisioning_phase.h"

namespace {
  // Phase B Authentication service
  const char* kAuthServiceUUID        = "0000aaaa-1234-5678-9abc-def012345678";
  const char* kCharHandshakeWriteUUID = "0000aaab-1234-5678-9abc-def012345678"; // Write: phone sends ephemeral+sig
  const char* kCharHandshakeReadUUID  = "0000aaac-1234-5678-9abc-def012345678"; // Read/Notify: ECU sends ephemeral+sig  
  const char* kCharStatusUUID         = "0000aaad-1234-5678-9abc-def012345678"; // Notify: auth status

  // GATT characteristic handles
  NimBLECharacteristic* g_cHandshakeRead = nullptr;
  NimBLECharacteristic* g_cStatus = nullptr;

  // RNG context
  mbedtls_ctr_drbg_context* s_drbg = nullptr;

  // Authentication state machine
  enum AuthState {
    AUTH_IDLE,
    AUTH_WAITING_FOR_PHONE,
    AUTH_VERIFYING_PHONE,
    AUTH_COMPUTING_SHARED_SECRET,
    AUTH_SESSION_READY,
    AUTH_FAILED
  };

  AuthState s_authState = AUTH_IDLE;
  
  // Ephemeral keypair (generated fresh per connection)
  mbedtls_ecp_group s_grp;
  mbedtls_mpi s_ephemeral_priv;
  mbedtls_ecp_point s_ephemeral_pub;
  uint8_t s_ecu_ephemeral_pub_bytes[65]; // uncompressed format: 0x04 + X(32) + Y(32)
  bool s_ephemeral_generated = false;

  // Phone ephemeral data (received)
  uint8_t s_phone_ephemeral_pub[65];
  uint8_t s_phone_signature[80]; // DER signature buffer
  size_t s_phone_sig_len = 0;
  bool s_phone_data_received = false;

  // Session keys
  uint8_t s_session_key_enc[32];  // AES-256 key
  uint8_t s_session_key_mac[32];  // HMAC key
  uint8_t s_shared_secret[32];    // ECDH result
  bool s_session_keys_ready = false;

  // Worker task
  TaskHandle_t s_authWorkerTask = nullptr;

  // Debug counters
  uint32_t s_auth_attempts = 0;
  uint32_t s_auth_successes = 0;
  uint32_t s_auth_failures = 0;

  void print_hex(const char* label, const uint8_t* data, size_t len) {
    Serial.printf("[AUTH-DEBUG] %s (%u bytes): ", label, (unsigned)len);
    for (size_t i = 0; i < len; i++) {
      Serial.printf("%02X", data[i]);
      if (i < len - 1) Serial.print(" ");
    }
    Serial.println();
  }

  void reset_auth_state() {
    Serial.println("[AUTH] Resetting authentication state");
    
    s_authState = AUTH_IDLE;
    s_ephemeral_generated = false;
    s_phone_data_received = false;
    s_session_keys_ready = false;
    s_phone_sig_len = 0;
    
    memset(s_ecu_ephemeral_pub_bytes, 0, sizeof(s_ecu_ephemeral_pub_bytes));
    memset(s_phone_ephemeral_pub, 0, sizeof(s_phone_ephemeral_pub));
    memset(s_phone_signature, 0, sizeof(s_phone_signature));
    memset(s_session_key_enc, 0, sizeof(s_session_key_enc));
    memset(s_session_key_mac, 0, sizeof(s_session_key_mac));
    memset(s_shared_secret, 0, sizeof(s_shared_secret));

    // Clean up mbedTLS contexts
    mbedtls_ecp_group_free(&s_grp);
    mbedtls_mpi_free(&s_ephemeral_priv);
    mbedtls_ecp_point_free(&s_ephemeral_pub);
  }

  bool generate_ephemeral_keypair() {
    Serial.println("[AUTH] Step A: Generating ephemeral keypair");
    
    mbedtls_ecp_group_init(&s_grp);
    mbedtls_mpi_init(&s_ephemeral_priv);
    mbedtls_ecp_point_init(&s_ephemeral_pub);

    // Load P-256 curve
    int ret = mbedtls_ecp_group_load(&s_grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to load P-256 curve: -0x%x\n", -ret);
      return false;
    }

    // Generate keypair
    ret = mbedtls_ecp_gen_keypair(&s_grp, &s_ephemeral_priv, &s_ephemeral_pub, 
                                  mbedtls_ctr_drbg_random, s_drbg);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to generate ephemeral keypair: -0x%x\n", -ret);
      return false;
    }

    // Export public key to uncompressed format
    size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&s_grp, &s_ephemeral_pub, 
                                         MBEDTLS_ECP_PF_UNCOMPRESSED, 
                                         &olen, s_ecu_ephemeral_pub_bytes, 
                                         sizeof(s_ecu_ephemeral_pub_bytes));
    if (ret != 0 || olen != 65) {
      Serial.printf("[AUTH] ERROR: Failed to export ephemeral public key: -0x%x (olen=%u)\n", -ret, (unsigned)olen);
      return false;
    }

    print_hex("ECU Ephemeral Public Key", s_ecu_ephemeral_pub_bytes, 65);
    
    s_ephemeral_generated = true;
    Serial.println("[AUTH] ✓ Ephemeral keypair generated successfully");
    return true;
  }

  bool sign_ephemeral_with_device_key() {
    Serial.println("[AUTH] Step B: Signing ephemeral key with device identity");
    
    if (!ProvisioningPhase::isProvisioned()) {
      Serial.println("[AUTH] ERROR: Device not provisioned - cannot sign");
      return false;
    }

    // Get device private key from provisioning phase
    uint8_t device_priv_pem[500];
    size_t device_priv_len = ProvisioningPhase::getDevicePrivateKeyPEM(device_priv_pem, sizeof(device_priv_pem));
    
    if (device_priv_len == 0) {
      Serial.println("[AUTH] ERROR: Cannot retrieve device private key");
      return false;
    }

    // Parse device private key
    mbedtls_pk_context pk_ctx;
    mbedtls_pk_init(&pk_ctx);
    
    int ret = mbedtls_pk_parse_key(&pk_ctx, device_priv_pem, device_priv_len, nullptr, 0);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to parse device private key: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    // Hash the ephemeral public key
    uint8_t hash[32];
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);
    
    ret = mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    if (ret == 0) {
      mbedtls_md_starts(&md_ctx);
      mbedtls_md_update(&md_ctx, s_ecu_ephemeral_pub_bytes, 65);
      mbedtls_md_finish(&md_ctx, hash);
    }
    mbedtls_md_free(&md_ctx);
    
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to hash ephemeral key: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    print_hex("Hash of ECU ephemeral key", hash, 32);

    // Sign the hash
    uint8_t signature[80];
    size_t sig_len = 0;
    
    ret = mbedtls_pk_sign(&pk_ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                          signature, &sig_len, mbedtls_ctr_drbg_random, s_drbg);
    
    mbedtls_pk_free(&pk_ctx);
    
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to sign ephemeral key: -0x%x\n", -ret);
      return false;
    }

    print_hex("ECU signature", signature, sig_len);

    // Build handshake packet: [ephemeral_pub(65)] + [sig_len(2)] + [signature]
    std::string handshake_packet;
    handshake_packet.reserve(65 + 2 + sig_len);
    
    // Add ephemeral public key
    handshake_packet.append((char*)s_ecu_ephemeral_pub_bytes, 65);
    
    // Add signature length (little endian)
    uint16_t sig_len_le = (uint16_t)sig_len;
    handshake_packet.push_back((char)(sig_len_le & 0xFF));
    handshake_packet.push_back((char)(sig_len_le >> 8));
    
    // Add signature
    handshake_packet.append((char*)signature, sig_len);

    // Update handshake read characteristic
    if (g_cHandshakeRead) {
      g_cHandshakeRead->setValue(handshake_packet);
      g_cHandshakeRead->notify();
      Serial.printf("[AUTH] ✓ ECU handshake packet sent (%u bytes)\n", (unsigned)handshake_packet.length());
      print_hex("Full ECU handshake packet", (uint8_t*)handshake_packet.data(), handshake_packet.length());
    } else {
      Serial.println("[AUTH] ERROR: Handshake read characteristic not available");
      return false;
    }

    s_authState = AUTH_WAITING_FOR_PHONE;
    Serial.println("[AUTH] ✓ ECU signature completed, waiting for phone");
    return true;
  }

  // Callback for phone handshake data
  class HandshakeWriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      std::string value = pCharacteristic->getValue();
      Serial.printf("[AUTH] Step C: Received phone handshake data (%u bytes)\n", (unsigned)value.length());
      
      s_auth_attempts++;
      
      if (s_authState != AUTH_WAITING_FOR_PHONE) {
        Serial.printf("[AUTH] ERROR: Not in waiting state (current: %d)\n", s_authState);
        return;
      }

      print_hex("Raw phone handshake", (uint8_t*)value.data(), value.length());
      
      // Parse: [ephemeral_pub(65)] + [sig_len(2)] + [signature]
      if (value.length() < 67) { // Minimum: 65 + 2
        Serial.printf("[AUTH] ERROR: Handshake too short (%u bytes)\n", (unsigned)value.length());
        return;
      }

      const uint8_t* data = (uint8_t*)value.data();
      
      // Extract ephemeral public key
      memcpy(s_phone_ephemeral_pub, data, 65);
      
      // Extract signature length (little endian)
      uint16_t sig_len = data[65] | (data[66] << 8);
      
      if (67 + sig_len != value.length()) {
        Serial.printf("[AUTH] ERROR: Length mismatch. Expected %u, got %u\n", 
                      67 + sig_len, (unsigned)value.length());
        return;
      }
      
      if (sig_len > sizeof(s_phone_signature)) {
        Serial.printf("[AUTH] ERROR: Signature too large (%u bytes)\n", sig_len);
        return;
      }

      // Extract signature
      memcpy(s_phone_signature, data + 67, sig_len);
      s_phone_sig_len = sig_len;
      s_phone_data_received = true;

      Serial.printf("[AUTH] ✓ Parsed phone data: pubkey=65 bytes, signature=%u bytes\n", sig_len);
      
      s_authState = AUTH_VERIFYING_PHONE;
      
      // Notify worker task
      if (s_authWorkerTask) {
        xTaskNotifyGive(s_authWorkerTask);
      }
    }
  };

  bool verify_phone_signature() {
    Serial.println("[AUTH] Step D: Verifying phone signature");
    
    if (!s_phone_data_received) {
      Serial.println("[AUTH] ERROR: No phone data received");
      return false;
    }

    // Get stored phone public key from Phase A
    uint8_t phone_pub_raw[65];
    size_t pub_len = ProvisioningPhase::getPhonePubRaw(phone_pub_raw, sizeof(phone_pub_raw));
    
    if (pub_len != 65) {
      Serial.printf("[AUTH] ERROR: Cannot retrieve phone public key (len=%u)\n", (unsigned)pub_len);
      return false;
    }

    print_hex("Stored phone public key", phone_pub_raw, 65);
    print_hex("Received phone ephemeral key", s_phone_ephemeral_pub, 65);
    print_hex("Received phone signature", s_phone_signature, s_phone_sig_len);

    // Parse phone public key
    mbedtls_pk_context pk_ctx;
    mbedtls_pk_init(&pk_ctx);
    
    int ret = mbedtls_pk_setup(&pk_ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to setup PK context: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    mbedtls_ecp_keypair* ec_key = mbedtls_pk_ec(pk_ctx);
    ret = mbedtls_ecp_group_load(&ec_key->grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to load curve: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    // Import phone public key
    ret = mbedtls_ecp_point_read_binary(&ec_key->grp, &ec_key->Q, phone_pub_raw, 65);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to parse phone public key: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    // Hash the phone ephemeral public key
    uint8_t hash[32];
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);
    
    ret = mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    if (ret == 0) {
      mbedtls_md_starts(&md_ctx);
      mbedtls_md_update(&md_ctx, s_phone_ephemeral_pub, 65);
      mbedtls_md_finish(&md_ctx, hash);
    }
    mbedtls_md_free(&md_ctx);

    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to hash phone ephemeral key: -0x%x\n", -ret);
      mbedtls_pk_free(&pk_ctx);
      return false;
    }

    print_hex("Hash of phone ephemeral key", hash, 32);

    // Verify signature
    ret = mbedtls_pk_verify(&pk_ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                            s_phone_signature, s_phone_sig_len);
    
    mbedtls_pk_free(&pk_ctx);

    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Phone signature verification failed: -0x%x\n", -ret);
      s_auth_failures++;
      return false;
    }

    Serial.println("[AUTH] ✓ Phone signature verified successfully");
    s_authState = AUTH_COMPUTING_SHARED_SECRET;
    return true;
  }

  bool compute_shared_secret_and_session_keys() {
    Serial.println("[AUTH] Step E: Computing ECDH shared secret");

    // Parse phone ephemeral public key
    mbedtls_ecp_point phone_ephemeral_point;
    mbedtls_ecp_point_init(&phone_ephemeral_point);
    
    int ret = mbedtls_ecp_point_read_binary(&s_grp, &phone_ephemeral_point, 
                                            s_phone_ephemeral_pub, 65);
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to parse phone ephemeral key: -0x%x\n", -ret);
      mbedtls_ecp_point_free(&phone_ephemeral_point);
      return false;
    }

    // Compute shared secret: S = d_ecu * Q_phone
    mbedtls_mpi shared_secret_mpi;
    mbedtls_mpi_init(&shared_secret_mpi);
    
    ret = mbedtls_ecdh_compute_shared(&s_grp, &shared_secret_mpi, 
                                      &phone_ephemeral_point, &s_ephemeral_priv,
                                      mbedtls_ctr_drbg_random, s_drbg);
    
    mbedtls_ecp_point_free(&phone_ephemeral_point);
    
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: ECDH computation failed: -0x%x\n", -ret);
      mbedtls_mpi_free(&shared_secret_mpi);
      return false;
    }

    // Convert to bytes (big-endian, zero-padded to 32 bytes)
    size_t olen = mbedtls_mpi_size(&shared_secret_mpi);
    memset(s_shared_secret, 0, sizeof(s_shared_secret));
    ret = mbedtls_mpi_write_binary(&shared_secret_mpi, 
                                   s_shared_secret + (sizeof(s_shared_secret) - olen), olen);
    mbedtls_mpi_free(&shared_secret_mpi);
    
    if (ret != 0) {
      Serial.printf("[AUTH] ERROR: Failed to export shared secret: -0x%x\n", -ret);
      return false;
    }

    print_hex("ECDH Shared Secret", s_shared_secret, 32);

    Serial.println("[AUTH] Step F: Deriving session keys with HKDF");

    // Derive encryption key: HKDF(shared_secret, "SmartCarAuth-ENC" || ecu_pub || phone_pub)
    uint8_t info_enc[16 + 65 + 65];
    size_t info_enc_len = 0;
    
    const char* label_enc = "SmartCarAuth-ENC";
    memcpy(info_enc, label_enc, 16);
    info_enc_len += 16;
    memcpy(info_enc + info_enc_len, s_ecu_ephemeral_pub_bytes, 65);
    info_enc_len += 65;
    memcpy(info_enc + info_enc_len, s_phone_ephemeral_pub, 65);
    info_enc_len += 65;

    // Derive encryption key (hkdf_sha256 returns void)
    hkdf_sha256(nullptr, 0, s_shared_secret, sizeof(s_shared_secret),
                info_enc, info_enc_len, s_session_key_enc, sizeof(s_session_key_enc));

    // Derive MAC key: HKDF(shared_secret, "SmartCarAuth-MAC" || ecu_pub || phone_pub)
    uint8_t info_mac[16 + 65 + 65];
    size_t info_mac_len = 0;
    
    const char* label_mac = "SmartCarAuth-MAC";
    memcpy(info_mac, label_mac, 16);
    info_mac_len += 16;
    memcpy(info_mac + info_mac_len, s_ecu_ephemeral_pub_bytes, 65);
    info_mac_len += 65;
    memcpy(info_mac + info_mac_len, s_phone_ephemeral_pub, 65);
    info_mac_len += 65;

    // Derive MAC key (hkdf_sha256 returns void)
    hkdf_sha256(nullptr, 0, s_shared_secret, sizeof(s_shared_secret),
                info_mac, info_mac_len, s_session_key_mac, sizeof(s_session_key_mac));

    print_hex("Session Encryption Key", s_session_key_enc, 32);
    print_hex("Session MAC Key", s_session_key_mac, 32);

    s_session_keys_ready = true;
    s_authState = AUTH_SESSION_READY;
    s_auth_successes++;
    
    Serial.println("[AUTH] ✓ Phase B authentication completed successfully!");
    Serial.printf("[AUTH] Stats: Attempts=%u, Successes=%u, Failures=%u\n", 
                  s_auth_attempts, s_auth_successes, s_auth_failures);

    // Notify status
    if (g_cStatus) {
      g_cStatus->setValue("AUTH_SUCCESS");
      g_cStatus->notify();
    }

    return true;
  }

  void auth_worker_task(void* parameter) {
    Serial.println("[AUTH] Worker task started");
    
    for (;;) {
      // Wait for notification
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      
      Serial.printf("[AUTH] Processing state: %d\n", s_authState);
      
      switch (s_authState) {
        case AUTH_VERIFYING_PHONE:
          if (verify_phone_signature()) {
            if (!compute_shared_secret_and_session_keys()) {
              s_authState = AUTH_FAILED;
              if (g_cStatus) {
                g_cStatus->setValue("AUTH_FAILED");
                g_cStatus->notify();
              }
            }
          } else {
            s_authState = AUTH_FAILED;
            if (g_cStatus) {
              g_cStatus->setValue("AUTH_FAILED");
              g_cStatus->notify();
            }
          }
          break;
          
        default:
          Serial.printf("[AUTH] Unexpected state in worker: %d\n", s_authState);
          break;
      }
    }
  }

  // Connection callbacks
  class AuthServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
      Serial.println("[AUTH] Client connected - starting Phase B authentication");
      
      // Reset state
      reset_auth_state();
      
      // Check prerequisites
      if (!ProvisioningPhase::isProvisioned()) {
        Serial.println("[AUTH] ERROR: Device not provisioned, cannot authenticate");
        s_authState = AUTH_FAILED;
        if (g_cStatus) {
          g_cStatus->setValue("NOT_PROVISIONED");
          g_cStatus->notify();
        }
        return;
      }

      // Generate ephemeral keypair
      if (!generate_ephemeral_keypair()) {
        Serial.println("[AUTH] ERROR: Failed to generate ephemeral keypair");
        s_authState = AUTH_FAILED;
        if (g_cStatus) {
          g_cStatus->setValue("KEYGEN_FAILED");
          g_cStatus->notify();
        }
        return;
      }

      // Sign and send our handshake
      if (!sign_ephemeral_with_device_key()) {
        Serial.println("[AUTH] ERROR: Failed to sign ephemeral key");
        s_authState = AUTH_FAILED;
        if (g_cStatus) {
          g_cStatus->setValue("SIGN_FAILED");
          g_cStatus->notify();
        }
        return;
      }

      Serial.println("[AUTH] ✓ ECU ready, waiting for phone handshake");
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
      (void)pServer; (void)connInfo; (void)reason;
      Serial.println("[AUTH] Client disconnected");
      reset_auth_state();
    }
  };
}

namespace BLEAuth {
  void registerService(NimBLEServer* server, mbedtls_ctr_drbg_context* drbg) {
    Serial.println("[AUTH] Registering Phase B authentication service");
    
    s_drbg = drbg;
    reset_auth_state();

    // Create or get auth service
    NimBLEService* pAuth = server->getServiceByUUID(kAuthServiceUUID);
    if (!pAuth) {
      pAuth = server->createService(kAuthServiceUUID);
    }

    // Handshake Write characteristic (phone -> ECU)
    NimBLECharacteristic* cHandshakeWrite = pAuth->createCharacteristic(
        kCharHandshakeWriteUUID, 
        NIMBLE_PROPERTY::WRITE
    );
    static HandshakeWriteCallbacks handshakeWriteCb;
    cHandshakeWrite->setCallbacks(&handshakeWriteCb);

    // Handshake Read characteristic (ECU -> phone)
    g_cHandshakeRead = pAuth->createCharacteristic(
        kCharHandshakeReadUUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Status characteristic
    g_cStatus = pAuth->createCharacteristic(
        kCharStatusUUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Set server callbacks
    static AuthServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // Start worker task
    xTaskCreate(auth_worker_task, "AuthWorker", 8192, nullptr, 5, &s_authWorkerTask);

    // Start service
    pAuth->start();
    
    Serial.println("[AUTH] ✓ Phase B service registered");
    Serial.printf("[AUTH] Service UUID: %s\n", kAuthServiceUUID);
    Serial.printf("[AUTH] Handshake Write: %s\n", kCharHandshakeWriteUUID);
    Serial.printf("[AUTH] Handshake Read: %s\n", kCharHandshakeReadUUID);
    Serial.printf("[AUTH] Status: %s\n", kCharStatusUUID);
  }

  bool isSessionReady() { 
    return s_authState == AUTH_SESSION_READY && s_session_keys_ready; 
  }

  const uint8_t* sessionEncKey() { 
    return s_session_keys_ready ? s_session_key_enc : nullptr; 
  }

  size_t sessionEncKeyLen() { 
    return s_session_keys_ready ? sizeof(s_session_key_enc) : 0; 
  }

  const uint8_t* sessionMacKey() {
    return s_session_keys_ready ? s_session_key_mac : nullptr;
  }

  size_t sessionMacKeyLen() {
    return s_session_keys_ready ? sizeof(s_session_key_mac) : 0;
  }

  void resetSession() {
    Serial.println("[AUTH] Manual session reset requested");
    reset_auth_state();
  }

  void printStats() {
    Serial.println("[AUTH] === Authentication Statistics ===");
    Serial.printf("  Total attempts: %u\n", s_auth_attempts);
    Serial.printf("  Successes: %u\n", s_auth_successes);
    Serial.printf("  Failures: %u\n", s_auth_failures);
    Serial.printf("  Current state: %d\n", s_authState);
    Serial.printf("  Session ready: %s\n", isSessionReady() ? "YES" : "NO");
    Serial.printf("  Provisioned: %s\n", ProvisioningPhase::isProvisioned() ? "YES" : "NO");
  }
}
