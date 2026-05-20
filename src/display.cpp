// display.cpp — ILI9341 240×320 UI.
//
// Layout (portrait, rotation 0):
//
//   ┌────────────────────────────────┐  y=0
//   │ Smart Irrigation               │  header (height 28)
//   │ AP 192.168.4.1                 │
//   ├────────────────────────────────┤
//   │ Tank 1   75%  ████████░░░░     │  rows of height 32
//   │ Tank 2   60%  ███████░░░░░     │
//   │ Soil     45%  █████░░░░░░░     │
//   │ Temp    28.5°C                 │
//   │ Humid   65%                    │
//   ├────────────────────────────────┤
//   │ Pump1: ON     Pump2: OFF       │
//   │ Mode: AUTO                     │
//   ├────────────────────────────────┤
//   │ ! dry-run lockout pump 1       │  error banner (only if any)
//   └────────────────────────────────┘
//
// Partial-redraw rule: each visible field has a cache slot; if the
// new value matches the cache the row is skipped, eliminating flicker.

#include "display.h"

#include "config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include <TFT_eSPI.h>

  static TFT_eSPI g_tft;

  // Cached previous values for partial redraw.
  struct Cache {
      int16_t tank1 = -1, tank2 = -1, soil = -1;
      float   temp  = -999.0f, hum = -999.0f;
      bool    pump1 = false,  pump2 = false;
      PumpFsm pump1Fsm = PumpFsm::Off, pump2Fsm = PumpFsm::Off;
      bool    pumpInit = false;
      bool    autoMode = false;
      bool    autoInit = false;
      char    ip[24]    = "";
      char    err[64]   = "";
      bool    tank1Err = false, tank2Err = false, soilErr = false, dhtErr = false;
  };
  static Cache g_cache;
  static uint32_t g_lastDrawMs = 0;

  // Colour palette (RGB565).
  static constexpr uint16_t COL_BG       = 0x0000;
  static constexpr uint16_t COL_HEADER   = 0x18C3;  // very dark blue
  static constexpr uint16_t COL_FG       = 0xFFFF;
  static constexpr uint16_t COL_DIM      = 0x8410;
  static constexpr uint16_t COL_OK       = 0x07E0;
  static constexpr uint16_t COL_WARN     = 0xFD20;
  static constexpr uint16_t COL_ERR      = 0xF800;
  static constexpr uint16_t COL_BAR_FG   = 0x07FF;
  static constexpr uint16_t COL_BAR_BG   = 0x2104;

  static constexpr int16_t SCREEN_W = 240;
  static constexpr int16_t SCREEN_H = 320;

  static void drawHeader(const SystemState& s) {
      g_tft.fillRect(0, 0, SCREEN_W, 38, COL_HEADER);
      g_tft.setTextColor(COL_FG, COL_HEADER);
      g_tft.setTextDatum(TL_DATUM);
      g_tft.drawString("Smart Irrigation", 6, 4, 4);

      char ipBuf[24];
      snprintf(ipBuf, sizeof(ipBuf), "AP %s", s.apIp.toString().c_str());
      if (strncmp(g_cache.ip, ipBuf, sizeof(g_cache.ip)) != 0) {
          strncpy(g_cache.ip, ipBuf, sizeof(g_cache.ip) - 1);
          g_cache.ip[sizeof(g_cache.ip) - 1] = '\0';
      }
      g_tft.fillRect(0, 22, SCREEN_W, 14, COL_HEADER);
      g_tft.setTextColor(COL_FG, COL_HEADER);
      g_tft.drawString(ipBuf, 6, 22, 2);
  }

  static void drawBarRow(int16_t y, const char* label, int16_t pct, bool err) {
      // Label.
      g_tft.fillRect(0, y, SCREEN_W, 30, COL_BG);
      g_tft.setTextColor(COL_FG, COL_BG);
      g_tft.setTextDatum(TL_DATUM);
      g_tft.drawString(label, 6, y + 4, 2);

      // Percentage text.
      char buf[8];
      if (err) snprintf(buf, sizeof(buf), "ERR");
      else     snprintf(buf, sizeof(buf), "%3d%%", pct);
      g_tft.setTextColor(err ? COL_ERR : COL_FG, COL_BG);
      g_tft.drawString(buf, 70, y + 4, 2);

      // Bar.
      int16_t barX = 110, barY = y + 6, barW = 120, barH = 16;
      g_tft.drawRect(barX, barY, barW, barH, COL_DIM);
      int16_t fill = err ? 0 : (pct * (barW - 2)) / 100;
      uint16_t fc = err ? COL_ERR : (pct < 30 ? COL_WARN : COL_BAR_FG);
      g_tft.fillRect(barX + 1, barY + 1, fill, barH - 2, fc);
      g_tft.fillRect(barX + 1 + fill, barY + 1,
                     (barW - 2) - fill, barH - 2, COL_BAR_BG);
  }

  static void drawScalarRow(int16_t y, const char* label,
                            float value, const char* unit, bool err) {
      g_tft.fillRect(0, y, SCREEN_W, 24, COL_BG);
      g_tft.setTextColor(COL_FG, COL_BG);
      g_tft.setTextDatum(TL_DATUM);
      g_tft.drawString(label, 6, y + 2, 2);
      char buf[16];
      if (err || isnan(value)) {
          snprintf(buf, sizeof(buf), "ERR");
          g_tft.setTextColor(COL_ERR, COL_BG);
      } else {
          snprintf(buf, sizeof(buf), "%.1f%s", value, unit);
          g_tft.setTextColor(COL_FG, COL_BG);
      }
      g_tft.drawString(buf, 110, y + 2, 4);
  }

  static void drawPumpStatus(const PumpState& p1, const PumpState& p2) {
      g_tft.fillRect(0, 220, SCREEN_W, 30, COL_BG);
      g_tft.setTextDatum(TL_DATUM);

      auto label = [](const PumpState& p, char* out, size_t n,
                      uint8_t idx) {
          switch (p.fsm) {
              case PumpFsm::On:             snprintf(out, n, "P%u ON",   idx); break;
              case PumpFsm::LockoutDryRun:  snprintf(out, n, "P%u DRY",  idx); break;
              case PumpFsm::LockoutRuntime: snprintf(out, n, "P%u TOUT", idx); break;
              case PumpFsm::LockoutSensor:  snprintf(out, n, "P%u SENS", idx); break;
              case PumpFsm::Off:
              default:                      snprintf(out, n, "P%u --",   idx); break;
          }
      };
      auto colour = [](const PumpState& p) -> uint16_t {
          switch (p.fsm) {
              case PumpFsm::On:             return COL_OK;
              case PumpFsm::LockoutDryRun:
              case PumpFsm::LockoutRuntime:
              case PumpFsm::LockoutSensor:  return COL_ERR;
              default:                      return COL_DIM;
          }
      };

      char buf1[12], buf2[12];
      label(p1, buf1, sizeof(buf1), 1);
      label(p2, buf2, sizeof(buf2), 2);
      g_tft.setTextColor(colour(p1), COL_BG);
      g_tft.drawString(buf1, 6, 224, 4);
      g_tft.setTextColor(colour(p2), COL_BG);
      g_tft.drawString(buf2, 130, 224, 4);
  }

  static void drawMode(bool autoMode) {
      g_tft.fillRect(0, 252, SCREEN_W, 22, COL_BG);
      g_tft.setTextDatum(TL_DATUM);
      g_tft.setTextColor(autoMode ? COL_BAR_FG : COL_WARN, COL_BG);
      g_tft.drawString(autoMode ? "Mode: AUTO" : "Mode: MANUAL", 6, 254, 2);
  }

  static void drawErrorBanner(const char* msg) {
      g_tft.fillRect(0, 280, SCREEN_W, 40, COL_BG);
      if (msg == nullptr || msg[0] == '\0') return;
      g_tft.fillRect(0, 280, SCREEN_W, 40, COL_ERR);
      g_tft.setTextColor(COL_FG, COL_ERR);
      g_tft.setTextDatum(TL_DATUM);
      g_tft.drawString("!", 6, 284, 4);
      g_tft.drawString(msg, 24, 290, 2);
  }
