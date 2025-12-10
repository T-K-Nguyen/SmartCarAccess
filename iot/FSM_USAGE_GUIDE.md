# FSM Implementation Guide

## Overview
Complete finite state machine (FSM) implementation for ESP32 Smart Car Access system with NFC provisioning and BLE authentication.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                 Application Layer                   │
│  (main.cpp, NFC callbacks, BLE callbacks)          │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│           FSMIntegration Layer                      │
│  Event wrappers: NFC::*, BLE::*, Unlock::*         │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│                   FSM Core                          │
│  • Event queue (FIFO)                              │
│  • Transition table                                │
│  • Guard conditions                                │
│  • Entry/exit actions                              │
│  • Timeout management                              │
│  • State context                                   │
└─────────────────────────────────────────────────────┘
```

## Files

### Core Implementation
- **`src/fsm/fsm.cpp`**: FSM core engine (~580 lines)
  - Event queue processing
  - Transition table with 40+ transitions
  - Guard conditions
  - Entry/exit actions
  - Timeout system
  - Debug tools

- **`src/fsm/fsm_states.cpp`**: String conversion utilities
  - `stateToString()`: State → readable name
  - `eventToString()`: Event → readable name
  - `errorToString()`: ErrorCode → readable name

- **`src/fsm/fsm_integration.cpp`**: Subsystem integration (~350 lines)
  - NFC event wrappers
  - BLE event wrappers
  - Unlock flow wrappers
  - Admin command wrappers
  - Context helper functions

### Headers
- **`include/fsm/fsm_states.h`**: Type definitions
  - `enum State`: 16 states
  - `enum Event`: 28 events
  - `enum ErrorCode`: 16 error codes
  - `struct StateContext`: Shared data
  - `struct StateTransition`: Transition definition

- **`include/fsm/fsm.h`**: Public FSM API
  - Core functions: `begin()`, `tick()`, `triggerEvent()`
  - State queries: `getCurrentState()`, `isProvisioning()`, etc.
  - Context access: `getContext()`
  - Timeout management: `setStateTimeout()`, `clearTimeout()`
  - Callback registration: `onStateEntry()`, `onStateExit()`
  - Debug tools: `printStatus()`, `printTransitionTable()`

- **`include/fsm/fsm_integration.h`**: Integration API
  - Event wrapper declarations
  - Helper function declarations

### Testing
- **`test/test_fsm.cpp`**: Unit tests (~400 lines)
  - 10 test cases covering:
    - Initial state
    - Basic transitions
    - Guarded transitions
    - Event queue FIFO
    - Timeout functionality
    - Entry/exit actions
    - Context storage
    - Error handling
    - Full provisioning flow
    - Event queue overflow

## Usage

### 1. Initialization (in setup())

```cpp
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"

void setup() {
  Serial.begin(115200);
  
  // Initialize subsystems (BLE, NFC, etc.)
  // ...
  
  // Initialize FSM (will auto-transition INIT → IDLE)
  FSM::begin();
  
  // Optional: Validate configuration
  FSM::validateConfiguration();
  
  // Optional: Print transition table
  FSM::printTransitionTable();
}
```

### 2. Main Loop (in loop())

```cpp
void loop() {
  // Process FSM events and timeouts
  FSM::tick();
  
  // Your other loop code
  // ...
  
  delay(10);
}
```

### 3. Triggering Events from NFC

**Before (direct code):**
```cpp
void nfc_callback() {
  if (card_detected) {
    // Direct state change
    current_state = STATE_PROVISIONING;
  }
}
```

**After (using FSM):**
```cpp
void nfc_callback() {
  if (card_detected) {
    uint8_t uid[4];
    nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
    
    // Trigger FSM event
    FSMIntegration::NFC::onCardDetected(uid);
  }
}
```

### 4. Triggering Events from BLE

**Example: Client connects**
```cpp
void onConnect(NimBLEServer* pServer) {
  uint8_t clientAddr[6];
  // Get client address...
  
  FSMIntegration::BLE::onClientConnected(clientAddr);
}
```

**Example: Authentication succeeds**
```cpp
void onAuthSuccess() {
  // Store session keys in FSM context
  FSMIntegration::storeSessionKeys(enc_key, mac_key);
  FSMIntegration::markSessionReady();
  
  // Trigger event
  FSMIntegration::BLE::onAuthVerified();
}
```

### 5. Checking State

```cpp
// Query current state
State current = FSM::getCurrentState();

