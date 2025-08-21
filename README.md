# FUNKTHERMO-TIMER 1.0

## Overview
This Arduino sketch is designed for the **ESP8266** (e.g., NodeMCU, Wemos D1 mini) as a **wireless temperature & humidity receiver** with display and web configuration.  

It combines:
- **Wireless data reception** (via `/update` endpoint)
- **TM1637 4-digit 7-segment display** (temperature & humidity alternating)
- **Web interface** for settings
- **EEPROM persistence** for configuration
- **Acoustic alarm (buzzer)** on threshold violations
- **mDNS support** → access via `http://envrx.local/`

---

## Features
- **Endpoints:**
  - `GET /` → Status page + link to settings
  - `GET /api/last` → JSON response:  
    ```json
    { "t": 23.5, "h": 40.2, "age_ms": 1234, "from": "192.168.1.50" }
    ```
  - `GET /update?token=XYZ&t=25.4&h=60.1` → accepts new measurement values (with token authentication)
  - `GET /settings` → HTML configuration form
  - `POST /save` → saves settings to EEPROM

- **Display:**
  - Shows **temperature & humidity** in alternation
  - Format: `"25:50"` = 25.5 °C
  - Configurable: always on **or** periodically (X seconds every N minutes)

- **Alarm:**
  - Buzzer sounds if thresholds are exceeded
  - Configurable **cooldown** & **hysteresis**
  - Alarm can be enabled/disabled via settings

- **Robust design:**
  - Handles NaN values gracefully (displays placeholder)
  - Clean re-initialization after saving or rebooting

---

## Hardware Requirements
- **ESP8266** (NodeMCU / Wemos D1 mini recommended)
- **TM1637 4-digit display** (CLK + DIO pins configurable in code)
- **Buzzer** for alarm output
- Stable **Wi-Fi network**

---

## Software Dependencies
Install via Arduino IDE Library Manager:
- `ESP8266WiFi`
- `ESP8266WebServer`
- `ESP8266mDNS`
- `TM1637Display`
- `EEPROM`

---

## Configuration
In the sketch, set your Wi-Fi credentials:
```cpp
const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourPassword";
```

Optionally configure:
- `TOKEN` for `/update` endpoint
- Display mode (always on / periodic)
- Alarm thresholds, cooldown, hysteresis

---

## Usage
1. Flash the sketch to your ESP8266.
2. Connect the TM1637 display and buzzer as defined in the code.
3. On boot:
   - Device connects to Wi-Fi via DHCP
   - mDNS available: `http://envrx.local/`
   - Status and IP are shown in the web interface
4. Access **`/settings`** to configure thresholds, display timing, and other options.
5. Use **`/update`** endpoint from a sender device (e.g., weather station) to push new values.

---

## Troubleshooting
- **No Wi-Fi connection** → Check SSID & password, ensure DHCP is enabled.
- **mDNS not working** → Access via IP instead of `envrx.local`.
- **Display off** → Check TM1637 wiring (CLK/DIO), verify display mode settings.
- **Buzzer always on** → Adjust alarm thresholds or hysteresis.
- **Settings not saved** → Ensure `EEPROM.commit()` is present (included in code).

---

## License / Credits
- Built for ESP8266 + TM1637 modules
- Uses Arduino core libraries and TM1637Display
- License: Add your preferred license (MIT recommended)
