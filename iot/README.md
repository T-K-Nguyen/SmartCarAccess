# SmartCarAccess (ESP32-S3) – NFC Provisioning + BLE Mutual Auth

This project turns an ESP32‑S3 into a secure “digital car key” ECU. A phone app pairs with the ECU using NFC (short‑range, safe for the first exchange), then connects over BLE to perform a proper mutual authentication and set up per‑session encryption keys.

The result: the car only unlocks for a phone that really owns the long‑term key, and each BLE session uses fresh, unique keys (forward secrecy).

---

## What’s inside

- Hardware: ESP32‑S3 (Yolo_Uno_S3 variant), PN532 NFC over I²C
- Phase A (NFC provisioning): exchange long‑term identity (public keys), set up trust, and optionally enroll NFC tags for admin tasks
- Phase B (BLE mutual auth): ephemeral ECDH + signed key exchange + HKDF + HMAC confirmation → per‑session keys ready for secure messages
- Optional Admin over BLE: manage provisioning data and NFC tag list from a dev app

---

## Big picture

```
Phone App                BLE/NFC               ESP32‑S3 ECU
-----------              -------               -----------
   |  (Phase A)  NFC tap  |                          |
   | <--- Exchange long‑term public keys ----------> |
   |       Store trust in secure storage             |
   |                                                 |
   |  (Phase B)   BLE connect                        |
   | ---- Send ephemeral pubkey + signature -------> |
   | <--- Send ephemeral pubkey + signature -------  |
   | ---- HMAC confirm ----------------------------> |
   | <--- HMAC confirm ----------------------------  |
   |          (ECDH + HKDF => K_enc, K_mac)         |
   |               Secure session ready              |
```

Security goals:
- No private keys ever leave the phone or ECU
- Session keys are new every time (forward secrecy)
- Both sides prove they own the long‑term key established in Phase A

---

## Repository layout

- `platformio.ini` – PlatformIO config for the Yolo_Uno_S3
- `boards/yolo_uno.json` – board variant
- `include/` – public headers shared across modules
  - `ble.h` – BLE admin + handshake status API
  - `nfc.h` – PN532 bring‑up and tag polling API
  - `provisioning.h` – provisioning, tag list, and crypto helper APIs
- `src/` – implementations
  - `main.cpp` – orchestrates startup (LED task, NFC, provisioning, BLE)
  - `nfc.cpp` – PN532 (I²C) setup, robust recovery, tag polling
  - `provisioning.cpp` – ECC keypair (mbedTLS), cert storage, tag management, signing/verification helpers
  - `ble.cpp` – BLE server with AUTH service (Phase B) + Admin service

---

## Phase A — NFC provisioning (what it does)

- ESP32‑S3 generates an ECC P‑256 keypair and stores the private key in NVS (non‑volatile storage)
- The phone provides its long‑term public key (or certificate) and the ECU stores it in NVS
- Optional: enroll NFC tag UIDs that you can later manage over BLE Admin

Key points in code:
- ECC keypair: mbedTLS (see `Provisioning::begin()`)
- Long‑term phone key currently expected as a PEM public key stored under NVS key `"cert"`
- Tag management stored in a compact blob under NVS key `"tags"` (legacy single‑tag `"tag"` handled too)

Console helpers:
- `Provisioning::printInfo()` prints a fingerprint of the ECU public key and any enrolled tags

---

## Phase B — BLE mutual authentication (how it works)

Service: AUTH (UUID `d0d0d0d0-0000-4000-8000-d0d0d0d00000`)

Characteristics:
- `ClientHello` (Write, `...0001`): phone sends its ephemeral public key + ECDSA signature (using phone long‑term key)
- `ServerHello` (Read/Notify, `...0002`): ECU returns its ephemeral public key + ECDSA signature (using ECU long‑term key)
- `ClientConfirm` (Write, `...0003`): phone sends HMAC proof using derived K_mac
- `ServerConfirm` (Read/Notify, `...0004`): ECU sends HMAC proof using derived K_mac

Step‑by‑step:
1) Ephemeral keys: both sides create fresh P‑256 keypairs (not reused)
2) Signed key exchange: each side signs its ephemeral public key with its long‑term private key
3) Verify: each side verifies the peer’s signature using the long‑term public key saved in Phase A
4) ECDH: both compute the same secret S = d_self × Q_peer
5) HKDF: derive two session keys (32 bytes each)
   - `K_enc = HKDF(S, info="BLE-AUTH" || cliPub || srvPub)`
   - `K_mac = HKDF(S, info="BLE-AUTH-MAC" || cliPub || srvPub)`
