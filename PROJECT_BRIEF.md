# ESP32 Smart Irrigation Controller

## Goal

ESP32 asosida aqlli sug'orish tizimi — 2 ta suv idishining sathini monitoring qilish, tuproq namligi asosida avtomatik sug'orish, harorat/namlik o'lchash. Barcha ma'lumotlar TFT ekranda va ESP32 AP mode WiFi orqali web-interfeys da ko'rsatiladi. Tizim to'liq avtonom — internet kerak emas.

## Hardware

| Komponent | Soni | Pin | Izoh |
|-----------|------|-----|------|
| ESP32 DevKit | 1 | — | Asosiy kontroller |
| Water Level Sensor | 2 | GPIO34, GPIO35 | Analog, 2 ta idish uchun |
| Relay Module | 2 | GPIO26, GPIO27 | Nasos 1 va Nasos 2 |
| Soil Moisture Sensor | 1 | GPIO32 | Analog, tuproq namligi |
| DHT22 (AM2302) | 1 | GPIO4 | Harorat + namlik |
| TFT ILI9341 2.8" SPI | 1 | See below | 240×320, SPI, v1.2 |

### TFT Pin mapping (ILI9341 SPI)

| TFT Pin | ESP32 Pin |
|---------|-----------|
| MOSI | GPIO23 |
| MISO | GPIO19 |
| SCK | GPIO18 |
| CS | GPIO5 |
| DC | GPIO2 |
| RST | GPIO15 |
| LED | 3.3V |

## Core Features

### 1. Sensor monitoring (real-time)
- 2 ta suv sathi sensori — foiz (0-100%) ko'rinishida
- 1 ta tuproq namligi — foiz (0-100%)
- DHT22 — harorat (°C) va namlik (%)
- O'qish intervali: 2 soniya

### 2. Nasos boshqaruvi
- 2 ta relay orqali 2 ta nasosni yoqish/o'chirish
- Manual mode: web-interfeys dan yoqish/o'chirish
- Auto mode: tuproq namligi threshold ga asosan avtomatik
- Safety: suv sathi past bo'lsa nasos yoqilmasin (dry-run himoya)
- Nasos ishlash vaqti limiti (max 30 daqiqa uzluksiz)

### 3. TFT Display (ILI9341 240×320)
- Asosiy ekran: barcha sensor qiymatlari real-time
- Nasos holati (ON/OFF) indikatori
- WiFi AP IP manzili
- Auto/Manual mode ko'rsatkich
- Xato holatlari (sensor uzilishi, suv kam)
- UI: professional ko'rinish, ranglar bilan status

### 4. WiFi Access Point + Web Server
- ESP32 o'zi WiFi AP tarqatadi (internet kerak emas)
- SSID: "SmartIrrigation" (sozlanishi mumkin)
- Password: "12345678" (sozlanishi mumkin)
- Web UI: http://192.168.4.1
- Responsive design (telefon va kompyuter uchun)

### 5. Web Interface sahifalari
- **Dashboard** (`/`): Barcha sensor qiymatlari real-time (AJAX/fetch, 2s interval)
- **Control** (`/control`): Nasoslarni manual yoqish/o'chirish tugmalari
- **Settings** (`/settings`): Threshold, timer, WiFi nomi sozlamalari
- **API** (`/api/status`): JSON format da barcha ma'lumotlar

### 6. Sozlamalar (EEPROM/Preferences da saqlanadi)
- Tuproq namligi threshold (auto mode uchun): default 40%
- Suv sathi minimum (nasos himoyasi): default 20%
- Nasos max ishlash vaqti: default 30 daqiqa
- WiFi SSID va password
- Auto/Manual mode tanlash

## Tech Stack

- **Framework**: PlatformIO + Arduino framework
- **Board**: esp32dev (ESP32 DevKit V1)
- **Display**: TFT_eSPI library (ILI9341)
- **DHT**: DHT sensor library (Adafruit)
- **Web**: ESPAsyncWebServer + AsyncTCP
- **Storage**: Preferences (NVS)
- **JSON**: ArduinoJson

## Project Structure

```
esp32-smart-irrigation/
├── platformio.ini
├── src/
│   ├── main.cpp              # Setup + loop
│   ├── config.h              # Pin definitions, constants
│   ├── sensors.h / .cpp      # Sensor o'qish (water level, soil, DHT)
│   ├── relays.h / .cpp       # Relay boshqaruvi + safety logic
│   ├── display.h / .cpp      # TFT UI rendering
│   ├── webserver.h / .cpp    # WiFi AP + AsyncWebServer
│   └── storage.h / .cpp      # Preferences (settings save/load)
├── data/                     # SPIFFS web files (agar kerak bo'lsa)
├── PROJECT_BRIEF.md
└── agentloop.toml
```

## Acceptance Criteria

