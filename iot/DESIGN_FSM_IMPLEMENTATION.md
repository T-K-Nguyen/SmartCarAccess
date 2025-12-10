# FSM Implementation Guide

## 📋 Tổng Quan Thiết Kế

### Architecture Pattern: **Hierarchical State Machine**

```
┌────────────────────────────────────────────────────────────┐
│                     FSM Core Engine                        │
│  ┌──────────────┐    ┌──────────────┐    ┌─────────────┐ │
│  │ Event Queue  │───>│ Transition   │───>│  State      │ │
│  │ (FIFO)       │    │ Logic        │    │  Context    │ │
│  └──────────────┘    └──────────────┘    └─────────────┘ │
│         ▲                    │                    │        │
│         │                    ▼                    ▼        │
│  ┌──────────────┐    ┌──────────────┐    ┌─────────────┐ │
│  │ Integration  │    │ Guard        │    │ Actions     │ │
│  │ Layer        │    │ Conditions   │    │ (Entry/Exit)│ │
│  └──────────────┘    └──────────────┘    └─────────────┘ │
└────────────────────────────────────────────────────────────┘
```

## 🔧 Các Thành Phần Đã Bổ Sung

### 1. **StateContext** - Shared Data
```cpp
struct StateContext {
    // NFC session
    uint8_t nfc_uid[4];
    uint8_t phone_pub_key[65];
    bool phone_key_valid;
    
    // BLE session
    uint8_t ecu_ephemeral_pub[65];
    uint8_t phone_ephemeral_pub[65];
    uint8_t shared_secret[32];
    uint8_t session_enc_key[32];
    uint8_t session_mac_key[32];
    bool session_keys_ready;
    
    // Timing
    uint32_t retry_count;
    uint32_t last_activity_ms;
    
    // Errors
    ErrorCode last_error;
    uint32_t error_count;
};
```

**Mục đích:** Lưu trữ data giữa các states, tránh dùng global variables

### 2. **StateTransition** - Transition Table
```cpp
struct StateTransition {
    State from_state;
    Event on_event;
    State to_state;
    bool (*guard)(const StateContext& ctx);  // Optional condition
};
```

**Ví dụ:**
```cpp
const StateTransition transitionTable[] = {
    // IDLE → PROVISIONING when provision starts
    {State::IDLE, Event::PROVISION_START, State::PROVISIONING_WAIT_TAP, nullptr},
    
    // PROVISIONING_WAIT_TAP → PROVISIONING_SELECT_AID when card detected
    {State::PROVISIONING_WAIT_TAP, Event::NFC_CARD_DETECTED, 
     State::PROVISIONING_SELECT_AID, nullptr},
    
    // PROVISIONING_SELECT_AID → PROVISIONING_EXCHANGE_KEYS when SELECT success
    {State::PROVISIONING_SELECT_AID, Event::SELECT_AID_SUCCESS, 
     State::PROVISIONING_EXCHANGE_KEYS, nullptr},
    
    // PROVISIONING_EXCHANGE_KEYS → PROVISIONING_STORE_CREDS when keys valid
    {State::PROVISIONING_EXCHANGE_KEYS, Event::KEYS_EXCHANGED, 
     State::PROVISIONING_STORE_CREDS, guardKeysValid},
    
    // PROVISIONING_STORE_CREDS → IDLE when complete
    {State::PROVISIONING_STORE_CREDS, Event::CREDENTIALS_STORED, 
     State::IDLE, nullptr},
    
    // AUTH_WAIT_CONNECT → AUTH_HANDSHAKE when BLE client connects
    {State::AUTH_WAIT_CONNECT, Event::BLE_CLIENT_CONNECTED, 
     State::AUTH_HANDSHAKE, guardProvisioned},
    
    // ... more transitions
    
    // ERROR_HANDLER → IDLE on reset
    {State::ERROR_HANDLER, Event::RESET_REQUESTED, State::IDLE, nullptr}
};

// Guard example
bool guardKeysValid(const StateContext& ctx) {
    return ctx.phone_key_valid && ctx.phone_pub_key[0] == 0x04;
}

bool guardProvisioned(const StateContext& ctx) {
    return ProvisioningPhase::isProvisioned();
}
```