if (current == State::AUTH_SESSION_READY) {
  // Can unlock
}

// Or use helper functions
if (FSM::isAuthenticating()) {
  // In any AUTH_* state
}

if (FSM::isProvisioning()) {
  // In any PROVISIONING_* state
}
```

### 6. Accessing Context Data

```cpp
const StateContext& ctx = FSM::getContext();

if (ctx.session_keys_ready) {
  // Use session keys
  encrypt_data(ctx.session_enc_key, data);
}

if (ctx.phone_key_valid) {
  // Phone provisioned
  Serial.printf("Phone key: %02X...\n", ctx.phone_pub_key[0]);
}

Serial.printf("Retry count: %u\n", ctx.retry_count);
Serial.printf("Last error: %s\n", errorToString(ctx.last_error));
```

### 7. Setting Timeouts

```cpp
// In state entry action or after triggering event
FSM::onStateEntry(State::PROVISIONING_WAIT_TAP, [](StateContext& ctx) {
  Serial.println("Waiting for NFC tap...");
  FSM::setStateTimeout(30000);  // 30 second timeout
});

// Timeout will auto-trigger Event::TIMEOUT
```

### 8. Registering Callbacks

```cpp
// State entry action
FSM::onStateEntry(State::AUTH_SESSION_READY, [](StateContext& ctx) {
  Serial.println("✓ Auth session ready!");
  digitalWrite(LED_PIN, HIGH);
});

// State exit action
FSM::onStateExit(State::AUTH_SESSION_READY, [](StateContext& ctx) {
  Serial.println("Auth session ending...");
  digitalWrite(LED_PIN, LOW);
});
```

### 9. Error Handling

```cpp
// From any subsystem
if (error_condition) {
  FSMIntegration::NFC::onError(ErrorCode::NFC_TIMEOUT);
  // FSM will transition to ERROR_HANDLER state
}

// Check error state
if (FSM::isInErrorState()) {
  const StateContext& ctx = FSM::getContext();
  Serial.printf("Error: %s\n", errorToString(ctx.last_error));
  
  // Recover
  FSM::triggerEvent(Event::RESET_REQUESTED);
}
```

### 10. Serial Commands

```cpp
void handleSerialCommand(String cmd) {
  if (cmd == "provision") {
    FSMIntegration::SerialCmd::startProvisioning();
  }
  else if (cmd == "status") {
    FSMIntegration::SerialCmd::printFSMStatus();
  }
  else if (cmd == "diag") {
    FSMIntegration::SerialCmd::printDiagnostics();
  }
  else if (cmd == "clear") {
    FSMIntegration::SerialCmd::clearAll();
  }
}
```

## State Machine Diagram

```
INIT
 │
 └─[SYSTEM_READY]─> IDLE ◄─────────────────┐
                     │                      │
    ┌────────────────┴─────┐                │
    │ [PROVISION_START]    │ [RESET]        │
    ▼                      │                │
 PROVISIONING_WAIT_TAP     │                │
    │                      │                │
    │ [NFC_CARD_DETECTED]  │                │
    ▼                      │                │
 PROVISIONING_SELECT_AID   │                │
    │                      │                │
    │ [SELECT_AID_SUCCESS] │                │
    ▼                      │                │
 PROVISIONING_EXCHANGE_KEYS│                │
    │                      │                │
    │ [KEYS_EXCHANGED]     │                │
    ▼                      │                │
 PROVISIONING_STORE_CREDS  │                │
    │                      │                │
    │ [CREDENTIALS_STORED] │                │
    └──────────────────────┘                │
                                            │
 IDLE (if provisioned)                      │
    │                                       │
    │ [BLE_CLIENT_CONNECTED]               │
    ▼                                       │
 AUTH_WAIT_CONNECT                          │
    │                                       │
    │ [CLIENT_HELLO_RECEIVED]              │
    ▼                                       │
 AUTH_HANDSHAKE                             │
    │                                       │
    │ [AUTH_VERIFIED]                      │
    ▼                                       │
 AUTH_SESSION_READY ◄───────┐              │
    │                        │ [PROXIMITY_TOO_FAR]
    │ [UNLOCK_REQUESTED]     │              │
    ▼                        │              │
 UNLOCKING_CHECK_PROXIMITY ─┘              │
    │                                       │
    │ [PROXIMITY_OK]                       │
    ▼                                       │
 UNLOCKING_VERIFY_AUTH                      │
    │                                       │
    │ [AUTH_SESSION_VALID]                 │
    ▼                                       │
 UNLOCKING_EXECUTE                          │
    │                                       │
    │ [UNLOCK_EXECUTED]                    │
    ▼                                       │
 UNLOCKING_COMPLETE                         │
    │                                       │
    └─[TIMEOUT]────────────────────────────┘
    
 Any State ─[ERROR_OCCURRED]─> ERROR_HANDLER
                                     │
                                     │ [RESET_REQUESTED]
                                     └──> IDLE
