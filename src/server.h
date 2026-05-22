// server.h — HTTP reporter + command receiver.
//
// Online: POSTs sensor data to server, receives pump commands.
// Offline: does nothing (caller uses local threshold rules).

#pragma once

#include "state.h"

void serverBegin(SystemState& state);
void serverTick(SystemState& state);

// True if server recently responded with valid pump commands.
bool serverHasDecision();
bool serverGetPumpDecision(uint8_t pumpIdx);
