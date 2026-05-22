// server.cpp — HTTP reporter + command receiver.
//
// Every SERVER_DECIDE_INTERVAL_MS:
//   1. POST sensor data + current pump state to server
//   2. Server responds with desired pump state
//   3. relaysTick() reads serverGetPumpDecision() and drives pumps

#include "server.h"
#include "config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static uint32_t g_lastTickMs = 0;
static bool     g_hasDecision = false;
static bool     g_pumpDecision[2] = {false, false};
static WiFiClientSecure g_secureClient;

void serverBegin(SystemState& /*state*/) {
    g_lastTickMs = 0;
    g_hasDecision = false;
    g_secureClient.setInsecure(); // skip cert verification (Cloudflare)
}

void serverTick(SystemState& state) {
    if (WiFi.status() != WL_CONNECTED) { g_hasDecision = false; return; }

    uint32_t now = millis();
    if (g_lastTickMs != 0 && (now - g_lastTickMs) < SERVER_DECIDE_INTERVAL_MS) return;
    g_lastTickMs = now;

    Settings cfg;
    SensorReading sensors;
    bool pump1On, pump2On;
    if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    cfg = state.settings;
    sensors = state.sensors;
    pump1On = state.pumps[0].on;
    pump2On = state.pumps[1].on;
    xSemaphoreGive(state.mutex);

    if (cfg.serverUrl[0] == '\0') { g_hasDecision = false; return; }

    HTTPClient http;
    char url[MAX_SERVER_URL_LEN + 32];
    snprintf(url, sizeof(url), "%s/api/device/report", cfg.serverUrl);

    if (strncmp(cfg.serverUrl, "https", 5) == 0)
        http.begin(g_secureClient, url);
    else
        http.begin(url);
    http.addHeader("Content-Type", "application/json");
    if (cfg.serverApiKey[0]) http.addHeader("Authorization", cfg.serverApiKey);
    http.setTimeout(SERVER_HTTP_TIMEOUT_MS);

    StaticJsonDocument<320> doc;
    doc["soil1"] = sensors.soil1Pct;
    doc["soil2"] = sensors.soil2Pct;
    doc["tank1"] = sensors.tank1Pct;
    doc["tank2"] = sensors.tank2Pct;
    if (!sensors.dhtErr) {
        doc["temp"] = sensors.tempC;
        doc["hum"]  = sensors.humPct;
    }
    doc["tank1_err"] = sensors.tank1Err;
    doc["tank2_err"] = sensors.tank2Err;
    doc["soil1_err"] = sensors.soil1Err;
    doc["soil2_err"] = sensors.soil2Err;
    doc["pump1"] = pump1On;
    doc["pump2"] = pump2On;

    char body[320];
    serializeJson(doc, body, sizeof(body));

    int code = http.POST(body);
    if (code >= 200 && code < 300) {
        String resp = http.getString();
        StaticJsonDocument<512> rd;
        if (deserializeJson(rd, resp) == DeserializationError::Ok) {
            g_pumpDecision[0] = rd["pump1"] | false;
            g_pumpDecision[1] = rd["pump2"] | false;
            g_hasDecision = true;
            Serial.printf("[server] got: pump1=%d pump2=%d\n",
                          g_pumpDecision[0], g_pumpDecision[1]);
        }
    } else {
        g_hasDecision = false;
        Serial.printf("[server] report failed: %d\n", code);
    }
    http.end();
}

bool serverHasDecision() { return g_hasDecision; }
bool serverGetPumpDecision(uint8_t i) { return i < 2 && g_pumpDecision[i]; }

#else
void serverBegin(SystemState&) {}
void serverTick(SystemState&) {}
bool serverHasDecision() { return false; }
bool serverGetPumpDecision(uint8_t) { return false; }
#endif
