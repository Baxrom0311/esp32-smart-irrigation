// eventlog.cpp — Ring-buffer event log implementation.
//
// The "pure" half (eventLogInit/Add/Get + level text) is testable
// under the native env. The Arduino-only `pushSystemEvent` helper
// briefly locks `SystemState::mutex` and forwards to eventLogAdd;
// it is the convenience entry point for modules that do not
// already hold the mutex (sensors.cpp, the AP watchdog, etc.).

#include "eventlog.h"

#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>

  #include "state.h"
#endif

void eventLogInit(EventLog& log) {
    log.head  = 0;
    log.count = 0;
    log.total = 0;
    // The entry array is left uninitialised on purpose — `count`
    // is the source of truth for valid slots, and zeroing 32 ×
    // 96-byte structs is wasted work on the boot path. Callers
    // never see stale entries because eventLogGet() bounds on
    // `count`.
}

void eventLogAdd(EventLog& log,
                 uint32_t timestampMs,
                 EventLevel level,
                 const char* msg) {
    EventEntry& e = log.entries[log.head];
    e.timestampMs = timestampMs;
    e.sequence    = log.total;  // 0-based; matches "this many came before"
    e.level       = level;

    if (msg == nullptr) {
        e.msg[0] = '\0';
    } else {
        // strncpy is fine here because we always NUL-terminate
        // explicitly afterwards; the trailing-NUL gotcha doesn't
        // apply.
        strncpy(e.msg, msg, EVENTLOG_MSG_LEN - 1);
        e.msg[EVENTLOG_MSG_LEN - 1] = '\0';
    }

    log.head = (uint16_t)((log.head + 1) % EVENTLOG_CAPACITY);
    if (log.count < EVENTLOG_CAPACITY) {
        log.count = (uint16_t)(log.count + 1);
    }
    // Saturating monotonic counter. UINT32_MAX events at one event
    // per second is ~136 years; the saturation branch exists for
    // correctness rather than realism.
    if (log.total < UINT32_MAX) {
        log.total++;
    }
}

const EventEntry* eventLogGet(const EventLog& log, size_t i) {
    if (i >= log.count) return nullptr;
    // Oldest entry index = (head - count) mod CAPACITY. The
    // expression below avoids signed subtraction.
    size_t oldestIdx = (size_t)((log.head + EVENTLOG_CAPACITY - log.count)
                                % EVENTLOG_CAPACITY);
    size_t idx = (oldestIdx + i) % EVENTLOG_CAPACITY;
    return &log.entries[idx];
}

const char* eventLevelText(EventLevel lvl) {
    switch (lvl) {
        case EventLevel::Info:  return "info";
        case EventLevel::Warn:  return "warn";
        case EventLevel::Error: return "error";
    }
    return "unknown";
}

#ifdef ARDUINO
void pushSystemEvent(SystemState& state,
                     EventLevel level,
                     const char* msg) {
    if (msg == nullptr) msg = "";
    // Mirror the serial log so a tail of `pio device monitor`
    // shows the same story as the dashboard. Best-effort write —
    // if Serial is not available the printf is a no-op.
    Serial.printf("[event %s] %s\n", eventLevelText(level), msg);

    if (!state.mutex) return;
    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        // Drop rather than block: an event log entry is not worth
        // stalling the FSM tick or an HTTP handler. The serial line
        // above keeps the breadcrumb either way.
        return;
    }
    eventLogAdd(state.events, millis(), level, msg);
    xSemaphoreGive(state.mutex);
}
#endif