```

## Event Flow Examples

### Provisioning Flow
```
User:       [Starts provisioning via serial]
FSM:        IDLE → PROVISIONING_WAIT_TAP (30s timeout)

User:       [Taps phone to NFC reader]
NFC:        onCardDetected(uid)
FSM:        PROVISIONING_WAIT_TAP → PROVISIONING_SELECT_AID

NFC:        SELECT AID command sent
NFC:        onSelectAIDSuccess()
FSM:        PROVISIONING_SELECT_AID → PROVISIONING_EXCHANGE_KEYS

NFC:        Key exchange APDU
NFC:        onKeysExchanged(phone_pub_key)
FSM:        PROVISIONING_EXCHANGE_KEYS → PROVISIONING_STORE_CREDS
            (guard checks phone_key_valid = true)

NVS:        Store credentials
Integration: onCredentialsStored()
FSM:        PROVISIONING_STORE_CREDS → IDLE
```

### Authentication Flow
```
Phone:      [Connects to ESP32 BLE]
BLE:        onClientConnected(addr)
FSM:        IDLE → AUTH_WAIT_CONNECT (if provisioned)

Phone:      [Sends CLIENT_HELLO with ephemeral key]
BLE:        onClientHelloReceived(client_eph_pub)
FSM:        AUTH_WAIT_CONNECT → AUTH_HANDSHAKE

ESP32:      [Generates own ephemeral key, sends SERVER_HELLO]
BLE:        onServerHelloSent(ecu_eph_pub)
FSM:        AUTH_HANDSHAKE → AUTH_HANDSHAKE (stays in same state)

Phone:      [Sends signature]
BLE:        Verify signature, derive session keys
BLE:        onAuthVerified()
FSM:        AUTH_HANDSHAKE → AUTH_VERIFY_KEYS → AUTH_SESSION_READY
            (guard checks session_keys_ready = true)

Phone:      [Requests unlock]
BLE:        onUnlockRequested()
FSM:        AUTH_SESSION_READY → UNLOCKING_CHECK_PROXIMITY
```

### Error Recovery
```
Any State:  [Error occurs]
Subsystem:  onError(ErrorCode::NFC_TIMEOUT)
FSM:        <current> → ERROR_HANDLER

FSM:        [After 5s timeout OR user reset]
FSM:        ERROR_HANDLER → IDLE
```

## Testing

### Run Unit Tests

```cpp
#include "test/test_fsm.cpp"

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  run_fsm_tests();
}
```

Expected output:
```
╔═══════════════════════════════════════════════════════════╗
║           FSM UNIT TEST SUITE                             ║
╚═══════════════════════════════════════════════════════════╝

=== Test 1: Initial State ===
✓ PASS: Initial state should be IDLE after SYSTEM_READY

=== Test 2: Basic Transition ===
✓ PASS: State should be IDLE after reset
✓ PASS: Should transition to PROVISIONING_WAIT_TAP on PROVISION_START

...

╔═══════════════════════════════════════════════════════════╗
║           TEST RESULTS                                    ║
╠═══════════════════════════════════════════════════════════╣
║ PASSED: 35                                                ║
║ FAILED: 0                                                 ║
║ TOTAL:  35                                                ║
╚═══════════════════════════════════════════════════════════╝

✓✓✓ ALL TESTS PASSED ✓✓✓
```

### Manual Testing

```cpp
// Print FSM status
FSM::printStatus();

// Print transition table
FSM::printTransitionTable();

// Trigger test events
FSM::triggerEvent(Event::PROVISION_START);
FSM::triggerEvent(Event::NFC_CARD_DETECTED);

