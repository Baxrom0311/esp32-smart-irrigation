// main.cpp — ESP32 Smart Irrigation Controller entry point.
//
// Boot order:
//   1. Serial + NVS / settings
//   2. Sensors (ADC + DHT)
//   3. Relays (driven OFF before pinMode)
//   4. Display (TFT)
//   5. WiFi AP + AsyncWebServer
//   6. Task watchdog (30 s)
//
// loop() is single-threaded for sensors/relays/display. Async HTTP
// runs on its own task. Each tick is non-blocking; we feed the
// watchdog every iteration.

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "state.h"
#include "storage.h"
#include "sensors.h"
#include "relays.h"
#include "display.h"
#include "webserver.h"
#include "server.h"

static SystemState g_state{};

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println(F("=== ESP32 Smart Irrigation Controller ==="));
    Serial.print(F("Firmware version: "));
#ifdef APP_VERSION
    Serial.println(APP_VERSION);
#else
    Serial.println(F("dev"));
#endif

    g_state.mutex = xSemaphoreCreateMutex();
    if (g_state.mutex == nullptr) {
        // Without the mutex every cross-task read/write is a race;
        // sensor snapshots, settings persistence and the relay FSM
        // would all silently corrupt each other. Refuse to run.
        // Reboot is fail-safe because relays are driven OFF on every
        // boot before pinMode(OUTPUT) is called.
        Serial.println(F("[fatal] mutex creation failed; rebooting in 2 s"));
        Serial.flush();
        delay(2000);
        ESP.restart();
        // ESP.restart() does not return; the while-loop satisfies
        // the compiler and provides a fallback if the restart call
        // were ever stubbed out.
        while (true) { delay(1000); }
    }
    g_state.lastError[0] = '\0';
    eventLogInit(g_state.events);
    metricsInit(g_state.metrics);
    g_state.metrics.bootMs = millis();
    g_state.apHealthLastOkMs = 0;

    storageBegin();
    if (!storageLoadSettings(g_state.settings)) {
        settingsLoadDefaults(g_state.settings);
        Serial.println(F("[storage] using defaults (load failed)"));
    }
    settingsClamp(g_state.settings);

    sensorsBegin(g_state);
    relaysBegin(g_state);
    displayBegin(g_state);
    webserverBegin(g_state);
    serverBegin(g_state);

    // Task WDT: any hang in loop() longer than WDT_TIMEOUT_S forces a
    // reset. Relays are driven OFF on boot, so a reset is fail-safe.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    Serial.printf("[wdt] task watchdog armed (%d s)\n", WDT_TIMEOUT_S);

    Serial.println(F("[boot] setup complete"));
    pushSystemEvent(g_state, EventLevel::Info, "boot complete");
}

void loop() {
    esp_task_wdt_reset();
    sensorsTick(g_state);
    serverTick(g_state);
    relaysTick(g_state);
    displayTick(g_state);
    webserverTick(g_state);

    // Bump the loop-tick counter so /api/metrics can surface main-
    // task progress. Also refresh the heap watermark on every
    // iteration; the periodic serial log below uses the same value.
    if (g_state.mutex && xSemaphoreTake(g_state.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        metricsIncSat(g_state.metrics.mainLoopTicks, 1);
        uint32_t freeNow = ESP.getFreeHeap();
        if (g_state.metrics.freeHeapMin == 0 ||
            freeNow < g_state.metrics.freeHeapMin) {
            g_state.metrics.freeHeapMin = freeNow;
        }
        xSemaphoreGive(g_state.mutex);
    }

    // Periodic free-heap audit. AsyncTCP + WiFi + the per-request
    // body buffers can leak slowly under unusual traffic patterns;
    // a steadily falling minimum is the earliest signal we can act
    // on before the system reaches an OOM-driven reboot. We track
    // both the current free heap and the all-time minimum so the
    // serial log shows both.
    static uint32_t lastHeapLogMs = 0;
    static uint32_t minHeapSeen   = 0xFFFFFFFFu;
    uint32_t now = millis();
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minHeapSeen) minHeapSeen = freeHeap;
    if (lastHeapLogMs == 0 || (now - lastHeapLogMs) >= HEAP_LOG_PERIOD_MS) {
        lastHeapLogMs = now;
        if (freeHeap < HEAP_LOW_THRESHOLD_BYTES) {
            Serial.printf("[heap] LOW free=%u min=%u (threshold=%u)\n",
                          (unsigned)freeHeap,
                          (unsigned)minHeapSeen,
                          (unsigned)HEAP_LOW_THRESHOLD_BYTES);
        } else {
            Serial.printf("[heap] free=%u min=%u\n",
                          (unsigned)freeHeap,
                          (unsigned)minHeapSeen);
        }
    }

    // Keep the loop responsive but yield to AsyncTCP/WiFi tasks.
    delay(10);
}
