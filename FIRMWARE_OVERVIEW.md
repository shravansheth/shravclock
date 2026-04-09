# shravclock Firmware Overview

A comprehensive reference for the desk clock firmware running on an ESP32-C6 with a 4.2" e-ink display.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | Seeed XIAO ESP32C6 |
| Display | WeAct 4.2" SPI e-ink, SSD1683 driver, B&W, 400×300 px |
| GxEPD2 driver | `GxEPD2_420_GDEY042T81` |
| RTC | DS3231 Mini (I2C) |
| Button | Momentary pushbutton, active-LOW with internal pull-up |
| Battery | 5000 mAh 3.7V Li-ion |

### Pin Assignments

| Signal | GPIO |
|--------|------|
| SPI MOSI | 18 |
| SPI SCK | 19 |
| E-ink CS | 1 |
| E-ink DC | 2 |
| E-ink RST | 21 |
| E-ink BUSY | 16 |
| I2C SDA (RTC) | 22 |
| I2C SCL (RTC) | 23 |
| Button | 17 |

---

## Software Stack

| Item | Details |
|------|---------|
| Build system | PlatformIO, Arduino framework |
| Display library | `zinggjm/GxEPD2` |
| RTC library | `adafruit/RTClib` |
| JSON library | `bblanchon/ArduinoJson` v7 (`JsonDocument`) |
| Partitions | `min_spiffs.csv` (maximizes app flash) |
| Config storage | ESP32 NVS via `Preferences` (namespace: `shravclock`) |
| Weather API | Open-Meteo HTTP (no API key) |
| Geocoding | Nominatim / OpenStreetMap HTTPS |

---

## Features

### Time Display
- 24-hour format `HH:MM`, no seconds shown
- Time is sourced from the **DS3231 RTC** (UTC stored internally, converted to local time via POSIX TZ string at display time)
- On cold boot the RTC is synced from NTP (pool.ntp.org / time.nist.gov)
- Partial display refresh every minute, affecting only the time region (fast, no full flicker)
- Full display refresh every hour (on the :00 boundary)

### Date Display
- Two-line format:
  - Line 1: full day name e.g. `Wednesday` (bold sans-serif)
  - Line 2: month + day + year e.g. `Mar 25, 2026` (regular sans-serif)
- Both lines are centered dynamically at draw time using `getTextBounds()`

### Weather
- Fetched from **Open-Meteo** (`api.open-meteo.com`) via HTTP (plain, no TLS — the ESP32-C6's mbedTLS stack has intermittent handshake failures with open-meteo's TLS config, and the data is public/unauthenticated so encryption adds no real benefit)
- Data pulled: current temperature (°F), WMO weather code, is_day flag, daily low, daily high
- Updated **once per hour** at the `:59` minute mark (pre-fetched before the :00 display refresh)
- Up to **3 retry attempts** with 2-second delays on failure
- If fetch fails, previously cached weather data is shown (persists across deep sleep via `RTC_DATA_ATTR`)
- Weather icon chosen from WMO code → Meteocons character map (see below)

### Weather Icons (Meteocons)
Custom renderer for the **ThingPulse/squix column-major** font format (`Meteocons_Plain_36`, 37×38 px glyphs stored in PROGMEM).

WMO code → character mapping:

| WMO Codes | Condition | Char (day/night) |
|-----------|-----------|-----------------|
| 0 | Clear sky | `B` / `C` |
| 1–2 | Partly cloudy | `H` / `I` |
| 3 | Overcast | `N` |
| 45, 48 | Fog | `M` |
| 51–67 | Drizzle / rain | `Q` |
| 71–77, 85–86 | Snow | `W` |
| 80–82 | Rain showers | `R` |
| 95, 96, 99 | Thunderstorm | `P` |
| default | Cloudy fallback | `N` |

### City Name
- Reverse-geocoded once on cold boot from **Nominatim** using the configured lat/lon
- Tries `city` → `town` → `village` → `display_name` fields in order
- Shown below the Lo/Hi temperature line
- Cached in `RTC_DATA_ATTR` across deep sleep

### Dark Mode
- Inverts foreground/background: white text on black instead of black on white
- Toggled in the web config portal
- Stored in NVS and survives reboots/deep sleep
- When dark mode is active a **crescent moon icon** (two overlapping filled circles) is drawn in the top-left corner of the display

