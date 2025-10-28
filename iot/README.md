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

## Phase A — NFC provisioning (HCE protocol)

This phase uses real NFC over ISO‑DEP (ISO 14443‑4). The ECU (ESP32‑S3 + PN532) is the reader/initiator; the phone runs an Android HCE app that behaves like a smartcard.

Overview
- Roles
  - ECU (ESP32‑S3 + PN532): NFC reader/initiator, emits RF field and sends C‑APDUs
  - Phone (Android): NFC card/target via Host Card Emulation (HCE), receives C‑APDUs and returns R‑APDUs
- Transport: APDU over ISO‑DEP (T=CL)
- Scope: Entire provisioning exchange happens on 13.56 MHz NFC; no BLE/Wi‑Fi/internet needed for provisioning

System architecture (layers)
- Physical: PN532 + antenna (ECU) ↔ phone NFC controller/antenna
- Transport: ISO‑DEP; APDU frames carried by the NFC stacks (PN532 driver ↔ Android NFC)
- Application: ECU firmware sends/receives APDUs; Android HCE service implements APDU logic
- Logic: ECU issues commands (SELECT, GET_INFO, CHALLENGE); phone returns public key, optional cert chain, keyId, and signatures

End‑to‑end flow
1) Phone enters HCE range
   - ECU enables RF field; Android detects field and activates HCE for matching AID
2) ECU selects the app via SELECT AID
   - C‑APDU (example): `00 A4 04 00 Lc <AID> 00`
   - Success R‑APDU: `90 00`
3) ECU requests provisioning data (custom INS)
   - Example C‑APDU layout:
     - CLA=00, INS=CA (GET_DATA/custom), P1=00, P2=00, Lc=N
     - Data may include `vehicleId` and a fresh `nonce`
4) Phone HCE handles the request
   - HCE parses APDU, validates request
   - Fetches or generates keypair (prefer Android Keystore)
   - Optionally signs the ECU’s challenge (nonce)
   - Returns payload `{ keyId | publicKey | certLen | cert... | signature? }` with `90 00`
5) ECU stores pairing
   - ECU parses R‑APDU, extracts `keyId`, `publicKey`, optional `certChain`
   - Persists in NVS (with timestamp and optional phone NFC UID)

Recommended APDUs (proposal)
- SELECT AID
  - C‑APDU: `00 A4 04 00 Lc <AID> 00`
  - R‑APDU: `90 00`
- GET_INFO (custom) — phone identity and proof
  - C‑APDU: `00 CA 00 00 Lc { vehicleId(8) | challenge(8..32) }`
  - R‑APDU: `{ keyIdLen(1) | keyId | pubKey(65) | certLen(2) | cert... | sigLen(2)? | sigDER? } 90 00`
    - `pubKey` in uncompressed P‑256 form (0x04 || X || Y) = 65 bytes
    - `cert` can be empty for demo; for large chains, add fragmentation (below)
    - `signature` is optional here (can be moved to unlock flow), but recommended when you need attestation

Fragmentation
- Keep a single APDU payload < 255 bytes. For larger data (e.g., cert chains), implement a simple sequence:
  - ECU sends GET_INFO(P1=seqIndex); phone returns piece + `90 00` until done
  - Or use the ISO7816 GET RESPONSE pattern if your driver supports it

Android HCE implementation (sketch)
- Manifest service:
  - Declare `HostApduService` with `android.permission.BIND_NFC_SERVICE`
  - Add `res/xml/apduservice.xml` mapping your AID(s)
  - Consider `android:requireDeviceUnlock="true"` if you need the device unlocked
- HostApduService:
  - Implement `processCommandApdu(byte[] apdu, Bundle extras)`
  - Switch on INS; for GET_INFO: assemble `{keyId, pubKey, certChain, signature?}` then return data || `0x90 0x00`
  - Implement `onDeactivated(int reason)` for cleanup
- Key management:
  - Prefer Android Keystore (hardware‑backed when available). Check `KeyInfo.isInsideSecureHardware()` when needed