- [ ] PlatformIO loyiha `pio run` bilan xatosiz compile bo'ladi
- [ ] Barcha sensorlar to'g'ri o'qiladi (analog calibration bilan)
- [ ] TFT ekranda barcha ma'lumotlar professional UI da ko'rinadi
- [ ] WiFi AP ishlaydi, web-interfeys ochiladi
- [ ] Web dashboard real-time sensor qiymatlarini ko'rsatadi
- [ ] Nasoslarni web dan yoqib/o'chirib bo'ladi
- [ ] Auto mode ishlaydi (threshold asosida)
- [ ] Dry-run himoya ishlaydi (suv kam = nasos yoqilmaydi)
- [ ] Sozlamalar saqlanadi (reboot dan keyin ham)
- [ ] Xato holatlari to'g'ri handle qilinadi (sensor uzilishi)
- [ ] Kod modular, o'qilishi oson, commentlar bor

## Non-Goals

- Internet/cloud ulanish (faqat lokal AP)
- OTA update (keyingi versiya uchun)
- Bluetooth
- SD card logging
- Murakkab grafik/chart (oddiy raqamlar yetarli)

## Safety Rules

- Nasos HECH QACHON suv sathi 20% dan past bo'lganda yoqilmasin
- Nasos uzluksiz 30 daqiqadan ko'p ishlamasin
- Sensor o'qilmasa (NaN/error) — nasos o'chirilsin
- Relay default holati: OFF (active LOW)
- Watchdog timer: 30 soniya (hang bo'lsa restart)

## Sensor Calibration

```
Water Level: 0V = bo'sh (0%), 3.3V = to'la (100%)
Soil Moisture: 3.3V = quruq (0%), 0V = ho'l (100%) — teskari!
DHT22: -40°C to 80°C, 0-100% RH
```

## Web API Endpoints

```
GET  /api/status     → {"tank1": 75, "tank2": 60, "soil": 45, "temp": 28.5, "hum": 65, "pump1": false, "pump2": false, "mode": "auto"}
POST /api/pump       → {"pump": 1, "state": true}
POST /api/settings   → {"threshold": 40, "min_water": 20, "max_time": 30, "ssid": "...", "pass": "..."}
GET  /api/settings   → current settings JSON
```

## Current State (2026-05-21)

The firmware compiles and runs on ESP32. The following is DONE:
- Pin config: Water MH sensors GPIO34/35, Capacitive Soil v1.2 GPIO32/33, DHT11 GPIO14, Relays GPIO26/27
- 2 soil sensors (one per agarot/pump)
- TFT display removed (no screen)
- Telegram bot with ArduinoJson-safe serialization
- WiFi AP+STA dual mode with reconnect
- Web UI: Dashboard, Control, Settings (with STA + Telegram), Calibration (with soil2)
- NVS schema v3 with all new fields
- Rate limiting, event log, metrics, AP health watchdog

## Remaining Tasks for Orchestrator

Priority order — fix bugs first, then improve:

1. **BUG: "empty body" error on Settings save** — The browser sometimes sends a double POST (one with body, one empty). The form was changed to `type="button"` + `onclick` but verify it works. If the error persists, investigate ESPAsyncWebServer body handling. The server-side `[storage] settings saved` log confirms the save works, but the browser shows "Error: empty body" — this means TWO requests are being sent.

2. **BUG: WiFi STA AUTH_FAIL** — When STA credentials are wrong, the device retries every 30s forever. Add a retry limit (e.g., 5 attempts) then stop until next reboot or settings change. Log the failure count.

3. **Web UI improvements:**
   - Control page: show soil1/soil2 values per pump (not just pump on/off)
   - Dashboard: show WiFi STA connection status (connected/disconnected/connecting)
   - Settings: show current STA connection status

4. **Telegram notifications integration:**
   - Send notification on pump lockout (dry-run, runtime, sensor)
   - Send notification on WiFi STA connect/disconnect
   - Send periodic status summary (every 6 hours)

5. **Code quality:**
   - Ensure all JSON endpoints use DynamicJsonDocument (not Static) to avoid stack overflow on AsyncTCP task
   - Add serial debug logging for POST /api/settings (log what was received)
   - Verify native tests pass (need gcc installed or skip)

6. **Safety:**
   - Each agarot should be independently controllable (already done: pump1→soil1, pump2→soil2)
   - Verify dry-run protection works with 2 tanks (tank1→pump1, tank2→pump2)

## Hardware Pin Map (FINAL)

| Function | GPIO | Notes |
|----------|------|-------|
| Water tank 1 (MH sensor) | 34 | ADC1 ch6 |
| Water tank 2 (MH sensor) | 35 | ADC1 ch7 |
| Soil moisture 1 (Agarot 1) | 32 | ADC1 ch4, Capacitive v1.2 |
| Soil moisture 2 (Agarot 2) | 33 | ADC1 ch5, Capacitive v1.2 |
| DHT11 | 14 | with 4.7kΩ pull-up |
| Pump 1 relay (Agarot 1) | 26 | active LOW |
| Pump 2 relay (Agarot 2) | 27 | active LOW |
