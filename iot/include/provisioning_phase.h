#pragma once
#include <Arduino.h>

class PN532; // forward decl

namespace ProvisioningPhase {
  // Initialize provisioning (e.g., device keypair generation). Safe to call once at boot.
  void begin();

  // True if a phone keyId has already been stored.
  bool isProvisioned();

  // Store a keyId as hex if nothing is provisioned yet. Returns true on first-time set.
  bool storeKeyIdHexIfEmpty(const uint8_t* bytes, size_t len);
  // Store ASCII keyId (as provided by phone) if not already provisioned.
  bool storeKeyIdAsciiIfEmpty(const char* ascii);
  // Overwrite stored keyId unconditionally (for testing provisioning repeatedly)
  bool storeKeyIdAsciiForce(const char* ascii);

  // Clear all provisioning-related state (keys and key id)
  void clearAll();

  // Optional: run a simple provisioning exchange using the currently working HCE flow.
  // Uses the SELECT payload or subsequent UID APDU payload as the persistent ID.
  // Returns true if a new keyId was stored.
  bool runOnceWithHce(PN532& nfc, const uint8_t* aid, size_t aidLen, uint32_t waitMs = 15000);
}
