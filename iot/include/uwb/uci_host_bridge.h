#pragma once

#include <stddef.h>
#include <stdint.h>

#include "uwb/uci_oob.h"

namespace UwbUciHost {

void init(UwbUci::UciSessionManager* manager);
void tick();

bool submitBleOob(const uint8_t* payload, size_t len, const char** err);
bool requestStart(const char** err);
bool requestStop(const char** err);
bool hasCachedConfig();
bool isBusy();
bool hasPending();

}  // namespace UwbUciHost
