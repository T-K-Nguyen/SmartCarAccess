#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#include <cstring>
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
#include <freertos/queue.h>

#include "ble/ble_auth.h"
#include "ble/ble.h"
#include "ble/ble_rollout.h"
#include "ble/pke_telemetry.h"
#include "provisioning_phase.h"
#include "ccc_mailbox.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"

// Uncomment to enable verbose debug logging (adds ~150-200ms overhead)
// #define BLE_AUTH_VERBOSE_DEBUG

namespace {
  // Phase B Authentication service
  const char* kAuthServiceUUID = "0000aaaa-1234-5678-9abc-def012345678";
  const char* kCharCCCRxUUID   = "0000aac1-1234-5678-9abc-def012345678"; // Write: phone sends APDU-like commands
  const char* kCharCCCTxUUID   = "0000aac2-1234-5678-9abc-def012345678"; // Read/Notify: ESP sends responses/events

  // APDU-like tunnel instructions
  static constexpr uint8_t kInsAuth0       = 0x80;
  static constexpr uint8_t kInsAuth1       = 0x81;
  static constexpr uint8_t kInsExchange    = 0x82;
  static constexpr uint8_t kInsControlFlow = 0x83;

  // SW status words
  static constexpr uint8_t kSw1Ok = 0x90;
  static constexpr uint8_t kSw2Ok = 0x00;
  static constexpr uint8_t kSw1WrongData = 0x6A;
  static constexpr uint8_t kSw2WrongData = 0x80;
  static constexpr uint8_t kSw1WrongP1P2 = 0x6A;
  static constexpr uint8_t kSw2WrongP1P2 = 0x86;
  static constexpr uint8_t kSw1Conditions = 0x69;
  static constexpr uint8_t kSw2Conditions = 0x85;
  static constexpr uint8_t kSw1Unsupported = 0x6A;
  static constexpr uint8_t kSw2Unsupported = 0x81;
  static constexpr uint8_t kSw1Internal = 0x6F;
  static constexpr uint8_t kSw2Internal = 0x00;

  // GATT characteristic handles
  NimBLECharacteristic* g_cTunnelTx = nullptr;

  // RNG context
  mbedtls_ctr_drbg_context* s_drbg = nullptr;

  // Authentication state machine
  enum AuthState {
    AUTH_IDLE,
    AUTH_WAITING_FOR_PHONE,
    AUTH_VERIFYING_PHONE,
    AUTH_EPHEMERAL_READY,
    AUTH_COMPUTING_SHARED_SECRET,
    AUTH_CHALLENGE_READY,
    AUTH_SIGNING_SENT,
    AUTH_VERIFYING_RESPONSE,
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

  // Challenge/response (Phase B verification)
  uint8_t s_vehicle_id[8];
  uint8_t s_nonce[16];
  uint8_t s_challenge[24]; // vehicleId(8) || nonce(16)
  uint8_t s_sig_buf[128];
  size_t s_sig_len = 0;

  // Worker task
  TaskHandle_t s_authWorkerTask = nullptr;

  // Debug counters
  uint32_t s_auth_attempts = 0;
  uint32_t s_auth_successes = 0;
  uint32_t s_auth_failures = 0;

  // GPS data callback
  BLEAuth::GpsDataCallback s_gps_callback = nullptr;

  struct AuthLatencyCounters {
    uint32_t run_id = 0;
    uint32_t t_connect_ms = 0;
    uint32_t t_auth0_rx_ms = 0;
    uint32_t t_auth1_tx_ms = 0;
    uint32_t t_auth_verified_ms = 0;
    uint32_t t_control_flow_ack_ms = 0;
    bool has_connect = false;
    bool has_auth0_rx = false;
    bool has_auth1_tx = false;
    bool has_auth_verified = false;
    bool has_control_flow_ack = false;
    bool reported = false;
  };

  AuthLatencyCounters s_latency;

  static uint32_t delta_ms(uint32_t start, uint32_t end) {
    return end - start;
  }

  static uint32_t offset_from_connect(const AuthLatencyCounters& c, bool hasMark, uint32_t markMs) {
    if (!c.has_connect || !hasMark) return 0;
    return delta_ms(c.t_connect_ms, markMs);
  }