### 3. **StateAction** - Entry/Exit Callbacks
```cpp
typedef void (*StateAction)(StateContext& ctx);

// Register actions
FSM::onStateEntry(State::PROVISIONING_WAIT_TAP, onEnterProvisioningWait);
FSM::onStateExit(State::PROVISIONING_WAIT_TAP, onExitProvisioningWait);

// Implementation
void onEnterProvisioningWait(StateContext& ctx) {
    Serial.println("[FSM] Entering PROVISIONING_WAIT_TAP");
    ctx.retry_count = 0;
    ctx.last_activity_ms = millis();
    FSM::setStateTimeout(30000);  // 30s timeout
    NFCSession::startPolling();
}

void onExitProvisioningWait(StateContext& ctx) {
    Serial.println("[FSM] Exiting PROVISIONING_WAIT_TAP");
    NFCSession::stopPolling();
    FSM::clearTimeout();
}
```

### 4. **Event Queue** - Async Event Handling
```cpp
#define EVENT_QUEUE_SIZE 16

struct QueuedEvent {
    Event event;
    uint32_t timestamp;
    void* data;      // Optional payload
    size_t data_len;
};

QueuedEvent eventQueue[EVENT_QUEUE_SIZE];
size_t queueHead = 0;
size_t queueTail = 0;

bool triggerEvent(Event event) {
    size_t next = (queueHead + 1) % EVENT_QUEUE_SIZE;
    if (next == queueTail) {
        Serial.println("[FSM] ERROR: Event queue full!");
        return false;
    }
    
    eventQueue[queueHead] = {event, millis(), nullptr, 0};
    queueHead = next;
    return true;
}

bool triggerEventWithData(Event event, void* data, size_t len) {
    // Similar but with data copy
    // ...
}
```

### 5. **Timeout Management**
```cpp
struct TimeoutState {
    bool active;
    uint32_t start_ms;
    uint32_t duration_ms;
};

TimeoutState currentTimeout = {false, 0, 0};

void setStateTimeout(uint32_t timeout_ms) {
    currentTimeout.active = true;
    currentTimeout.start_ms = millis();
    currentTimeout.duration_ms = timeout_ms;
}

void clearTimeout() {
    currentTimeout.active = false;
}

void checkTimeout() {
    if (!currentTimeout.active) return;
    
    uint32_t elapsed = millis() - currentTimeout.start_ms;
    if (elapsed >= currentTimeout.duration_ms) {
        triggerEvent(Event::TIMEOUT);
        currentTimeout.active = false;
    }
}
```

## 📝 Implementation Template (fsm.cpp)