ECU firmware notes (PN532 + ESP32)
- Link: I²C/SPI/UART; I²C is common on ESP32‑S3
- Use initiator + ISO‑DEP support in your PN532 driver to exchange raw APDUs:
  - `inDataExchange(selectAidApdu, ...) → 90 00`
  - `inDataExchange(getInfoApdu, ...) → payload || 90 00`
- Handle timeouts and deactivations (deselect/link loss) and restart polling gracefully

Persistence format (example)
```
{
  "vehicleId": "...",
  "keyId": "...",
  "publicKey": "PEM or raw",
  "certChain": "PEM (optional)",
  "phoneNfcUid": "... (optional)",
  "timestamp": 1690000000
}
```

Security and risks
- HCE is software‑based; use Keystore (hardware‑backed if available) for stronger protections
- Use a fresh challenge/nonce in provisioning to prevent replay
- Don’t rely on NFC UID for security — it can be spoofed on some devices

Throughput and practical limits
- Data rate typically 106 kbps (can negotiate 212/424)
- Single APDU payload < 255 bytes; fragment larger payloads
- P‑256 uncompressed public key is 65 bytes — fits comfortably in one APDU without certs

Checklist (both sides)
- Register AID in the Android `apduservice.xml`
- Implement PN532 initiator + ISO‑DEP APDU exchange on ECU
- Define INS, data formats, and status words (e.g., `90 00`, `6D 00`)
- Test: SELECT AID → `90 00`; GET_INFO → returns pubKey and metadata; verify failure cases

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

## Python demo: BLE mutual auth and Admin

The repo includes a Python helper (`tools/demo_ble_auth.py`) that lets you:

- Perform the full BLE mutual-auth handshake and run a secure AES‑GCM echo
- Upload the phone’s public key (PEM) to the ECU via the Admin service
- Enroll/remove/list NFC tags via Admin modes

### Prerequisites

- Python 3.10+
- Install dependencies from the project root (iot/):

```powershell
pip install -r tools/requirements.txt
```

By default the script looks for a device named `ESP-Smart-Car-ECU` and a key file `phone_key.pem` in the current folder. The first run generates `phone_key.pem` automatically if it doesn’t exist.

### CLI overview

```text
python tools\demo_ble_auth.py [options]

Options:
  --device-name NAME     BLE device name to connect (default: ESP-Smart-Car-ECU)
  --keyfile PATH         Path to phone long-term private key PEM (default: phone_key.pem)

  --upload-key           Upload phone public key PEM to ECU (Admin service), then exit

  --enroll               Set Admin mode to ENROLL for ~10s; present a tag to add it
  --remove               Set Admin mode to REMOVE for ~10s; present an authorized tag to remove it
  --list-tags            Ask ECU to print the authorized tag list (to serial/log)

No flags → perform BLE mutual-auth handshake + secure echo test.
```

### Typical workflows

1) First-time setup: upload the phone public key once, then perform handshake

```powershell
# Upload public key (derived from --keyfile). Stores in ECU NVS.
python tools\demo_ble_auth.py --upload-key

# Start BLE mutual-auth handshake and run secure echo
python tools\demo_ble_auth.py
```

2) Admin: enroll/remove/list NFC tags

```powershell
# Enroll window (~10s); present a tag
python tools\demo_ble_auth.py --enroll --list-tags

# Remove window (~10s); present an authorized tag
python tools\demo_ble_auth.py --remove --list-tags

# Just request the device to list tags (prints to serial/log)
python tools\demo_ble_auth.py --list-tags
```

3) Device selection and custom key path

```powershell
# If your device advertises a different name
python tools\demo_ble_auth.py --device-name My-ECU

# Use a different private key PEM
python tools\demo_ble_auth.py --keyfile .\my_phone_key.pem
```

### Notes and troubleshooting (Python)

- The script prints your phone public key PEM at startup; that’s what the ECU stores in NVS.
- On Windows, long BLE writes are chunked internally to avoid ATT/MTU issues.
- If the device isn’t found, ensure it’s advertising and the `--device-name` matches.
- Keep the device close to the PC during BLE operations to reduce packet loss.

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
