## ESP32 Smart Car Access (IoT)

This firmware implements the ESP32 side of a CCC-inspired digital key flow. It keeps all vehicle secrets on-device, uses NFC for owner provisioning, uses BLE for authentication and secure data, and exposes a BLE attestation path for key sharing packages that are stored in Firebase on the phone side.

The design follows two key rules:
1. The cloud never stores immobilizer tokens.
2. The ESP32 only trusts what it can verify locally (owner public key, signatures, and mailbox state).

---

## 1) CCC Mailbox (Confidential Storage)

The CCC mailbox is persisted in ESP32 NVS (namespace `ccc_dk`) and cached in RAM for fast access.

Stored fields:
- `v_id` (8 bytes): Vehicle ID, generated once at first boot.
- `v_pub` (65 bytes): Vehicle public key, generated once at first boot.
- `ep_pub` (65 bytes): Owner (endpoint) public key, stored during NFC provisioning.
- `slot_bmp` (8 bits): Slot activation bitmap (0..7).
- `sig_bmp` (16 bits): Signaling bitmap (flags).
- `tok_0..tok_7` (32 bytes each): Immobilizer tokens per slot.

Behavior:
- First boot generates `v_id` and `v_pub` and clears legacy provisioning data.
- Owner provisioning sets slot 0 active and generates `tok_0`.
- Slots 1..7 remain inactive until sharing is enabled.

---

## 2) NFC Owner Provisioning (Phase A)

NFC provisioning uses the phone HCE applet and a two-step GET_CHALLENGE exchange.

Flow summary:
1. ESP32 SELECTs the phone AID.
2. Base GET_CHALLENGE returns phone public key (65 bytes) and optional cert chain.
3. Signature GET_CHALLENGE verifies the phone signature over the challenge.
4. On success, ESP32 stores the owner public key, sets slot 0, and generates `tok_0`.

Challenge format:
- `challenge = vehicle_id(8) || nonce(16)`
- `vehicle_id` is taken from the CCC mailbox; a fallback constant is used only if missing.

Result:
- Owner is provisioned and the FSM is notified of credentials stored.

---

## 3) BLE Authentication (Phase B)

BLE Phase B establishes a secure session and proves the phone owns the long-term key.

High-level steps:
1. ESP32 generates an ephemeral keypair and sends the public key.
2. Phone returns its ephemeral key + signature over it.
3. ESP32 verifies the signature using the stored owner public key.
4. ECDH derives session keys (AES-256 key + HMAC key).
5. ESP32 sends a challenge: `vehicle_id(8) || nonce(16)`.
6. Phone signs the challenge; ESP32 verifies it.

Notes:
- `vehicle_id` for the challenge is pulled from the CCC mailbox, not random.
- On success, the FSM is notified and the session keys are ready for encrypted channels.

---

## 4) BLE Attestation Service (Key Sharing Package)

This is the ESP-side endpoint for the Firebase-driven sharing flow. The phone downloads the `attestation_package` from Firebase and writes it to the ESP32 over BLE.

Service UUIDs:
- Service: `555a0001-00aa-1111-2222-333344445555`
- Auth_RX (phone -> ESP): `555a0002-00aa-1111-2222-333344445555`
- Auth_TX (ESP -> phone): `555a0003-00aa-1111-2222-333344445555`

Attestation payload (fixed 147 bytes):
- Bytes 0..82 are hashed (SHA-256) and signed by the owner.
- Signature is raw ECDSA R/S (64 bytes), not DER.

Layout:
- 0x00..0x07: Vehicle_ID (8 bytes)
- 0x08: Slot_ID (1 byte)
- 0x09..0x49: Friend public key (65 bytes)
- 0x4A..0x4D: Valid_From (uint32, big-endian)
- 0x4E..0x51: Valid_Until (uint32, big-endian)
- 0x52: Entitlement (1 byte)
- 0x53..0x72: Signature_R (32 bytes)
- 0x73..0x92: Signature_S (32 bytes)

Verification rules on ESP32:
1. Vehicle_ID must match the CCC mailbox `v_id`.
2. Signature must verify using the stored owner public key.
3. Time window must be valid (requires a trusted epoch).
4. Slot_ID must be 1..7.

