#pragma once
#include <cstddef>
#include <cstdint>

namespace CCCMailbox {

struct CCC_Mailbox {
  char vehicle_id[9];
  uint8_t vehicle_pub[65];
  uint8_t endpoint_pub[65];
  uint16_t signaling_bitmap;
  uint8_t slot_bitmap;
  uint8_t immobilizer_tokens[8][32];
  bool vehicle_pub_valid;
  bool endpoint_pub_valid;
  bool token_valid[8];
};

bool begin();
const CCC_Mailbox& get();

const char* vehicleId();
bool hasVehiclePub();
bool getVehiclePub(uint8_t* out, size_t max);

bool hasEndpointPub();
bool getEndpointPub(uint8_t* out, size_t max);
bool setEndpointPub(const uint8_t* pub65);
bool clearEndpointPub();

bool isSlotActive(uint8_t slotIndex);
bool setSlotActive(uint8_t slotIndex, bool active);

bool hasToken(uint8_t slotIndex);
bool getToken(uint8_t slotIndex, uint8_t out[32]);
bool setToken(uint8_t slotIndex, const uint8_t tok[32]);
bool clearToken(uint8_t slotIndex);
bool ensureToken(uint8_t slotIndex);

uint16_t signalingBitmap();
bool setSignalingBitmap(uint16_t bitmap);
bool setSignalingFlag(uint16_t mask, bool enabled);

// Clear only the mailboxes (endpoint pub, signaling, slot bitmap, tokens).
void clearMailboxes();
// Clear all CCC mailbox data, including vehicle identity.
void clearAll();

} // namespace CCCMailbox
