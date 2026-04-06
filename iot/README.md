## ESP32 Smart Car Access (IoT)

This repository is the in-vehicle side of a digital key system inspired by CCC Release 3.

If you are new to this domain, think of the system like this:
- The car (ESP32) is the gatekeeper.
- The phone is the key holder.
- The cloud is only a mailbox for share packages and app state, not a source of truth for unlock secrets.

Core security rules:
1. Immobilizer secrets stay in the vehicle.
2. The vehicle trusts only cryptographically verifiable data.

---

## 1) Quick Glossary (Beginner Friendly)

- CCC: Car Connectivity Consortium digital key standard family.
- AID: Applet ID. Like the app name inside NFC card emulation.
- HCE: Host Card Emulation. Android emulates a smart card over NFC.
- APDU: Command/response packet format for smart card style communication.
- Provisioning: First-time pairing of owner phone and vehicle.
- Endpoint key (`ep_pub`): Owner phone public key.
- Vehicle key (`v_pub`): Vehicle public key.
- Attestation package: Signed key-share package that proves owner approval.

---

## 2) System Architecture

Main components:
1. ESP32 firmware:
- NFC reader (PN532) for owner provisioning.
- BLE services for authentication and attestation upload.
- CCC mailbox in NVS for persistent confidential state.

2. Android app:
- HCE applet for NFC APDU exchange.
- Android Keystore for phone private key operations.
- Flutter UI for scanning, provisioning, and management.

3. Firebase:
- Stores app-level metadata and share records.
- Does not store vehicle immobilizer tokens.

---

## 3) CCC Mailbox (Vehicle Confidential Storage)

Stored in ESP32 NVS (`ccc_dk`) and mirrored in RAM:
- `v_id` (8 bytes): Vehicle ID generated once.
- `v_pub` (65 bytes): Vehicle public key generated once.
- `ep_pub` (65 bytes): Owner public key from successful provisioning.
- `slot_bmp` (8 bits): Active slots bitmap.
- `sig_bmp` (16 bits): Signaling flags bitmap.
- `tok_0..tok_7` (32 bytes each): Immobilizer tokens.

Current behavior:
1. First boot creates `v_id` and `v_pub`.
2. Owner provisioning fills slot 0 and ensures `tok_0` exists.
3. Share slots (1..7) remain inactive until key sharing activation is enabled.

---

## 4) Owner Provisioning (Phase A) - Current Live Flow

The implementation now follows this robust order:

1. SELECT AID
- ESP32 sends SELECT to CCC HCE applet AID.
- Android returns SELECT payload tags (`5A`, `5C`) + `9000`.
- ESP32 validates payload, not only `SW=9000`, to reject stale responses.

2. MasterCard-based authentication (SPAKE2+ shell)
- ESP32 builds challenge: `vehicle_id(8) || nonce(16)`.
- Sends INS `0x30` with TLV `0x50` challenge.
- Requests verify via INS `0x32`, expects TLV `0x58` HMAC.
- ESP32 computes local HMAC and must match exactly.

3. Base key exchange pull (GET DATA)
- ESP32 sends INS `0xCA` (`Lc=0`).
- Android returns base credential packet wrapped by tag `7F24`.
- ESP32 unwraps and parses phone endpoint public key and optional cert chain.

4. Vehicle key exchange push (WRITE DATA)
- ESP32 sends INS `0xD4` with TLVs:
  - `0x80`: `vehicle_id`
  - `0x81`: `vehicle_pub_key`
- Android validates and stores this binding locally.

5. Signature proof (GET DATA with challenge)
- ESP32 sends INS `0xCA` with 24-byte challenge as APDU data.
- Android signs with Keystore private key and returns DER signature.
- ESP32 verifies signature with parsed `ep_pub`.
- Fail-closed behavior: empty/invalid signature is rejected.

6. Commit gate (OP CONTROL FLOW)
- ESP32 sends INS `0x3C` with `P1=0x11`.
- If commit fails, provisioning is aborted and not persisted.

7. Persist owner data in mailbox
- Store owner `ep_pub`.
- Activate slot 0.
- Ensure `tok_0`.
- Set signaling flag.

8. Result notify and close
- ESP32 sends INS `0xDA` result to app.
- App updates provisioning result flags.
- Session exits with bounded removal wait to avoid re-trigger loops.

---

## 5) Reliability Hardening That Was Recently Added

This is what was updated and why it matters:

1. First-tap reliability
- Detection tuned for faster phone pickup on first tap.
- Prevents the old "first tap ignored, second tap works" behavior.

2. Stale-frame protection
- SELECT response content is validated to avoid accepting delayed old frames.

3. APDU recovery strategy
- In-place retry first.
- Then controlled `inRelease -> reselect -> resend` recovery.

4. Provisioning transaction hardening
- Added WRITE DATA (`0xD4`) path for vehicle key push.
- Moved SPAKE2+ authentication before key exchange.
- Removed zero-length signature acceptance fallback.
- OP CONTROL is now a commit gate (not merely informational).

5. UX and result consistency
- Countdown timeout in app no longer overwrites already successful provisioning.
- Back/close result handling keeps dashboard provisioning state consistent.

6. Cloud write compatibility fallback
- Primary owner provisioning record is saved into authorized `cars` doc.
- Optional mirror write to `Vehicles` is best-effort and ignored on permission-denied.
- This avoids false "registration failed" while preserving app continuity.

---

## 6) BLE Authentication (Phase B)

Purpose:
- After provisioning, prove phone owns the enrolled key and establish a secure session.

