// eventlog.h — Bounded ring-buffer event log.
//
// Captures a tail of recent system events (boot, lockouts, sensor
// errors, AP restarts) for the dashboard's `Recent events` panel
// and `/api/events`. Older entries are silently overwritten when
// the buffer fills — the log is intentionally bounded so it never
// allocates from the heap during normal operation.
//
// Pure C++: no Arduino dependency in the core type so the native
// test environment can verify overwrite semantics and the
// monotonic sequence counter without pulling in WiFi.h.
//
// Thread model: the struct is intended to be embedded inside
// `SystemState` and accessed under `SystemState::mutex`. The
// helpers themselves do not touch the mutex; the caller decides
// the locking discipline.

#pragma once

#include <stddef.h>
#include <stdint.h>

// 32 entries × 96-byte struct ≈ 3 KB on the heapless side. Plenty
// for an irrigation controller — a multi-day run typically logs a
// handful of events per day.
#define EVENTLOG_CAPACITY 32

// Per-entry message length. Sized to fit the longest realistic
// status string ("Pump 2 dry-run lockout cleared at 28 %") with
// some headroom; truncation is handled transparently.
#define EVENTLOG_MSG_LEN  72

enum class EventLevel : uint8_t {
    Info  = 0,
    Warn  = 1,
    Error = 2,
};

struct EventEntry {
    uint32_t   timestampMs;   // millis() at insert (caller-provided)
    uint32_t   sequence;      // monotonic ID; matches `total - 1` at insert
    EventLevel level;
    char       msg[EVENTLOG_MSG_LEN];
};

struct EventLog {
    EventEntry entries[EVENTLOG_CAPACITY];
    uint16_t   head;          // next write index (0..CAPACITY-1)
    uint16_t   count;         // valid entries (0..CAPACITY)
    uint32_t   total;         // monotonic; saturates at UINT32_MAX
};

// Reset the log to an empty state. Must be called once before use.
void eventLogInit(EventLog& log);

// Append a new event. Overwrites the oldest entry once the buffer
// is full. The caller-supplied `msg` is copied into the entry and
// truncated to EVENTLOG_MSG_LEN-1 chars; a NULL `msg` is treated
// as the empty string. `total` is incremented (saturating) on
// every call regardless of overwrite, so it can be used as a
// monotonic event-count metric.
void eventLogAdd(EventLog& log,
                 uint32_t timestampMs,
                 EventLevel level,
                 const char* msg);

// Indexed read where `0` returns the OLDEST entry currently in
// the log and `count - 1` returns the NEWEST. Returns nullptr if
// `i >= count`. The returned pointer is valid only until the next
// `eventLogAdd()` (or another mutator) on the same log.
const EventEntry* eventLogGet(const EventLog& log, size_t i);

// Stable label for transport (`"info"` / `"warn"` / `"error"`).
// Suitable for JSON payloads.
const char* eventLevelText(EventLevel lvl);

// Forward declaration; the Arduino-only push helper that locks
// SystemState::mutex lives in eventlog.cpp's ARDUINO branch.
struct SystemState;

#ifdef ARDUINO
// Convenience wrapper used by modules that do NOT already hold
// `SystemState::mutex`. Briefly takes the mutex, appends the
// event, releases. Safe to call from any task (main loop or
// AsyncTCP). Best-effort: if the mutex cannot be acquired within
// 20 ms the event is dropped (better than blocking the FSM).
void pushSystemEvent(SystemState& state,
                     EventLevel level,
                     const char* msg);
#endif