6) HMAC confirm: each side computes an HMAC over a small transcript to prove it has `K_mac`
   - Client → ECU: HMAC(K_mac, "AUTH-PROOF" || cliPub || srvPub)
   - ECU → Client: HMAC(K_mac, "AUTH-OK"    || srvPub || cliPub)
7) Done: if both confirms match, the session is ready (`BLEMod::isSessionReady()` is true on ECU)

Message formats (simple, length‑prefixed):
- ClientHello (Write to `ClientHello`):
  - `[1] version=0x01`
  - `[2] LE client_pub_len` (usually 65 bytes for uncompressed P‑256)
  - `[client_pub_len] client_pub` (0x04 || X || Y)
  - `[2] LE sig_len`
  - `[sig_len] ECDSA‑DER signature over client_pub`
- ServerHello mirrors the same layout with server pub/signature
- Confirms are fixed 32‑byte HMAC‑SHA256 values

Assumptions right now:
- The phone’s long‑term public key is stored as a PEM public key in NVS (`"cert"`). If you instead store a full certificate, we can add x509 parsing to extract the public key.

---

## BLE Admin service (for maintenance)

Service: Admin (UUID `9a9b9c9d-0000-4000-8000-9a9b9c9d0000`)

- `AdminMode` (R/W, `...0001`): 0=normal, 1=enroll, 2=remove
- `AdminCmd` (Write, `...0002`): small command bytes
  - `0x01` clear keys (ECU keypair + cert only)
  - `0x02` clear all (keys + cert + tag list)
  - `0x10` list tags (prints to serial; short status in AdminInfo)
- `AdminInfo` (R/Notify, `...0003`): status strings like READY, CLEARED_ALL, etc.

Tip: Use AdminMode ENROLL/REMOVE and present a tag to add/remove it from the authorized list.

---

## Building, flashing, and serial monitor

Prereqs:
- VS Code + PlatformIO extension, or PlatformIO Core

Build:
- Use the VS Code task “PlatformIO: Build” for the workspace

Upload:
- Use your normal PlatformIO upload task for `env:yolo_uno` (board requires a COM port)
- Serial monitor is configured at 115200 with time stamps, and tries to follow the device even if Windows re‑maps COM ports

If you prefer command line (optional):

```powershell
# From the iot/ folder
pio run
pio run --target upload --upload-port COM9
pio device monitor
```

Troubleshooting:
- If PN532 doesn’t respond, the firmware will scan I²C and try recovery (re‑init bus, optional HW reset, re‑begin). Check SDA/SCL pins and supply.
- If BLE handshake fails at signature verification, ensure the phone’s public key is stored in NVS as a PEM public key under `cert`.
- Make sure your app requests a large BLE MTU (>= 185) so the hello messages fit comfortably.

---

## NFC details

- I²C pins: SDA=11, SCL=12 (see `nfc.cpp`)
- PN532 typical addresses: 0x24 (7‑bit), some boards appear as 0x48. Code scans once and picks the best it finds
- Robustness: if reads start failing, the code probes firmware, attempts bus recovery, and re‑initializes
- The NFC task throttles duplicate UID logs to keep serial output readable

---

## Security notes and future enhancements

What’s already good:
- Long‑term private keys never leave devices
- Ephemeral keys per session + ECDH → forward secrecy
- Mutual proof via signed ephemerals and HMAC confirmation

Recommended next steps:
- Store the phone’s public key separately (e.g., NVS key `phone_pub`) and optionally keep the cert for audit
- Add x509 parsing to extract the public key when a full certificate is provided
- Include an explicit challenge/nonce or full transcript hash in the confirm step for stronger binding
- Add an encrypted BLE characteristic to test K_enc quickly (e.g., request/response protected by K_mac)
- Integrate UWB later using the same session keys

---

## Quick developer guide

- Entry point: `src/main.cpp`
  - Starts LED heartbeat task (GPIO 48), NFC polling task, provisioning, and BLE server
- NFC module: `src/nfc.cpp`
  - `NFCMod::begin()` sets up I²C + PN532; `startTask()` begins tag polling
- Provisioning module: `src/provisioning.cpp`
  - `Provisioning::begin()` ensures ECC keypair
  - `signWithDeviceKey()` and `verifyWithPhoneKey()` used by BLE AUTH
- BLE module: `src/ble.cpp`
  - AUTH service implements the mutual authentication handshake
  - `BLEMod::isSessionReady()` tells you when the secure session is established

Happy hacking — and if you want me to generate a tiny encrypted echo characteristic to validate K_enc/K_mac on both sides, say the word and I’ll add it.