  static void clear_latency_marks() {
    s_latency.t_connect_ms = 0;
    s_latency.t_auth0_rx_ms = 0;
    s_latency.t_auth1_tx_ms = 0;
    s_latency.t_auth_verified_ms = 0;
    s_latency.t_control_flow_ack_ms = 0;
    s_latency.has_connect = false;
    s_latency.has_auth0_rx = false;
    s_latency.has_auth1_tx = false;
    s_latency.has_auth_verified = false;
    s_latency.has_control_flow_ack = false;
    s_latency.reported = false;
  }

  static void start_latency_run() {
    s_latency.run_id++;
    clear_latency_marks();
    s_latency.t_connect_ms = millis();
    s_latency.has_connect = true;
  }

  static void print_latency_report_if_ready() {
    if (s_latency.reported || !s_latency.has_control_flow_ack) return;
    s_latency.reported = true;

    Serial.printf("[AUTH-LAT] run=%lu counters_ms AUTH0_rx=%lu AUTH1_tx=%lu AUTH_VERIFIED=%lu CONTROL_FLOW_ack=%lu\n",
                  (unsigned long)s_latency.run_id,
                  (unsigned long)offset_from_connect(s_latency, s_latency.has_auth0_rx, s_latency.t_auth0_rx_ms),
                  (unsigned long)offset_from_connect(s_latency, s_latency.has_auth1_tx, s_latency.t_auth1_tx_ms),
                  (unsigned long)offset_from_connect(s_latency, s_latency.has_auth_verified, s_latency.t_auth_verified_ms),
                  (unsigned long)offset_from_connect(s_latency, s_latency.has_control_flow_ack, s_latency.t_control_flow_ack_ms));

    if (s_latency.has_auth0_rx && s_latency.has_control_flow_ack) {
      Serial.printf("[AUTH-LAT] run=%lu door_open_path_ms(AUTH0_rx->CONTROL_FLOW_ack)=%lu\n",
                    (unsigned long)s_latency.run_id,
                    (unsigned long)delta_ms(s_latency.t_auth0_rx_ms, s_latency.t_control_flow_ack_ms));
    }
    if (s_latency.has_auth_verified && s_latency.has_control_flow_ack) {
      Serial.printf("[AUTH-LAT] run=%lu unlock_ack_after_verified_ms=%lu\n",
                    (unsigned long)s_latency.run_id,
                    (unsigned long)delta_ms(s_latency.t_auth_verified_ms, s_latency.t_control_flow_ack_ms));
    }
  }

  void print_hex(const char* label, const uint8_t* data, size_t len) {
#ifdef BLE_AUTH_VERBOSE_DEBUG
    Serial.printf("[AUTH-DEBUG] %s (%u bytes): ", label, (unsigned)len);
    for (size_t i = 0; i < len; i++) {
      Serial.printf("%02X", data[i]);
      if (i < len - 1) Serial.print(" ");
    }
    Serial.println();
#else
    (void)label; (void)data; (void)len; // Suppress unused warnings
#endif
  }

  bool sendTunnelResponse(uint8_t ins, uint8_t sw1, uint8_t sw2,
                          const uint8_t* payload = nullptr, size_t payloadLen = 0) {
    if (!g_cTunnelTx) {
      Serial.println("[AUTH] ERROR: CCC_TX characteristic not available");
      return false;
    }
    if (payloadLen > 255) {
      Serial.println("[AUTH] ERROR: Tunnel payload too large");
      return false;
    }

    std::string frame;
    frame.reserve(4 + payloadLen);
    frame.push_back((char)ins);
    frame.push_back((char)sw1);
    frame.push_back((char)sw2);
    frame.push_back((char)payloadLen);
    if (payload && payloadLen > 0) {
      frame.append((const char*)payload, payloadLen);
    }

    g_cTunnelTx->setValue(frame);
    g_cTunnelTx->notify();
    return true;
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
    memset(s_vehicle_id, 0, sizeof(s_vehicle_id));
    memset(s_nonce, 0, sizeof(s_nonce));
    memset(s_challenge, 0, sizeof(s_challenge));
    memset(s_sig_buf, 0, sizeof(s_sig_buf));
    s_sig_len = 0;
    clear_latency_marks();

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

    s_ephemeral_generated = true;
    print_hex("ECU Ephemeral Public Key", s_ecu_ephemeral_pub_bytes, 65);
    Serial.println("[AUTH] ✓ Ephemeral keypair generated successfully");
    return true;
  }