### Configuration (AP / Web Portal)
Triggered by **holding the button for ≥ 1 second** during the active window.

**What happens:**
1. The e-ink display shows a setup screen with SSID `shravclock-setup`, password `shravann`, and the IP `192.168.4.1`
2. The device starts in `WIFI_AP_STA` mode — AP for the config portal, STA to simultaneously connect to the saved home network for city search proxying
3. A **DNS server** on port 53 redirects all domains to `192.168.4.1` (captive portal behavior — iOS, Android, and Windows will pop the portal automatically)
4. A **WebServer** on port 80 serves the configuration UI

**Portal endpoints:**

| Endpoint | Purpose |
|----------|---------|
| `GET /` | Main config HTML page |
| `GET /wifi-status` | JSON: whether STA is connected and to what SSID |
| `GET /search-city?q=` | Proxy to Nominatim search (requires STA connection) |
| `POST /save` | Saves config to NVS, restarts device |
| Various captive portal detection URLs | Redirect to `/` |

**Config fields saved to NVS:**

| Key | Default | Description |
|-----|---------|-------------|
| `ssid` | `secrets.h` value | Home WiFi SSID |
| `pass` | `secrets.h` value | Home WiFi password |
| `tz_posix` | `PST8PDT,M3.2.0/2,M11.1.0/2` | POSIX timezone string |
| `tz_name` | `America/Los_Angeles` | IANA timezone name (auto-resolved from POSIX) |
| `lat` | `secrets.h` value | Latitude for weather |
| `lon` | `secrets.h` value | Longitude for weather |
| `dark_mode` | `false` | Dark mode toggle |

The portal times out after **5 minutes** if no config is saved, then resumes normal deep-sleep operation.

---

## Screen Layout (400×300 display)

```
┌─────────────────────────────────────────┐
│ [moon]                                  │  ← crescent if dark mode
│                 HH:MM                   │  Y baseline ≈ 70 (FreeSansBold18pt ×2)
│                                         │
│                Wednesday                |  Y ≈ 117  (FreeSansBold18pt)
│              Mar 25, 2026               │  Y ≈ 157  (FreeSans18pt)
│                                         │
│             [icon]  72°F                │  Y ≈ 223  (FreeSansBold24pt)
│            Lo 58° | Hi 79°              │  Y below icon bottom
│                 City                    │  Y 6px below Lo|Hi
└─────────────────────────────────────────┘
```

### Font Usage

| Element | Font | Size/Scale |
|---------|------|------------|
| Time | FreeSansBold18pt7b | ×2 (renders as ~36pt) |
| Day name | FreeSansBold18pt7b | ×1 |
| Date | FreeSans18pt7b | ×1 |
| Temperature | FreeSansBold24pt7b | ×1 |
| Lo/Hi + city | FreeSans12pt7b | ×1 |
| Setup screen title | FreeSansBold18pt7b | ×1 |
| Setup screen info | FreeSans12pt7b | ×1 |

The degree symbol is hand-drawn as `drawCircle()` rather than a font glyph.

---

## Deep Sleep Architecture

The firmware is entirely **setup()-driven** — `loop()` just calls `goToSleep()` and is never meaningfully executed. Each wake is a fresh `setup()` call.

### Wake Schedule

The device wakes at **:58 seconds** every minute:

```
:58  → wake (timer)
:58–:59 → button check, weather fetch if hourly boundary
:00  → init display, draw (partial or full)
       if weather fetch ran past :05, draw immediately instead
:00–:05 → active window (button still monitored)
:05+  → go back to sleep until :58
```

Sleep duration formula: `if (sec < 58): sleep = 58 - sec`, else `sleep = (60 - sec) + 58`

### Display Init Timing (important)
The display is **not initialized at :58 wake** — only at the :00 draw point. This is intentional: initializing GxEPD2 sends a reset pulse on GPIO21 (RST), which would wake the SSD1683 from hibernate and cause a **visible flash artifact** at :58 on every cycle. By deferring `display.init()` to just before drawing, the :58 wakeup is visually silent.