High-level steps:
1. Exchange ephemeral public keys.
2. Verify phone signature using stored owner key.
3. Derive shared session keys via ECDH.
4. Perform challenge-response bound to `vehicle_id`.
5. Notify FSM and open encrypted command path.

---

## 7) Attestation Service (Critical for Key Sharing)

### What attestation means in plain language

Attestation is a signed permission slip from the owner that says:
- which vehicle,
- which friend key,
- which slot,
- what time window,
- what entitlement,
are allowed.

Without a valid owner signature, the vehicle rejects the package.

### Why attestation matters

1. Key Sharing:
- Owner can authorize another user key without giving away owner private key.

2. Policy control:
- Can limit access by time (`valid_from`, `valid_until`) and entitlement.

3. Auditable trust:
- Vehicle can independently verify owner approval offline.

### Current payload format (147 bytes)

Layout:
- `0x00..0x07`: Vehicle_ID (8)
- `0x08`: Slot_ID (1)
- `0x09..0x49`: Friend public key (65)
- `0x4A..0x4D`: Valid_From (uint32, big-endian)
- `0x4E..0x51`: Valid_Until (uint32, big-endian)
- `0x52`: Entitlement (1)
- `0x53..0x72`: Signature_R (32)
- `0x73..0x92`: Signature_S (32)

Verification on ESP32:
1. Vehicle_ID must match local mailbox `v_id`.
2. Signature must verify with stored owner public key.
3. Time window must be valid (trusted epoch required).
4. Slot_ID must be in range 1..7.

Current policy:
- Even valid slot 1..7 attestation is currently blocked with `ERR_SLOT_LOCKED` until sharing activation is enabled.

Service UUIDs:
- Service: `555a0001-00aa-1111-2222-333344445555`
- Auth_RX: `555a0002-00aa-1111-2222-333344445555`
- Auth_TX: `555a0003-00aa-1111-2222-333344445555`

Responses:
- `READY`, `OK`, `ERR_LEN`, `ERR_VID`, `ERR_SIG`, `ERR_TIME`, `ERR_SLOT`, `ERR_NOT_PROVISIONED`, `ERR_SLOT_LOCKED`

---

## 8) Android App + Cloud Integration Notes

NFC result handshake:
- `flutter.provision_result` (bool)
- `flutter.provision_ts` (epoch ms)

Current app behavior:
1. On success, dashboard marks car as provisioned.
2. `ownerProvisioning` record is written into the `cars` document.
3. Optional mirror write to `Vehicles/<vehicle_id_hex>` may be blocked by Firestore rules and is treated as non-fatal.

If you still see Firestore permission warnings for `Vehicles`, provisioning can still be healthy as long as:
- car doc has `provisioned: true`
- car doc has `ownerProvisioning` payload

---

## 9) BLE Admin Commands

Notable commands:
- `0x01`: Clear provisioning only
- `0x02`: Clear all provisioning data
- `0x33`: Status summary
- `0x34`: Validate cert vs stored public key
- `0x35`: Check if public key is present
- `0x36`: CCC mailbox summary

---

## 10) Build and Flash

Firmware:

```bash
platformio run
platformio run --target upload
platformio device monitor
```

---

## 11) Time Requirement

Attestation validation depends on trusted time.

Without trusted epoch:
- verifier may return `ERR_TIME`.

Time source options:
1. SNTP/NTP
2. RTC module
3. BLE time sync from phone

---

## 12) Current Limitations

1. Slots 1..7 are verified but intentionally not activated yet.
2. Token MAC binding for share payload is not yet implemented in the current 147-byte format.
3. `Vehicles` collection write may require Firestore rule updates (optional path).

---

## 13) What We Updated, Why It Matters, and What Comes Next

This section explains recent updates in simple terms and the roadmap.

### What we just updated

1. Provisioning trust flow got stricter:
- Added explicit vehicle-to-phone WRITE DATA (`0xD4`).
- Authentication now runs earlier.
- Signature verification is strict fail-closed.
- Commit step (`0x3C`) now gates persistence.

2. NFC user experience improved:
- Better first tap reliability.
- Bounded removal wait with less log spam.

3. App consistency improved:
- Success no longer flips to timeout by accident.
- Cloud write fallback avoids false failure experience when optional collection is blocked.

### Why this is important

1. Security:
- Better guarantees that only verified data becomes persistent owner state.

2. Reliability:
- Fewer transport edge-case failures and better recovery.

3. Product readiness:
- Strong provisioning foundation makes the next phases much easier and safer.

### What next we can do

1. Authentication phase hardening (BLE Phase B):
- Add richer session lifecycle checks.
- Add replay protection and strict session expiry handling.

2. UWB ranging phase:
- Integrate secure ranging session setup after BLE/NFC trust is established.
- Use UWB distance as an extra signal for passive entry decisions.

3. Key sharing phase:
- Enable slot activation 1..7 once attestation policy is finalized.
- Add friend key lifecycle: issue, rotate, revoke, expire.

4. Attestation evolution:
- Add optional token-bound MAC field if needed for stronger cryptographic coupling.
- Extend entitlement model for feature-level permissions.

5. Cloud and policy hardening:
- Finalize Firestore rules for `Vehicles` writes by authorized owner only.
- Add audit trail and recovery flows.

6. Production readiness:
- Ensure trusted time source is always available.
- Remove debug bypass paths.
- Add full E2E automated verification for provisioning and attestation.

---

## 14) Security Notes

1. Owner private key remains in Android Keystore.
2. ESP32 stores owner public key and verifies locally.
3. Cloud must never store immobilizer tokens.
4. Debug/test bypass features must be disabled in production.





