  bool sign_ephemeral_with_device_key() {
    Serial.println("[AUTH] Step B: Sending AUTH1 payload signed by vehicle key");

    if (!ProvisioningPhase::isProvisioned()) {
      Serial.println("[AUTH] ERROR: Device not provisioned - cannot continue");
      return false;
    }

    if (!CCCMailbox::hasVehiclePriv()) {
      Serial.println("[AUTH] ERROR: Vehicle private key missing");
      return false;
    }

    uint8_t signature_der[96];
    size_t signature_len = 0;
    if (!CCCMailbox::signVehicleDataP256(
            s_ecu_ephemeral_pub_bytes,
            sizeof(s_ecu_ephemeral_pub_bytes),
            signature_der,
            sizeof(signature_der),
            &signature_len)) {
      Serial.println("[AUTH] ERROR: Failed to sign AUTH1 payload with vehicle key");
      return false;
    }

    // Build handshake packet: [ephemeral_pub(65)] + [sig_len(2,LE)] + [DER signature]
    std::string handshake_packet;
    handshake_packet.reserve(65 + 2 + signature_len);

    // Add ephemeral public key
    handshake_packet.append((char*)s_ecu_ephemeral_pub_bytes, 65);

    // Add signature length (little endian)
    uint16_t sig_len_le = (uint16_t)signature_len;
    handshake_packet.push_back((char)(sig_len_le & 0xFF));
    handshake_packet.push_back((char)(sig_len_le >> 8));
    handshake_packet.append((const char*)signature_der, signature_len);

    // Send AUTH1 payload via tunnel TX.
    if (!sendTunnelResponse(kInsAuth1, kSw1Ok, kSw2Ok,
                            (const uint8_t*)handshake_packet.data(), handshake_packet.length())) {
      Serial.println("[AUTH] ERROR: Failed to send AUTH1 payload over tunnel");
      return false;
    }

    Serial.printf("[AUTH] ✓ AUTH1 payload sent (%u bytes, sig=%u)\n",
            (unsigned)handshake_packet.length(), (unsigned)signature_len);
        s_latency.t_auth1_tx_ms = millis();
        s_latency.has_auth1_tx = true;
    PKETelemetry::emit(PKETelemetry::Event::Auth1Sent);
    print_hex("AUTH1 payload", (uint8_t*)handshake_packet.data(), handshake_packet.length());
    FSMIntegration::BLE::onAuth1Sent();

    s_authState = AUTH_WAITING_FOR_PHONE;
    Serial.println("[AUTH] ✓ ECU ephemeral ready; waiting for phone handshake");
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

      s_authState = AUTH_VERIFYING_PHONE;
      Serial.printf("[AUTH] ✓ Parsed phone data: pubkey=65 bytes, signature=%u bytes\n", sig_len);
      
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

    Serial.println("[AUTH] Step F: Deriving session keys with HKDF");
    print_hex("ECDH Shared Secret", s_shared_secret, 32);

    // Derive encryption key: HKDF(shared_secret, "SmartCarv1|ENC" || ecu_pub || phone_pub)
    // Label is 14 bytes, pad to 16 with null bytes for alignment
    uint8_t info_enc[16 + 65 + 65];
    size_t info_enc_len = 0;
    
    const char* label_enc = "SmartCarv1|ENC\0\0"; // 14 chars + 2 null padding = 16 bytes
    memcpy(info_enc, label_enc, 16);
    info_enc_len += 16;
    memcpy(info_enc + info_enc_len, s_ecu_ephemeral_pub_bytes, 65);
    info_enc_len += 65;
    memcpy(info_enc + info_enc_len, s_phone_ephemeral_pub, 65);
    info_enc_len += 65;

    // Derive encryption key (hkdf_sha256 returns void)
    hkdf_sha256(nullptr, 0, s_shared_secret, sizeof(s_shared_secret),
                info_enc, info_enc_len, s_session_key_enc, sizeof(s_session_key_enc));

    // Derive MAC key: HKDF(shared_secret, "SmartCarv1|MAC" || ecu_pub || phone_pub)
    // Label is 14 bytes, pad to 16 with null bytes for alignment
    uint8_t info_mac[16 + 65 + 65];
    size_t info_mac_len = 0;
    
    const char* label_mac = "SmartCarv1|MAC\0\0"; // 14 chars + 2 null padding = 16 bytes
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
    
    Serial.println("[AUTH] ✓ Phase B authentication completed successfully!");
    Serial.printf("[AUTH] Stats: Attempts=%u, Successes=%u, Failures=%u\n", 
                  s_auth_attempts, s_auth_successes, s_auth_failures);

    // Prepare challenge: use CCC vehicleId and fresh nonce; publish via EXCHANGE frame.
    const char* vid = CCCMailbox::vehicleId();
    bool vidOk = vid && strlen(vid) == 8;
    if (vidOk) {
      memcpy(s_vehicle_id, vid, 8);
    } else {
      Serial.println("[AUTH] WARNING: CCC vehicleId missing; using random bytes");
      for (int i = 0; i < 8; ++i) s_vehicle_id[i] = (uint8_t)esp_random();
    }
    for (int i = 0; i < 16; ++i) s_nonce[i] = (uint8_t)esp_random();
    memcpy(s_challenge, s_vehicle_id, 8);
    memcpy(s_challenge + 8, s_nonce, 16);
    print_hex("VehicleId", s_vehicle_id, 8);
    print_hex("Nonce", s_nonce, 16);
    print_hex("Challenge", s_challenge, 24);

    sendTunnelResponse(kInsExchange, kSw1Ok, kSw2Ok, s_challenge, sizeof(s_challenge));
    Serial.println("[AUTH] ✓ Challenge published over tunnel; awaiting EXCHANGE signature response");
    s_authState = AUTH_CHALLENGE_READY;
    return true;
  }