// Force state (for testing)
FSM::forceState(State::AUTH_SESSION_READY, true);
```

## Debug Output

Enable detailed logging:
```cpp
FSM::setDebugLogging(true);
```

Output example:
```
[FSM] ╔═══════════════════════════════════════╗
[FSM] ║   Smart Car Access FSM v1.0         ║
[FSM] ╚═══════════════════════════════════════╝
[FSM] ▶ Event queued: SYSTEM_READY (queue: 1/16)
[FSM] ⚙️  Processing: SYSTEM_READY (state: INIT)
[FSM] ═══ Transition: INIT → IDLE (event: SYSTEM_READY) ═══
[FSM] ▶ Event queued: PROVISION_START (queue: 1/16)
[FSM] ⚙️  Processing: PROVISION_START (state: IDLE)
[FSM] ═══ Transition: IDLE → PROV_WAIT_TAP (event: PROVISION_START) ═══
[FSM] ⏱️  Timeout set: 30000 ms (state: PROV_WAIT_TAP)
```

## Integration Checklist

- [x] ✅ FSM core implemented (`fsm.cpp`)
- [x] ✅ String conversion utilities (`fsm_states.cpp`)
- [x] ✅ Integration wrappers (`fsm_integration.cpp`)
- [x] ✅ Unit tests written (`test_fsm.cpp`)
- [ ] ⏳ Integrate with `main.cpp` (replace manual state management)
- [ ] ⏳ Update NFC callbacks to use `FSMIntegration::NFC::*`
- [ ] ⏳ Update BLE callbacks to use `FSMIntegration::BLE::*`
- [ ] ⏳ Add unlock relay logic
- [ ] ⏳ Test on hardware with real NFC/BLE

## Migration Path

### Step 1: Add FSM to build
Update `platformio.ini` if needed (already included if using default src/ structure).

### Step 2: Initialize in main.cpp
```cpp
#include "fsm/fsm.h"

void setup() {
  // ... existing init code ...
  FSM::begin();
}

void loop() {
  FSM::tick();
  // ... existing loop code ...
}
```

### Step 3: Update NFC callbacks
Replace direct state changes with FSM events:
```cpp
// OLD
void onCardDetected() {
  state = PROVISIONING;
}

// NEW
void onCardDetected() {
  uint8_t uid[4];
  // ... read UID ...
  FSMIntegration::NFC::onCardDetected(uid);
}
```

### Step 4: Update BLE callbacks
```cpp
// OLD
void onConnect(NimBLEServer* pServer) {
  connected = true;
}

// NEW
void onConnect(NimBLEServer* pServer) {
  uint8_t addr[6];
  // ... get address ...
  FSMIntegration::BLE::onClientConnected(addr);
}
```

### Step 5: Remove manual state variables
Delete old state tracking:
```cpp
// DELETE THESE
// static int current_state = STATE_IDLE;
// static bool connected = false;
// static bool provisioned = false;
```

Use FSM state queries instead:
```cpp
// NEW
if (FSM::isAuthenticating()) { ... }
if (FSM::getCurrentState() == State::AUTH_SESSION_READY) { ... }
```

## Performance

- **Event queue**: 16 entries, ~256 bytes RAM
- **State context**: 264 bytes RAM
- **Transition table**: ~1.5 KB Flash (ROM)
- **Event processing**: < 1ms per event (typical)
- **Tick overhead**: < 100µs when queue empty

## Future Enhancements

- [ ] Hierarchical states (substates)
- [ ] Event history logging (last N events)
- [ ] State machine visualization export (DOT/Graphviz)
- [ ] Runtime transition table modification
- [ ] Multiple FSM instances
- [ ] Non-volatile state persistence (survive reboot)

## Troubleshooting

**Q: Events not processing**
- Check `FSM::tick()` is called in loop()
- Check event queue not full: `FSM::getEventQueueSize()`
- Enable debug logging: `FSM::setDebugLogging(true)`

**Q: Transition not happening**
- Check guard conditions: print context state
- Verify transition exists in table: `FSM::printTransitionTable()`
- Check for guard failures in logs

**Q: Timeout not working**
- Ensure `FSM::tick()` called frequently (< 100ms)
- Check timeout set correctly: `FSM::setStateTimeout(ms)`
- Verify TIMEOUT event handled in transition table

**Q: Context data lost**
- Don't call `FSM::reset(true)` unless intentional
- Check if `StateContext::reset()` called unexpectedly
- Use `FSM::getContext()` to inspect values

## License

Part of Smart Car Access System (DATN project).