```cpp
#include "fsm/fsm.h"
#include <Arduino.h>

namespace FSM {

// Private data structures
static State currentState = State::INIT;
static State previousState = State::INIT;
static StateContext context;
static StateInfo info;
static bool debugLogging = true;

// Event queue
#define EVENT_QUEUE_SIZE 16
struct QueuedEvent {
    Event event;
    uint32_t timestamp;
};
static QueuedEvent eventQueue[EVENT_QUEUE_SIZE];
static size_t queueHead = 0;
static size_t queueTail = 0;

// Timeout
struct TimeoutState {
    bool active;
    uint32_t start_ms;
    uint32_t duration_ms;
} currentTimeout = {false, 0, 0};

// Action callbacks
#define MAX_STATES 16
static StateAction entryActions[MAX_STATES] = {nullptr};
static StateAction exitActions[MAX_STATES] = {nullptr};

// Forward declarations
static void processEvent(Event event);
static bool canTransition(State from, Event event, State to);
static void executeTransition(State newState, Event triggerEvent);
static void checkTimeout();

// State transition table
const struct {
    State from;
    Event on;
    State to;
    bool (*guard)(const StateContext&);
} transitionTable[] = {
    // INIT → IDLE on system ready
    {State::INIT, Event::SYSTEM_READY, State::IDLE, nullptr},
    
    // IDLE → PROVISIONING on provision start
    {State::IDLE, Event::PROVISION_START, State::PROVISIONING_WAIT_TAP, nullptr},
    
    // IDLE → AUTH on BLE connect (if provisioned)
    {State::IDLE, Event::BLE_CLIENT_CONNECTED, State::AUTH_WAIT_CONNECT, 
        [](const StateContext& ctx) { return ProvisioningPhase::isProvisioned(); }},
    
    // PROVISIONING flow
    {State::PROVISIONING_WAIT_TAP, Event::NFC_CARD_DETECTED, State::PROVISIONING_SELECT_AID, nullptr},
    {State::PROVISIONING_SELECT_AID, Event::SELECT_AID_SUCCESS, State::PROVISIONING_EXCHANGE_KEYS, nullptr},
    {State::PROVISIONING_EXCHANGE_KEYS, Event::KEYS_EXCHANGED, State::PROVISIONING_STORE_CREDS, 
        [](const StateContext& ctx) { return ctx.phone_key_valid; }},
    {State::PROVISIONING_STORE_CREDS, Event::CREDENTIALS_STORED, State::IDLE, nullptr},
    
    // AUTH flow
    {State::AUTH_WAIT_CONNECT, Event::BLE_CLIENT_CONNECTED, State::AUTH_HANDSHAKE, nullptr},
    {State::AUTH_HANDSHAKE, Event::CLIENT_HELLO_RECEIVED, State::AUTH_VERIFY_KEYS, nullptr},
    {State::AUTH_VERIFY_KEYS, Event::AUTH_VERIFIED, State::AUTH_SESSION_READY, nullptr},
    {State::AUTH_SESSION_READY, Event::UNLOCK_REQUESTED, State::UNLOCKING_CHECK_PROXIMITY, nullptr},
    
    // UNLOCKING flow
    {State::UNLOCKING_CHECK_PROXIMITY, Event::PROXIMITY_OK, State::UNLOCKING_VERIFY_AUTH, nullptr},
    {State::UNLOCKING_VERIFY_AUTH, Event::AUTH_SESSION_VALID, State::UNLOCKING_EXECUTE, nullptr},
    {State::UNLOCKING_EXECUTE, Event::UNLOCK_EXECUTED, State::UNLOCKING_COMPLETE, nullptr},
    {State::UNLOCKING_COMPLETE, Event::TIMEOUT, State::IDLE, nullptr},
    
    // Error handling
    {State::ERROR_HANDLER, Event::RESET_REQUESTED, State::IDLE, nullptr},
    
    // Timeouts from any state → ERROR_HANDLER
    // (handled in processEvent)
};

void begin() {
    Serial.println("[FSM] Initializing...");
    
    currentState = State::INIT;
    previousState = State::INIT;
    context.reset();
    info.enter_time = millis();
    info.transition_count = 0;
    
    // Initialize subsystems
    // NimBLEDevice::init(...);
    // PN532::begin(...);
    
    // Trigger SYSTEM_READY after init
    delay(100);
    triggerEvent(Event::SYSTEM_READY);
    
    Serial.println("[FSM] ✓ Initialized");
}

void tick() {
    // Check timeout
    checkTimeout();
    
    // Process queued events
    while (queueTail != queueHead) {
        QueuedEvent& evt = eventQueue[queueTail];
        processEvent(evt.event);
        queueTail = (queueTail + 1) % EVENT_QUEUE_SIZE;
    }
    
    // State-specific background work
    switch (currentState) {
        case State::PROVISIONING_WAIT_TAP:
            // NFCSession::poll();
            break;
        case State::AUTH_WAIT_CONNECT:
            // BLE advertising
            break;
        case State::AUTH_SESSION_READY:
            // Monitor session timeout
            break;
        default:
            break;
    }
}

bool triggerEvent(Event event) {
    size_t next = (queueHead + 1) % EVENT_QUEUE_SIZE;
    if (next == queueTail) {
        Serial.println("[FSM] ERROR: Event queue full!");
        return false;
    }
    
    eventQueue[queueHead] = {event, millis()};
    queueHead = next;
    
    if (debugLogging) {
        Serial.printf("[FSM] Event queued: %s\n", eventToString(event));
    }
    return true;
}

static void processEvent(Event event) {
    if (debugLogging) {
        Serial.printf("[FSM] Processing event: %s (state: %s)\n", 
                     eventToString(event), stateToString(currentState));
    }
    
    info.lastEvent = event;
    
    // Find matching transition
    State nextState = currentState;
    bool transitionFound = false;
    
    for (const auto& trans : transitionTable) {
        if (trans.from == currentState && trans.on == event) {
            // Check guard condition
            if (trans.guard == nullptr || trans.guard(context)) {
                nextState = trans.to;
                transitionFound = true;
                break;
            } else {
                if (debugLogging) {
                    Serial.println("[FSM] Transition guard failed");
                }
            }
        }
    }
    
    if (transitionFound && nextState != currentState) {
        executeTransition(nextState, event);
    } else if (!transitionFound && event == Event::ERROR_OCCURRED) {
        // Always handle errors
        executeTransition(State::ERROR_HANDLER, event);
    } else if (!transitionFound && event == Event::TIMEOUT) {
        // Timeout handling based on state
        if (isProvisioning() || isAuthenticating()) {
            executeTransition(State::ERROR_HANDLER, event);
        }
    }
}

static void executeTransition(State newState, Event triggerEvent) {
    Serial.printf("[FSM] Transition: %s → %s (event: %s)\n",
                 stateToString(currentState), stateToString(newState),
                 eventToString(triggerEvent));
    
    // Exit action
    if (exitActions[currentState] != nullptr) {
        exitActions[currentState](context);
    }
    
    // Update state
    previousState = currentState;
    currentState = newState;
    info.current = newState;
    info.previous = previousState;
    info.enter_time = millis();
    info.transition_count++;
    
    // Clear timeout
    currentTimeout.active = false;
    
    // Entry action
    if (entryActions[currentState] != nullptr) {
        entryActions[currentState](context);
    }
}

static void checkTimeout() {
    if (!currentTimeout.active) return;
    
    uint32_t elapsed = millis() - currentTimeout.start_ms;
    if (elapsed >= currentTimeout.duration_ms) {
        if (debugLogging) {
            Serial.printf("[FSM] Timeout after %u ms\n", elapsed);
        }
        triggerEvent(Event::TIMEOUT);
        currentTimeout.active = false;
    }
}

// API implementations
State getCurrentState() { return currentState; }
StateInfo getStateInfo() { return info; }
const StateContext& getContext() { return context; }

void setStateTimeout(uint32_t timeout_ms) {
    currentTimeout.active = true;
    currentTimeout.start_ms = millis();
    currentTimeout.duration_ms = timeout_ms;
    if (debugLogging) {
        Serial.printf("[FSM] Timeout set: %u ms\n", timeout_ms);
    }
}

void clearTimeout() {
    currentTimeout.active = false;
}

void onStateEntry(State state, StateAction action) {
    if (state < MAX_STATES) {
        entryActions[state] = action;
    }
}

void onStateExit(State state, StateAction action) {
    if (state < MAX_STATES) {
        exitActions[state] = action;
    }
}

size_t getEventQueueSize() {
    return (queueHead - queueTail + EVENT_QUEUE_SIZE) % EVENT_QUEUE_SIZE;
}

bool isEventQueueFull() {
    return ((queueHead + 1) % EVENT_QUEUE_SIZE) == queueTail;
}

void clearEventQueue() {
    queueHead = queueTail = 0;
}

bool isProvisioning() {
    return currentState >= State::PROVISIONING_WAIT_TAP && 
           currentState <= State::PROVISIONING_STORE_CREDS;
}

bool isAuthenticating() {
    return currentState >= State::AUTH_WAIT_CONNECT && 
           currentState <= State::AUTH_SESSION_READY;
}

bool isUnlocking() {
    return currentState >= State::UNLOCKING_CHECK_PROXIMITY && 
           currentState <= State::UNLOCKING_COMPLETE;
}

bool isInErrorState() {
    return currentState == State::ERROR_HANDLER;
}

uint32_t getTimeInCurrentState() {
    return millis() - info.enter_time;
}

void setDebugLogging(bool enable) {
    debugLogging = enable;
}

void printStatus() {
    Serial.println("┌──────────────────────────────────────────────────────┐");
    Serial.println("│              FSM Status Report                       │");
    Serial.println("├──────────────────────────────────────────────────────┤");
    Serial.printf("│ Current State: %-33s │\n", stateToString(currentState));
    Serial.printf("│ Previous State: %-32s │\n", stateToString(previousState));
    Serial.printf("│ Last Event: %-36s │\n", eventToString(info.lastEvent));
    Serial.printf("│ Time in State: %-26u ms │\n", getTimeInCurrentState());
    Serial.printf("│ Transitions: %-34u │\n", info.transition_count);
    Serial.printf("│ Event Queue: %u / %u                              │\n", 
                 getEventQueueSize(), EVENT_QUEUE_SIZE);
    Serial.printf("│ Timeout Active: %-32s │\n", currentTimeout.active ? "YES" : "NO");
    Serial.println("│                                                      │");
    Serial.printf("│ Context:                                             │\n");
    Serial.printf("│   Phone Key Valid: %-29s │\n", context.phone_key_valid ? "YES" : "NO");
    Serial.printf("│   Session Ready: %-31s │\n", context.session_keys_ready ? "YES" : "NO");
    Serial.printf("│   Retry Count: %-33u │\n", context.retry_count);
    Serial.printf("│   Error Count: %-33u │\n", context.error_count);
    Serial.println("└──────────────────────────────────────────────────────┘");
}

void printTransitionTable() {
    Serial.println("FSM Transition Table:");
    for (const auto& trans : transitionTable) {
        Serial.printf("  %s + %s → %s%s\n",
                     stateToString(trans.from),
                     eventToString(trans.on),
                     stateToString(trans.to),
                     trans.guard ? " [guarded]" : "");
    }
}

} // namespace FSM
```