  // Callback for signature over challenge
  class ChallengeWriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      (void)connInfo;
      std::string v = pCharacteristic->getValue();
      Serial.printf("[AUTH] Step G: Received challenge signature (%u bytes)\n", (unsigned)v.length());
      if (s_authState != AUTH_CHALLENGE_READY) {
        Serial.printf("[AUTH] ERROR: Not expecting challenge response (state=%d)\n", s_authState);
        return;
      }
      if (v.empty() || v.size() > sizeof(s_sig_buf)) {
        Serial.println("[AUTH] ERROR: Signature size invalid");
        return;
      }
      memcpy(s_sig_buf, v.data(), v.size());
      s_sig_len = v.size();
      s_authState = AUTH_SIGNING_SENT;
      Serial.println("[AUTH] ✓ Phone signature received; verifying response");
      s_authState = AUTH_VERIFYING_RESPONSE;
      sendTunnelResponse(kInsExchange, kSw1Ok, kSw2Ok);
      if (s_authWorkerTask) xTaskNotifyGive(s_authWorkerTask);
    }
  };

  // Unified CCC tunnel command callback (single RX write endpoint)
  class CCCCommandWriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      (void)connInfo;
      std::string value = pCharacteristic->getValue();
      if (value.size() < 5) {
        sendTunnelResponse(0x00, kSw1WrongData, kSw2WrongData);
        return;
      }

      const uint8_t* frame = (const uint8_t*)value.data();
      const uint8_t cla = frame[0];
      const uint8_t ins = frame[1];
      const uint8_t p1 = frame[2];
      const uint8_t p2 = frame[3];
      const uint8_t lc = frame[4];
      (void)cla;
      (void)p2;

      if (value.size() < (size_t)(5 + lc)) {
        sendTunnelResponse(ins, kSw1WrongData, kSw2WrongData);
        return;
      }
      const uint8_t* data = frame + 5;

      switch (ins) {
        case kInsAuth0: {
          s_latency.t_auth0_rx_ms = millis();
          s_latency.has_auth0_rx = true;
          FSMIntegration::BLE::onAuth0Received();
          PKETelemetry::setVehicleId(CCCMailbox::vehicleId());
          PKETelemetry::emit(PKETelemetry::Event::Auth0Received);

          BLERollout::Flags flags = BLERollout::flags();
          if (!connInfo.isBonded()) {
            if (flags.bondingEnforce) {
              Serial.println("[AUTH][SEC] Rejecting AUTH0: peer is unbonded while bonding_enforce=1");
              PKETelemetry::emit(PKETelemetry::Event::UnlockDecision,
                                PKETelemetry::kRssiUnknown,
                                "deny",
                                "bonding_enforced_unbonded");
              sendTunnelResponse(ins, kSw1Conditions, kSw2Conditions);
              break;
            }
            Serial.println("[AUTH][SEC] Progressive mode: peer unbonded, allowing standard AUTH0");
          }

          if (p1 == 0x01) {
            if (flags.fastTransaction) {
              Serial.println("[AUTH] Fast path requested but not yet implemented; fallback to standard P1=0x11");
            }
            sendTunnelResponse(ins, kSw1Unsupported, kSw2Unsupported);
            break;
          }
          if (p1 != 0x11) {
            sendTunnelResponse(ins, kSw1WrongP1P2, kSw2WrongP1P2);
            break;
          }
          if (!ProvisioningPhase::isProvisioned()) {
            sendTunnelResponse(ins, kSw1Conditions, kSw2Conditions);
            break;
          }
          if (!generate_ephemeral_keypair()) {
            sendTunnelResponse(ins, kSw1Internal, kSw2Internal);
            break;
          }
          sendTunnelResponse(ins, kSw1Ok, kSw2Ok, s_ecu_ephemeral_pub_bytes, sizeof(s_ecu_ephemeral_pub_bytes));
          FSMIntegration::BLE::onAuth0ResponseSent();
          // Prevent back-to-back notify coalescing on some BLE stacks.
          vTaskDelay(pdMS_TO_TICKS(20));
          s_authState = AUTH_EPHEMERAL_READY;
          if (s_authWorkerTask) {
            xTaskNotifyGive(s_authWorkerTask);
          } else {
            sendTunnelResponse(kInsAuth1, kSw1Internal, kSw2Internal);
          }
          break;
        }

        case kInsAuth1: {
          FSMIntegration::BLE::onAuth1ResponseReceived();
          s_auth_attempts++;

          if (lc < 67) {
            sendTunnelResponse(ins, kSw1WrongData, kSw2WrongData);
            break;
          }

          memcpy(s_phone_ephemeral_pub, data, 65);
          uint16_t sig_len = data[65] | (data[66] << 8);
          if ((size_t)(67 + sig_len) != lc || sig_len > sizeof(s_phone_signature)) {
            sendTunnelResponse(ins, kSw1WrongData, kSw2WrongData);
            break;
          }
          memcpy(s_phone_signature, data + 67, sig_len);
          s_phone_sig_len = sig_len;
          s_phone_data_received = true;
          s_authState = AUTH_VERIFYING_PHONE;
          if (s_authWorkerTask) {
            xTaskNotifyGive(s_authWorkerTask);
          }
          break;
        }

        case kInsExchange: {
          FSMIntegration::BLE::onExchangeReceived();
          // During standard auth flow, EXCHANGE carries challenge signature.
          if (s_authState == AUTH_CHALLENGE_READY) {
            if (lc == 0 || lc > sizeof(s_sig_buf)) {
              sendTunnelResponse(ins, kSw1WrongData, kSw2WrongData);
              break;
            }
            memcpy(s_sig_buf, data, lc);
            s_sig_len = lc;
            s_authState = AUTH_VERIFYING_RESPONSE;
            if (s_authWorkerTask) {
              xTaskNotifyGive(s_authWorkerTask);
            }
            break;
          }

          // Post-auth secure channel EXCHANGE acknowledgement (time sync payload handling TODO).
          if (!s_session_keys_ready) {
            sendTunnelResponse(ins, kSw1Conditions, kSw2Conditions);
            break;
          }
          sendTunnelResponse(ins, kSw1Ok, kSw2Ok);
          FSMIntegration::BLE::onExchangeResponseSent();
          break;
        }

        case kInsControlFlow: {
          FSMIntegration::BLE::onControlFlowReceived();
          if (!s_session_keys_ready) {
            sendTunnelResponse(ins, kSw1Conditions, kSw2Conditions);
            break;
          }
          FSMIntegration::BLE::onUnlockRequested();
          if (sendTunnelResponse(ins, kSw1Ok, kSw2Ok)) {
            s_latency.t_control_flow_ack_ms = millis();
            s_latency.has_control_flow_ack = true;
            PKETelemetry::emit(PKETelemetry::Event::ControlFlowAck);
            PKETelemetry::emit(PKETelemetry::Event::UnlockDecision,
                              PKETelemetry::kRssiUnknown,
                              "allow",
                              "control_flow_ack_ok");
            print_latency_report_if_ready();
          }
          FSMIntegration::BLE::onControlFlowResponseSent();
          break;
        }

        default:
          sendTunnelResponse(ins, kSw1Unsupported, kSw2Unsupported);
          break;
      }
    }
  };

  // GPS data callback - receives encrypted location data
  class GpsDataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      (void)connInfo;
      std::string v = pCharacteristic->getValue();
      Serial.printf("[GPS] Received GPS data packet (%u bytes)\n", (unsigned)v.length());
      
      // Check if session is ready
      if (!s_session_keys_ready) {
        Serial.println("[GPS] ERROR: Session not ready - cannot decrypt GPS data");
        return;
      }
      
      // Expected format: [encrypted_data(32) | hmac(32)] = 64 bytes total
      if (v.length() != 64) {
        Serial.printf("[GPS] ERROR: Invalid packet size. Expected 64 bytes, got %u\n", (unsigned)v.length());
        return;
      }
      
      const uint8_t* data = (uint8_t*)v.data();
      const uint8_t* encrypted_data = data;
      const uint8_t* received_hmac = data + 32;
      
      // Step 1: Verify HMAC
      uint8_t computed_hmac[32];
      if (!hmac_sha256(s_session_key_mac, sizeof(s_session_key_mac),
               encrypted_data, 32, computed_hmac)) {
        Serial.println("[GPS] ERROR: HMAC computation failed");
        return;
      }
      
      // Compare HMACs (constant-time comparison)
      bool hmac_valid = true;
      for (size_t i = 0; i < 32; i++) {
        if (computed_hmac[i] != received_hmac[i]) {
          hmac_valid = false;
        }
      }
      
      if (!hmac_valid) {
        Serial.println("[GPS] ERROR: HMAC verification failed - data may be tampered");
        print_hex("Expected HMAC", computed_hmac, 32);
        print_hex("Received HMAC", received_hmac, 32);
        if (s_gps_callback) {
          s_gps_callback(nullptr, 0, false);
        }
        return;
      }
      
      Serial.println("[GPS] ✓ HMAC verified successfully");
      
      // Step 2: Decrypt data (XOR with session key)
      uint8_t decrypted_data[32];
      for (size_t i = 0; i < 32; i++) {
        decrypted_data[i] = encrypted_data[i] ^ s_session_key_enc[i % sizeof(s_session_key_enc)];
      }
      
      // Step 3: Parse location data
      // Format: [latitude(8) | longitude(8) | altitude(4) | accuracy(4) | timestamp(8)]
      double latitude, longitude;
      float altitude, accuracy;
      int64_t timestamp;
      
      memcpy(&latitude, decrypted_data, 8);
      memcpy(&longitude, decrypted_data + 8, 8);
      memcpy(&altitude, decrypted_data + 16, 4);
      memcpy(&accuracy, decrypted_data + 20, 4);
      memcpy(&timestamp, decrypted_data + 24, 8);
      
      Serial.println("[GPS] === Decrypted GPS Data ===");
      Serial.printf("  Latitude:  %.6f\n", latitude);
      Serial.printf("  Longitude: %.6f\n", longitude);
      Serial.printf("  Altitude:  %.2f m\n", altitude);
      Serial.printf("  Accuracy:  %.2f m\n", accuracy);
      Serial.printf("  Timestamp: %lld\n", timestamp);
      Serial.println("[GPS] ============================");
      
      // Call user callback if registered
      if (s_gps_callback) {
        s_gps_callback(decrypted_data, 32, true);
      }
    }
  };

  bool verify_challenge_signature_and_finalize() {
    Serial.println("[AUTH] Step H: Verifying challenge signature with stored phone pub");
    if (s_sig_len == 0) { Serial.println("[AUTH] ERROR: No signature"); return false; }
    uint8_t phone_pub[65]; size_t pubLen = ProvisioningPhase::getPhonePubRaw(phone_pub, sizeof(phone_pub));
    if (pubLen != 65) { Serial.println("[AUTH] ERROR: Missing phone public key from provisioning"); return false; }
    bool ok = ProvisioningPhase::verifySignatureP256(phone_pub, s_challenge, sizeof(s_challenge), s_sig_buf, s_sig_len);
    Serial.printf("[AUTH] Challenge signature verify: %s\n", ok?"OK":"FAIL");
    if (!ok) { s_auth_failures++; return false; }
    Serial.println("[AUTH] ✓ Authentication granted. Ready to unlock relay.");
    return true;
  }

  void auth_worker_task(void* parameter) {
    Serial.println("[AUTH] Worker task started");
    
    for (;;) {
      // Wait for notification
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      
      Serial.printf("[AUTH] Processing state: %d\n", s_authState);
      
      switch (s_authState) {
          case AUTH_EPHEMERAL_READY:
          // Sign ECU ephemeral and send handshake packet to phone
          if (!sign_ephemeral_with_device_key()) {
            s_authState = AUTH_FAILED;
          } else {
            // remain in AUTH_WAITING_FOR_PHONE; phone will write AUTH1 response
          }
          break;
        case AUTH_VERIFYING_PHONE:
          if (verify_phone_signature()) {
            if (!compute_shared_secret_and_session_keys()) {
              s_authState = AUTH_FAILED;
              sendTunnelResponse(kInsAuth1, kSw1Conditions, kSw2Conditions);
            }
          } else {
            s_authState = AUTH_FAILED;
            FSMIntegration::BLE::onAuthFailed();
            sendTunnelResponse(kInsAuth1, kSw1Conditions, kSw2Conditions);
          }
          break;
        case AUTH_VERIFYING_RESPONSE:
          if (verify_challenge_signature_and_finalize()) {
            s_authState = AUTH_SESSION_READY;
            s_auth_successes++;
            s_latency.t_auth_verified_ms = millis();
            s_latency.has_auth_verified = true;
            PKETelemetry::emit(PKETelemetry::Event::AuthVerified);
            FSMIntegration::BLE::onAuthVerified();
            sendTunnelResponse(kInsExchange, kSw1Ok, kSw2Ok);
            FSMIntegration::BLE::onExchangeResponseSent();
          } else {
            s_authState = AUTH_FAILED;
            FSMIntegration::BLE::onAuthFailed();
            sendTunnelResponse(kInsExchange, kSw1Conditions, kSw2Conditions);
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
      Serial.println("[AUTH] Client connected - waiting for AUTH0 over tunnel");
      
      // Reset state
      reset_auth_state();
      start_latency_run();

      uint8_t addr[6] = {0};
      FSMIntegration::BLE::onClientConnected(addr);
      
      // Check prerequisites
      if (!ProvisioningPhase::isProvisioned()) {
        Serial.println("[AUTH] ERROR: Device not provisioned, cannot authenticate");
        s_authState = AUTH_FAILED;
        sendTunnelResponse(kInsAuth0, kSw1Conditions, kSw2Conditions);
        return;
      }

      s_authState = AUTH_IDLE;
      Serial.println("[AUTH] ✓ Tunnel session initialized");
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
      (void)connInfo;
      Serial.printf("[AUTH] Client disconnected (reason=%d)\n", reason);
      reset_auth_state();
      FSMIntegration::BLE::onClientDisconnected();
      
      // CRITICAL: Restart advertising so device can be discovered again
      Serial.println("[AUTH] Restarting BLE advertising...");
      BLEMod::restartAdvertising(true, "auth_disconnect");
      Serial.println("[AUTH] ✓ Advertising restarted - device is discoverable");
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

    // CCC tunnel RX/TX characteristics
    NimBLECharacteristic* cCccRx = pAuth->createCharacteristic(
        kCharCCCRxUUID,
        NIMBLE_PROPERTY::WRITE
    );
    static CCCCommandWriteCallbacks cccRxCb;
    cCccRx->setCallbacks(&cccRxCb);

    g_cTunnelTx = pAuth->createCharacteristic(
        kCharCCCTxUUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Set server callbacks
    static AuthServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // Start worker task
    xTaskCreate(auth_worker_task, "AuthWorker", 12288, nullptr, 5, &s_authWorkerTask);

    // Start service
    pAuth->start();
    
    Serial.println("[AUTH] ✓ Phase B service registered");
    Serial.printf("[AUTH] Service UUID: %s\n", kAuthServiceUUID);
    Serial.printf("[AUTH] CCC RX: %s\n", kCharCCCRxUUID);
    Serial.printf("[AUTH] CCC TX: %s\n", kCharCCCTxUUID);
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

  void setGpsDataCallback(GpsDataCallback callback) {
    s_gps_callback = callback;
    Serial.println("[GPS] GPS data callback registered");
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
