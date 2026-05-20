// webserver.h — WiFi AP + AsyncWebServer public interface.

#pragma once

#include "state.h"

void webserverBegin(SystemState& state);
void webserverTick(SystemState& state);

// Schedule a soft-AP restart after settings change (deferred so the
// HTTP response is sent before we tear WiFi down).
void webserverScheduleApRestart(SystemState& state);
