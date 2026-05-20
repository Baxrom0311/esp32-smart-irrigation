// telegram.cpp — Telegram bot via HTTPClient (non-blocking queue).
#include "telegram.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

// Simple ring-buffer message queue
static char g_queue[TG_QUEUE_SIZE][160];
static uint8_t g_qHead = 0, g_qTail = 0, g_qCount = 0;
static uint32_t g_lastSendMs = 0;

static void enqueue(const char* msg) {
    if (g_qCount >= TG_QUEUE_SIZE) return; // drop oldest? no, drop newest
    strncpy(g_queue[g_qTail], msg, 159);
    g_queue[g_qTail][159] = '\0';
    g_qTail = (g_qTail + 1) % TG_QUEUE_SIZE;
    g_qCount++;
}

static bool sendToChat(const char* token, const char* chatId, const char* text) {
    if (!token[0] || !chatId[0]) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    char url[180];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    char body[300];
    // Escape basic chars in text for JSON
    snprintf(body, sizeof(body),
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
             chatId, text);

    int code = http.POST(body);
    http.end();
    return (code >= 200 && code < 300);
}

void telegramBegin(SystemState& /*state*/) {
    // Nothing to init — WiFi STA handles connection
}

void telegramTick(SystemState& state) {
    if (!state.settings.tgEnabled) return;
    if (!state.settings.tgToken[0]) return;
    if (state.settings.tgChatCount == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_qCount == 0) return;

    uint32_t now = millis();
    if ((now - g_lastSendMs) < TG_SEND_INTERVAL_MS) return;

    // Send one message per tick (non-blocking pattern)
    const char* msg = g_queue[g_qHead];
    for (uint8_t i = 0; i < state.settings.tgChatCount; i++) {
        sendToChat(state.settings.tgToken, state.settings.tgChatIds[i], msg);
    }
    g_qHead = (g_qHead + 1) % TG_QUEUE_SIZE;
    g_qCount--;
    g_lastSendMs = now;
}

void telegramSend(SystemState& state, const char* msg) {
    if (!state.settings.tgEnabled) return;
    enqueue(msg);
}

void telegramNotifyPumpOn(SystemState& state, uint8_t pump) {
    char buf[80];
    snprintf(buf, sizeof(buf), "💧 Nasos %d <b>YOQILDI</b>", pump + 1);
    telegramSend(state, buf);
}

void telegramNotifyPumpOff(SystemState& state, uint8_t pump) {
    char buf[80];
    snprintf(buf, sizeof(buf), "⏹ Nasos %d <b>O'CHIRILDI</b>", pump + 1);
    telegramSend(state, buf);
}

void telegramNotifyLowWater(SystemState& state, uint8_t tank, uint8_t pct) {
    char buf[80];
    snprintf(buf, sizeof(buf), "⚠️ Tank %d suv kam: <b>%d%%</b>", tank + 1, pct);
    telegramSend(state, buf);
}

void telegramNotifyError(SystemState& state, const char* err) {
    char buf[120];
    snprintf(buf, sizeof(buf), "❌ Xato: %s", err);
    telegramSend(state, buf);
}

#else
// Native build stubs
void telegramBegin(SystemState&) {}
void telegramTick(SystemState&) {}
void telegramSend(SystemState&, const char*) {}
void telegramNotifyPumpOn(SystemState&, uint8_t) {}
void telegramNotifyPumpOff(SystemState&, uint8_t) {}
void telegramNotifyLowWater(SystemState&, uint8_t, uint8_t) {}
void telegramNotifyError(SystemState&, const char*) {}
#endif
