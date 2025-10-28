#pragma once
#include <Arduino.h>

namespace Provisioning {

  // initialize ECC keys and load cert if any
  void begin();

  // perform provisioning exchange through NFC
  void runNfcProvisioning();

  // print key info or cert (debug)
  void printInfo();

  // check if provisioning data already exists
  bool isProvisioned();

  // Store the captured NFC tag UID as the authorized key (persists in NVS)
  bool storeAuthorizedTag(const uint8_t* uid, uint8_t len);

  // Check if the given NFC tag UID matches the stored authorized key
  bool isTagAuthorized(const uint8_t* uid, uint8_t len);

  // Returns true if an authorized NFC tag UID has been stored in NVS
  bool hasAuthorizedTag();

  // Administrative helpers
  // Clear device keypair and cert (does not touch authorized tags)
  void clearKeys();
  // Clear everything: keypair, cert, and all authorized tags
  void clearAll();

  // Multi-tag management
  // Add/remove a tag to the authorized list (persists in NVS)
  bool addAuthorizedTag(const uint8_t* uid, uint8_t len);
  bool removeAuthorizedTag(const uint8_t* uid, uint8_t len);
  // Print list of authorized tags
  void listAuthorizedTags();

  // Phase B helpers (crypto access)
  // Sign arbitrary data with the device's long-term private key (ECDSA-SHA256).
  // sigOut receives DER-encoded ECDSA signature.
  bool signWithDeviceKey(const uint8_t* data, size_t dataLen, uint8_t* sigOut, size_t* sigLen);

  // Verify signature using the stored phone long-term public key (from Phase A).
  // Returns false if no phone key/cert is stored.
  bool verifyWithPhoneKey(const uint8_t* data, size_t dataLen, const uint8_t* sig, size_t sigLen);

  // Whether a phone public key/cert is available for verification.
  bool hasPhonePublicKey();

  // Set or replace the stored phone long-term public key (PEM). Returns true on success.
  bool setPhonePublicKey(const char* pem);

  // Convenience: store an uncompressed P-256 public key (65 bytes: 0x04||X||Y).
  // Internally converts to PEM SubjectPublicKeyInfo and saves like setPhonePublicKey().
  bool setPhonePublicKeyRaw65(const uint8_t* uncompressed65, size_t len);

  // NFC pairing helpers (for coherent NFC-only provisioning/unlock)
  // Persist a phone keyId string in NVS (used to identify which key signed).
  bool setPhoneKeyId(const char* keyId);
  // Load the stored keyId into the provided buffer. On input, *inoutLen is capacity.
  bool getPhoneKeyId(char* out, size_t* inoutLen);

  // Optional: store phone certificate chain (opaque string, e.g., PEM)
  bool setPhoneCertChain(const char* certPem);
  bool hasPhoneCertChain();

  // Compute a fingerprint of the ECU device public key (32 bytes SHA-256 of PEM for demo)
  bool getDevicePubKeyFingerprint(uint8_t* out, size_t* inoutLen);

  // Fill random bytes using provisioning DRBG
  void randomBytes(uint8_t* out, size_t len);
}