### GPIO Hold During Sleep
Before entering deep sleep, `goToSleep()` calls `gpio_hold_en()` on RST, CS, and DC. Without this, the ESP32-C6 boot ROM resets all GPIO states before Arduino code runs on the next wake, causing RST to briefly glitch LOW — which wakes the e-ink controller from hibernate and produces a flash.

At the start of every `setup()` call, `gpio_hold_dis()` is called on those same three pins before `SPI.begin()`. The holds have served their purpose (protecting the boot ROM phase) by the time application code runs; releasing them here allows the SPI peripheral and display driver to control the pins normally. This is a no-op on cold boot since a chip reset clears all holds automatically.

### Refresh Policy

| Condition | Display Action |
|-----------|---------------|
| Cold boot | Full refresh |
| Top of hour (:59 wake → :00 draw) | Full refresh |
| Every other minute | Partial refresh (time region only) |

The full-refresh interval is controlled by `FULL_REFRESH_EVERY_N_MIN = 60`. The partial refresh window (`timeRect`) is pre-computed in `computeLayout()` with 8-pixel alignment (required by SSD1683 partial window constraints) and 10px padding around the time bounding box.

### RTC_DATA_ATTR Persistence
The following survive deep sleep in the ESP32 RTC memory (not the DS3231):

- `_rtcValid` — whether a valid state exists from a previous wake
- `_hasWeather`, `_weatherCode`, `_weatherIsDay`, `_weatherTempF`, `_weatherLowF`, `_weatherHighF`
- `_lastWxFetch` — Unix timestamp of last successful weather fetch
- `_darkMode`
- `_cityName[32]`

These are restored into working globals at the top of every timer wake.

---

## Boot / Wake Flow Summary

```
Power on / reset
    │
    ├─ Cold boot (first boot or RTC state invalid)
    │       loadConfig → NVS
    │       gpio_hold_dis (RST, CS, DC)
    │       init display, init RTC
    │       WiFi connect
    │         ├── settimeofday(0) → NTP sync → set DS3231 (UTC)
    │         ├── fetchCityName (Nominatim)
    │         └── fetchWeatherNow (Open-Meteo, up to 3 attempts)
    │       WiFi off
    │       Full display refresh
    │       → goToSleep()
    │
    ├─ Timer wake at :58
    │       loadConfig, gpio_hold_dis (RST, CS, DC)
    │       restore RTC_DATA_ATTR globals
    │       Check button (held ≥ 1s → AP portal)
    │       If min==59 and ≥2 min since last fetch:
    │         WiFi → fetchWeather → WiFi off
    │         doFullRefresh = true
    │       If min==59: doFullRefresh = true (hourly)
    │       Persist state to RTC_DATA_ATTR
    │       Loop:
    │         - Poll button (held ≥ 1s → AP portal)
    │         - At curSec < 55: init display, draw (full or partial)
    │           (waits for :00 normally; draws immediately if fetch ran past :05)
    │       Exit loop once drawn and curSec > 5 and < 55
    │       → goToSleep()
    │
    └─ Button wake (GPIO — not actually supported on C6 in deep sleep)
            Treated as cold boot path
```

> **Note:** GPIO17 is not an LP (low-power) GPIO on the ESP32-C6, so true deep-sleep GPIO wakeup is not possible. Button wakeup only works during the `:00`–`:05` active window while the CPU is running.

---

## WiFi Credential Fallback

`secrets.h` (not tracked in git) provides compile-time defaults for WiFi credentials and the fallback location:
```cpp
#define WIFI_SSID   "your_ssid"
#define WIFI_PASS   "your_password"
#define DEFAULT_LAT  00.00f 
#define DEFAULT_LON  00.00f
```
These are used on first boot before any NVS config exists. Once the portal saves new values, NVS takes precedence.

---

## NTP Sync Accuracy

Before calling `configTime()`, the firmware explicitly resets the ESP32 system clock to zero via `settimeofday()`. Without this, the ESP32's RTC slow memory (which persists across resets) can hold a stale time from a previous session. The polling loop would detect `time(nullptr) > 1700000000` immediately and exit before a real NTP packet has been received, setting the DS3231 to a potentially seconds-off value. Zeroing first forces the loop to wait for a genuine fresh sync.

---

## Build

```bash
~/.platformio/penv/bin/pio run
# Flash:
~/.platformio/penv/bin/pio run --target upload
```