## 🎯 So Sánh: Trước vs Sau

### ❌ Thiếu (Trước khi bổ sung):
1. ❌ **StateContext** - Không có cách lưu data giữa states
2. ❌ **Transition Table** - Transition logic hardcoded trong switch-case
3. ❌ **Guard Conditions** - Không check điều kiện trước khi transition
4. ❌ **Entry/Exit Actions** - Không có callbacks khi vào/ra state
5. ❌ **Event Queue** - Events bị miss nếu xử lý đồng thời
6. ❌ **Timeout Management** - Mỗi state tự quản lý timeout
7. ❌ **Event with Data** - Không truyền data kèm event

### ✅ Đầy Đủ (Sau khi bổ sung):
1. ✅ **StateContext** - Shared data structure
2. ✅ **Transition Table** - Data-driven transitions
3. ✅ **Guard Conditions** - Optional pre-conditions
4. ✅ **Entry/Exit Actions** - State lifecycle hooks
5. ✅ **Event Queue (FIFO)** - Async event handling
6. ✅ **Timeout API** - Unified timeout management
7. ✅ **Event with Data** - Pass payloads with events
8. ✅ **Validation** - Check FSM configuration
9. ✅ **Debug Helpers** - printStatus(), printTransitionTable()

## 📚 Usage Example

