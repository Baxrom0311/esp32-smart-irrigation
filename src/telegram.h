// telegram.h — Telegram bot notification module.
#pragma once

#include "state.h"

void telegramBegin(SystemState& state);
void telegramTick(SystemState& state);

// Queue a message to all configured chat_ids.
void telegramSend(SystemState& state, const char* msg);
// Formatted notification helpers
void telegramNotifyPumpOn(SystemState& state, uint8_t pump);
void telegramNotifyPumpOff(SystemState& state, uint8_t pump);
void telegramNotifyLowWater(SystemState& state, uint8_t tank, uint8_t pct);
void telegramNotifyError(SystemState& state, const char* err);
