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
}