#endif

void displayBegin(SystemState& state) {
#ifdef ARDUINO
    g_tft.init();
    g_tft.setRotation(0);  // portrait, 240×320
    g_tft.fillScreen(COL_BG);
    drawHeader(state);
    // Force first redraw of every field.
    g_cache = Cache{};
    g_lastDrawMs = 0;
#else
    (void)state;
#endif
}

void displayTick(SystemState& state) {
#ifdef ARDUINO
    uint32_t now = millis();
    if (g_lastDrawMs != 0 && (now - g_lastDrawMs) < DISPLAY_PERIOD_MS) {
        return;
    }
    g_lastDrawMs = now;

    SensorReading s;
    PumpState p[2];
    Settings cfg;
    char err[sizeof(state.lastError)];
    IPAddress ip;
    if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    s = state.sensors;
    p[0] = state.pumps[0];
    p[1] = state.pumps[1];
    cfg = state.settings;
    strncpy(err, state.lastError, sizeof(err) - 1);
    err[sizeof(err) - 1] = '\0';
    ip = state.apIp;
    xSemaphoreGive(state.mutex);

    // Header IP refresh (cheap; redraw only on change).
    char ipBuf[24];
    snprintf(ipBuf, sizeof(ipBuf), "AP %s", ip.toString().c_str());
    if (strncmp(g_cache.ip, ipBuf, sizeof(g_cache.ip)) != 0) {
        strncpy(g_cache.ip, ipBuf, sizeof(g_cache.ip) - 1);
        g_cache.ip[sizeof(g_cache.ip) - 1] = '\0';
        g_tft.fillRect(0, 22, SCREEN_W, 14, COL_HEADER);
        g_tft.setTextColor(COL_FG, COL_HEADER);
        g_tft.setTextDatum(TL_DATUM);
        g_tft.drawString(ipBuf, 6, 22, 2);
    }

    if ((int16_t)s.tank1Pct != g_cache.tank1 || s.tank1Err != g_cache.tank1Err) {
        drawBarRow(44, "Tank 1", s.tank1Pct, s.tank1Err);
        g_cache.tank1 = s.tank1Pct;
        g_cache.tank1Err = s.tank1Err;
    }
    if ((int16_t)s.tank2Pct != g_cache.tank2 || s.tank2Err != g_cache.tank2Err) {
        drawBarRow(78, "Tank 2", s.tank2Pct, s.tank2Err);
        g_cache.tank2 = s.tank2Pct;
        g_cache.tank2Err = s.tank2Err;
    }
    if ((int16_t)s.soilPct != g_cache.soil || s.soilErr != g_cache.soilErr) {
        drawBarRow(112, "Soil",   s.soilPct,  s.soilErr);
        g_cache.soil = s.soilPct;
        g_cache.soilErr = s.soilErr;
    }
    bool tempChanged = (fabsf(s.tempC - g_cache.temp) > 0.05f) ||
                       (s.dhtErr != g_cache.dhtErr);
    if (tempChanged) {
        drawScalarRow(150, "Temp", s.tempC, "C", s.dhtErr);
        g_cache.temp = s.tempC;
    }
    bool humChanged = (fabsf(s.humPct - g_cache.hum) > 0.05f) ||
                      (s.dhtErr != g_cache.dhtErr);
    if (humChanged) {
        drawScalarRow(184, "Humid", s.humPct, "%", s.dhtErr);
        g_cache.hum = s.humPct;
        g_cache.dhtErr = s.dhtErr;
    }

    if (!g_cache.pumpInit ||
        p[0].on  != g_cache.pump1 || p[1].on  != g_cache.pump2 ||
        p[0].fsm != g_cache.pump1Fsm || p[1].fsm != g_cache.pump2Fsm) {
        drawPumpStatus(p[0], p[1]);
        g_cache.pump1 = p[0].on;
        g_cache.pump2 = p[1].on;
        g_cache.pump1Fsm = p[0].fsm;
        g_cache.pump2Fsm = p[1].fsm;
        g_cache.pumpInit = true;
    }
    if (!g_cache.autoInit || cfg.autoMode != g_cache.autoMode) {
        drawMode(cfg.autoMode);
        g_cache.autoMode = cfg.autoMode;
        g_cache.autoInit = true;
    }
    if (strncmp(err, g_cache.err, sizeof(g_cache.err)) != 0) {
        strncpy(g_cache.err, err, sizeof(g_cache.err) - 1);
        g_cache.err[sizeof(g_cache.err) - 1] = '\0';
        drawErrorBanner(g_cache.err);
    }
#else
    (void)state;
#endif
}