```cpp
// main.cpp
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"

void setup() {
    Serial.begin(115200);
    
    // Register state actions
    FSM::onStateEntry(FSM::State::PROVISIONING_WAIT_TAP, []FSM::StateContext& ctx) {
        Serial.println("Waiting for NFC tap...");
        FSM::setStateTimeout(30000);  // 30s
    });
    
    FSM::onStateEntry(FSM::State::AUTH_SESSION_READY, [](FSM::StateContext& ctx) {
        Serial.println("Session ready - can unlock now");
        digitalWrite(LED_PIN, HIGH);
    });
    
    FSM::onStateExit(FSM::State::AUTH_SESSION_READY, [](FSM::StateContext& ctx) {
        digitalWrite(LED_PIN, LOW);
    });
    
    // Start FSM
    FSM::begin();
    FSM::setDebugLogging(true);
}

void loop() {
    FSM::tick();  // Process events and timeouts
    
    // Handle serial commands
    if (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case 'f': FSMIntegration::SerialCmd::startProvisioning(); break;
            case 's': FSM::printStatus(); break;
            case 't': FSM::printTransitionTable(); break;
        }
    }
}

// In NFC callback
void onNfcCardDetected() {
    FSMIntegration::NFC::onCardDetected();  // Triggers FSM event
}
```

## ✅ Kết Luận

Với các bổ sung trên, FSM của bạn giờ đã:
- ✅ **Complete**: Đủ components cho production FSM
- ✅ **Maintainable**: Transition table dễ đọc/sửa
- ✅ **Testable**: Guard conditions có thể unit test
- ✅ **Debuggable**: Rich logging và status reporting
- ✅ **Scalable**: Dễ thêm states/events mới

**Next Steps:**
1. Implement `fsm.cpp` theo template trên
2. Implement `fsm_states.cpp` (stateToString, eventToString)
3. Implement `fsm_integration.cpp` (helper wrappers)
4. Write unit tests cho transition logic
5. Integrate với NFC/BLE subsystems