Current policy:
- Slots 1..7 are locked for now, even if the attestation is valid. The ESP replies with `ERR_SLOT_LOCKED` until sharing is enabled.

Status responses on Auth_TX:
- `READY`, `OK`, `ERR_LEN`, `ERR_VID`, `ERR_SIG`, `ERR_TIME`, `ERR_SLOT`, `ERR_NOT_PROVISIONED`, `ERR_SLOT_LOCKED`

MTU requirement:
- The 147-byte attestation write expects a larger MTU than the default 23 bytes. The firmware requests a larger MTU during BLE init (currently 512) so the payload is not fragmented.

---

## 5) BLE Admin Commands

The admin service provides diagnostics and maintenance.

Notable admin commands:
- 0x01: Clear provisioning only
- 0x02: Clear all provisioning data
- 0x33: Status summary
- 0x34: Validate cert vs stored public key
- 0x35: Check if public key is present
- 0x36: CCC mailbox summary (vehicle ID, slot bitmap, endpoint presence)

---

## 6) Time Requirement

The attestation verifier checks `valid_from` and `valid_until`. This requires a trusted epoch on the ESP32. If time is not set, the ESP replies with `ERR_TIME`.

Possible time sources:
- SNTP/NTP
- RTC module
- BLE time sync from the phone

---

## 7) Build

Use PlatformIO to build the firmware:

```
platformio run
```

---

## 8) Current Limitations (By Design)

- Shared slots (1..7) are parsed and verified but not activated yet.
- Token MAC binding is not implemented because the current 147-byte format does not carry a MAC and the phone does not possess `tok_n`.
- Attestation time checks will fail unless the ESP32 has a valid epoch.

---

## 9) Next Steps

- Add a trusted time source or allow a controlled dev bypass.
- Enable slot activation for sharing (slots 1..7) once the phone flow is ready.
- Define a token MAC binding step if cryptographic coupling to `tok_n` is required.

---

## 10) Current Runtime Behavior (Important)

This section documents the latest stable behavior after NFC/HCE reliability hardening.

### NFC polling and first tap behavior

- Card detect polling is tuned for faster HCE pickup.
- The main NFC loop accepts detection with a single stable read to reduce missed first taps on phones.
- Non-HCE cards may still be detected at RF level, but they fail at SELECT AID as expected.

### SELECT response validation

- The reader does not trust `SW=9000` alone.
- SELECT success requires expected payload bytes (`5A...` and `5C...`) to reject stale or delayed frames.
- This prevents false-positive SELECT success from old APDU traffic.

### APDU retry and recovery strategy

- APDUs use in-place retries first.
- If needed, recovery performs:
	1) `inRelease()`
	2) reselect target
	3) resend APDU
- This minimizes unnecessary reselect churn while still recovering from link-loss/deactivation.

### Provisioning close-out behavior

- After successful provisioning, OP CONTROL FLOW is best-effort and non-blocking.
- Provisioning result ACK (`INS=0xDA`) is sent to Android HCE.
- Session enters a short removal wait path to avoid immediate re-trigger loops.

Removal wait tuning:
- Fast check loop runs with short timeout slices.
- Logging is throttled to avoid monitor spam.
- Hard cap is short (about 4.5 seconds) to keep UX snappy.
- If removal is not detected within cap, session continues safely.

---

## 11) Android App Integration Notes

Provisioning state handshake between ESP and Android uses shared preferences flags:
- `flutter.provision_result` (boolean)
- `flutter.provision_ts` (epoch ms)

Expected behavior:
- HCE sets these when ESP sends provisioning result.
- App provisioning flow checks these to decide success.

Important UI fix already applied in app flow:
- Countdown timeout no longer overwrites success after provisioning has already succeeded.
- Success path cancels timer and returns success to dashboard update logic.

---

## 12) Security Notes

- Owner private key stays in Android Keystore.
- ESP stores owner public key and verifies signatures locally.
- Cloud/Firebase must not hold immobilizer tokens.
- Test bypasses (if enabled in debug builds) must be disabled for production.
















