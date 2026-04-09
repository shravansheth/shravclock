#define ENABLE_GxEPD2_GFX 0

#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>  // time (2x scale) + day name + setup screen title
#include <Fonts/FreeSerifBold18pt7b.h> // unused — reserved
#include <Fonts/FreeSansBold24pt7b.h>  // current temperature
#include <Fonts/FreeSans18pt7b.h>      // date line (month day, year)
#include <Fonts/FreeSerif18pt7b.h>     // unused — reserved
#include <Fonts/FreeSans12pt7b.h>      // Lo/Hi + city name + setup screen info

#include "WeatherStationFonts.h"

// ====== E-ink SPI pins ======
#define MOSI_PIN 18
#define SCK_PIN  19
#define CS_PIN    1
#define DC_PIN    2
#define RES_PIN  21
#define BUSY_PIN 16
#define BUTTON_PIN 17

// ====== RTC I2C pins ======
#define SDA_PIN 22
#define SCL_PIN 23

// ====== City name ======
#define CITY_NAME_MAX 32

// ====== WiFi credentials (fallback defaults from secrets.h) ======
#include "secrets.h"

// Pacific Time with DST (fallback default)
#define TZ_PST "PST8PDT,M3.2.0/2,M11.1.0/2"

static const char *NTP1 = "pool.ntp.org";
static const char *NTP2 = "time.nist.gov";

// Refresh policy
static const uint32_t FULL_REFRESH_EVERY_N_MIN = 60;

// 4.2" BW, SSD1683
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
    display(GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

RTC_DS3231 rtc;

// ====== RTC_DATA_ATTR — persists across deep sleep ======
RTC_DATA_ATTR static bool   _rtcValid      = false;
RTC_DATA_ATTR static bool   _hasWeather    = false;
RTC_DATA_ATTR static int    _weatherCode   = 0;
RTC_DATA_ATTR static bool   _weatherIsDay  = true;
RTC_DATA_ATTR static float  _weatherTempF  = 0.0f;
RTC_DATA_ATTR static float  _weatherLowF   = 0.0f;
RTC_DATA_ATTR static float  _weatherHighF  = 0.0f;
RTC_DATA_ATTR static time_t _lastWxFetch   = 0;
RTC_DATA_ATTR static bool   _darkMode      = false;
RTC_DATA_ATTR static char   _cityName[CITY_NAME_MAX] = {};

// ====== Config system (NVS) ======
struct Config {
    char ssid[64];
    char pass[64];
    char tz_posix[64];   // e.g. "PST8PDT,M3.2.0/2,M11.1.0/2"
    char tz_name[48];    // e.g. "America/Los_Angeles"
    float lat;
    float lon;
    bool darkMode;
};

static Config cfg;

static void loadConfig() {
    Preferences prefs;
    prefs.begin("shravclock", true);  // read-only
    // WiFi - fall back to secrets.h on first boot
    strncpy(cfg.ssid, prefs.getString("ssid", WIFI_SSID).c_str(), sizeof(cfg.ssid)-1);
    cfg.ssid[sizeof(cfg.ssid)-1] = '\0';
    strncpy(cfg.pass, prefs.getString("pass", WIFI_PASS).c_str(), sizeof(cfg.pass)-1);
    cfg.pass[sizeof(cfg.pass)-1] = '\0';
    strncpy(cfg.tz_posix, prefs.getString("tz_posix", TZ_PST).c_str(), sizeof(cfg.tz_posix)-1);
    cfg.tz_posix[sizeof(cfg.tz_posix)-1] = '\0';
    strncpy(cfg.tz_name,  prefs.getString("tz_name",  "America/Los_Angeles").c_str(), sizeof(cfg.tz_name)-1);
    cfg.tz_name[sizeof(cfg.tz_name)-1] = '\0';
    cfg.lat      = prefs.getFloat("lat",  DEFAULT_LAT);
    cfg.lon      = prefs.getFloat("lon",  DEFAULT_LON);
    cfg.darkMode = prefs.getBool("dark_mode", false);
    prefs.end();
}

static void saveConfig() {
    Preferences prefs;
    prefs.begin("shravclock", false);  // read-write
    prefs.putString("ssid",     cfg.ssid);
    prefs.putString("pass",     cfg.pass);
    prefs.putString("tz_posix", cfg.tz_posix);
    prefs.putString("tz_name",  cfg.tz_name);
    prefs.putFloat("lat",       cfg.lat);
    prefs.putFloat("lon",       cfg.lon);
    prefs.putBool("dark_mode",  cfg.darkMode);
    prefs.end();
}

// --- Layout helpers ---
struct Rect { uint16_t x, y, w, h; };
static inline uint16_t align8(uint16_t v) { return (v / 8) * 8; }
static inline uint16_t ceil8 (uint16_t v) { return ((v + 7) / 8) * 8; }

Rect    timeRect;
int16_t timeCursorX    = 0, timeCursorY    = 0;
int16_t dayNameCursorY = 0;
int16_t dateCursorY    = 0;
int16_t weatherCursorX = 0;
int16_t weatherCursorY = 0;
int16_t weatherTextX   = 0;
int16_t weatherIconY   = 0;
int16_t loHiCursorY    = 0;
int16_t cityNameCursorY = 0;

// Dark mode — FG/BG are always reassigned in setup() before any draw call
static bool     darkMode = false;
static uint16_t FG       = GxEPD_BLACK;  // foreground color
static uint16_t BG       = GxEPD_WHITE;  // background color

// Weather state
static bool   hasWeather   = false;
static int    weatherCode  = 0;
static bool   weatherIsDay = true;
static float  weatherTempF = 0.0f;
static float  weatherLowF  = 0.0f;
static float  weatherHighF = 0.0f;

// City name working global
static char cityName[CITY_NAME_MAX] = "";


// ---- Meteocons character picker (WMO weather codes) ----
static char pickMeteoconChar(int code, bool isDay)
{
    if (code == 0)                                return isDay ? 'B' : 'C'; // clear sky / clear night
    if (code == 1 || code == 2)                   return isDay ? 'H' : 'I'; // partly cloudy day/night
    if (code == 3)                                return 'N'; // overcast
    if (code == 45 || code == 48)                 return 'M'; // fog
    if (code >= 51  && code <= 67)                return 'Q'; // drizzle / rain
    if ((code >= 71 && code <= 77) ||
         code == 85 || code == 86)                return 'W'; // snow
    if (code >= 80  && code <= 82)                return 'R'; // rain showers
    if (code == 95 || code == 96 || code == 99)   return 'P'; // thunderstorm
    return 'N'; // default: cloudy
}

// ---- Squix / ThingPulse column-major font renderer ----
static void drawMeteoconGlyph(int16_t x, int16_t y, char symbol,
                               const uint8_t *fontData, uint8_t scale = 2)
{
    uint8_t fontH  = pgm_read_byte(fontData + 1);
    uint8_t first  = pgm_read_byte(fontData + 2);
    uint8_t nChars = pgm_read_byte(fontData + 3);

    if ((uint8_t)symbol < first) return;
    uint8_t idx = (uint8_t)symbol - first;
    if (idx >= nChars) return;

    const uint8_t *jt  = fontData + 4;
    uint8_t msb = pgm_read_byte(jt + idx * 4 + 0);
    uint8_t lsb = pgm_read_byte(jt + idx * 4 + 1);
    if (msb == 0xFF && lsb == 0xFF) return;

    uint16_t off = ((uint16_t)msb << 8) | lsb;
    uint8_t  sz  = pgm_read_byte(jt + idx * 4 + 2);
    if (sz == 0) return;

    uint8_t rH = 1 + ((fontH - 1) >> 3);
    const uint8_t *bmp = fontData + 4 + (uint16_t)nChars * 4 + off;

    for (uint16_t i = 0; i < sz; i++) {
        uint8_t col  = i / rH;
        uint8_t rByt = i % rH;
        uint8_t bval = pgm_read_byte(bmp + i);

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (bval & (1 << bit)) {
                int16_t px = x + (int16_t)col  * scale;
                int16_t py = y + (int16_t)(rByt * 8 + bit) * scale;
                if (py < y + (int16_t)fontH * scale) {
                    if (scale <= 1) {
                        display.drawPixel(px, py, FG);
                    } else {
                        display.fillRect(px, py, scale, scale, FG);
                    }
                }
            }
        }
    }
}

// ---- WiFi ----
static bool wifiConnect()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    Serial.print("WiFi connecting");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi failed.");
        return false;
    }
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
    // WL_CONNECTED is raised before the TCP/IP stack is fully ready for TLS.
    // Starting an HTTPS request immediately causes the SSL handshake to stall
    // and fail with EOF after ~10 s. Cold boot avoids this because NTP sync
    // adds several seconds of delay before the first HTTPS call. Timer wakes
    // fire HTTPS immediately, so an explicit settle delay is needed.
    delay(1000);
    return true;
}

static void wifiDisconnect()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
}

// ---- RTC ----
static bool rtcNowLocal(struct tm &local_tm, time_t &epochUtc)
{
    DateTime utc = rtc.now();
    epochUtc = (time_t)utc.unixtime();
    return localtime_r(&epochUtc, &local_tm) != nullptr;
}

// ---- NTP + RTC sync ----
static bool fetchUtcEpochFromNTP(time_t &outUtc)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    // Zero the system clock before starting SNTP. The ESP32 preserves its
    // system time in RTC slow memory across resets, so time(nullptr) can
    // return a stale value from the previous session immediately — causing
    // the loop below to exit before a real NTP packet has been received.
    // Zeroing first ensures we wait for a genuine fresh sync.
    struct timeval tv_zero = {0, 0};
    settimeofday(&tv_zero, nullptr);

    configTime(0, 0, NTP1, NTP2);
    Serial.println("Waiting for NTP...");
    for (int i = 0; i < 60; i++) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            outUtc = now;
            setenv("TZ", cfg.tz_posix, 1);
            tzset();
            Serial.println("NTP OK.");
            return true;
        }
        delay(250);
    }
    Serial.println("NTP timeout.");
    return false;
}

static void syncRTCFromNTP_UTC()
{
    time_t utc;
    if (!fetchUtcEpochFromNTP(utc)) return;
    rtc.adjust(DateTime((uint32_t)utc));
    Serial.println("RTC adjusted from NTP (UTC).");
}

// ---- Weather fetch (with retry) ----
static bool fetchWeatherNow()
{
    if (WiFi.status() != WL_CONNECTED) return false;

    // HTTP (not HTTPS): open-meteo is a public API with no auth, so TLS adds
    // no real security. The ESP32-C6's mbedTLS stack has intermittent handshake
    // failures with open-meteo's TLS config; plain HTTP is reliable.
    String url = "http://api.open-meteo.com/v1/forecast"
                 "?latitude="  + String(cfg.lat, 4) +
                 "&longitude=" + String(cfg.lon, 4) +
                 "&current=temperature_2m,weather_code,is_day"
                 "&daily=temperature_2m_min,temperature_2m_max"
                 "&temperature_unit=fahrenheit&timezone=auto";

    for (int attempt = 1; attempt <= 3; attempt++) {
        WiFiClient client;
        HTTPClient http;
        if (!http.begin(client, url)) {
            Serial.printf("Weather attempt %d: http.begin failed\n", attempt);
            if (attempt < 3) delay(2000);
            continue;
        }

        int code = http.GET();
        if (code != 200) {
            Serial.printf("Weather attempt %d: HTTP %d\n", attempt, code);
            http.end();
            if (attempt < 3) delay(2000);
            continue;
        }

        String body = http.getString();
        http.end();

        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            Serial.printf("Weather attempt %d: JSON parse error\n", attempt);
            if (attempt < 3) delay(2000);
            continue;
        }

        weatherTempF  = doc["current"]["temperature_2m"].as<float>();
        weatherCode   = doc["current"]["weather_code"].as<int>();
        weatherIsDay  = doc["current"]["is_day"].as<int>() != 0;
        weatherLowF   = doc["daily"]["temperature_2m_min"][0].as<float>();
        weatherHighF  = doc["daily"]["temperature_2m_max"][0].as<float>();
        hasWeather    = true;
        Serial.printf("Weather OK (attempt %d): %.1fF code=%d isDay=%d Lo=%.1f Hi=%.1f\n",
                      attempt, weatherTempF, weatherCode, (int)weatherIsDay,
                      weatherLowF, weatherHighF);
        return true;
    }

    Serial.println("Weather: all 3 attempts failed.");
    return false;
}

// ---- City name fetch from Nominatim ----
static bool fetchCityName()
{
    if (WiFi.status() != WL_CONNECTED) return false;

    String url = "https://nominatim.openstreetmap.org/reverse?lat=" +
                 String(cfg.lat, 6) + "&lon=" + String(cfg.lon, 6) +
                 "&format=json&zoom=10";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.addHeader("User-Agent", "shravclock/1.0");
    if (!http.begin(client, url)) {
        Serial.println("CityName: http.begin failed");
        return false;
    }

    // Need to re-add header after begin
    http.addHeader("User-Agent", "shravclock/1.0");
    int code = http.GET();
    if (code != 200) {
        Serial.printf("CityName: HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("CityName: JSON parse error");
        return false;
    }

    const char *city = doc["address"]["city"] | "";
    if (!city || city[0] == '\0') city = doc["address"]["town"] | "";
    if (!city || city[0] == '\0') city = doc["address"]["village"] | "";
    if (!city || city[0] == '\0') city = doc["display_name"] | "";

    if (city && city[0] != '\0') {
        strncpy(cityName, city, CITY_NAME_MAX - 1);
        cityName[CITY_NAME_MAX - 1] = '\0';
        Serial.printf("CityName: %s\n", cityName);
        return true;
    }

    Serial.println("CityName: not found in response");
    return false;
}

// ---- Layout computation ----
static void computeLayout()
{
    display.setRotation(0);

    const int16_t  topY        = 70;   // time baseline
    const int16_t  gapDayName  = 47;   // → dayNameCursorY = 117
    const int16_t  gapDate     = 87;   // → dateCursorY    = 157
    const int16_t  gapWeather  = 153;  // → weatherCursorY = 223 (moved up to make room for city)
    const uint16_t cx         = display.width() / 2; // 200

    int16_t  tbx, tby;
    uint16_t tbw, tbh;

    // ---- TIME (2x scale) ----
    display.setFont(&FreeSansBold18pt7b);
    display.setTextSize(2);
    display.getTextBounds("23:59", 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setTextSize(1);

    timeCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
    timeCursorY = topY;

    int16_t winX = timeCursorX + tbx - 10;
    int16_t winY = timeCursorY + tby - 10;
    if (winX < 0) winX = 0;
    if (winY < 0) winY = 0;

    timeRect.x = align8((uint16_t)winX);
    timeRect.y = (uint16_t)winY;
    timeRect.w = ceil8(tbw + 20);
    timeRect.h = (uint16_t)(tbh + 20);

    // ---- DATE (two lines) ----
    dayNameCursorY = topY + gapDayName;
    dateCursorY    = topY + gapDate;

    // ---- WEATHER ROW ----
    display.setFont(&FreeSansBold24pt7b);
    display.getTextBounds("99F", 0, 0, &tbx, &tby, &tbw, &tbh);

    const uint16_t iconW   = 37;
    const uint16_t iconGap = 10;
    const uint16_t degW    = 14;
    uint16_t       totalW  = iconW + iconGap + tbw + degW;

    weatherCursorX = (int16_t)cx - (int16_t)(totalW / 2);
    weatherCursorY = topY + gapWeather;
    weatherTextX   = weatherCursorX + (int16_t)(iconW + iconGap);
    weatherIconY   = weatherCursorY + tby + (int16_t)(tbh / 2) - 19;

    // Lo|Hi baseline
    int16_t iconBottom  = weatherIconY + 38;
    int16_t blockBottom = max(iconBottom, (int16_t)weatherCursorY);
    int16_t lhx2, lhy2; uint16_t lhw2, lhh2;
    display.setFont(&FreeSans12pt7b);
    display.getTextBounds("Lo 88 | Hi 88", 0, 0, &lhx2, &lhy2, &lhw2, &lhh2);
    loHiCursorY = blockBottom + 8 - lhy2;

    // City name baseline: below Lo|Hi text
    int16_t loHiTextBottom = loHiCursorY + (int16_t)lhh2 + lhy2;
    int16_t cnx2, cny2; uint16_t cnw2, cnh2;
    display.setFont(&FreeSans12pt7b);
    display.getTextBounds("A", 0, 0, &cnx2, &cny2, &cnw2, &cnh2);
    cityNameCursorY = loHiTextBottom + 6 - cny2;  // 6px gap

    Serial.printf("timeRect    x=%u y=%u w=%u h=%u cursor=(%d,%d)\n",
                  timeRect.x, timeRect.y, timeRect.w, timeRect.h,
                  timeCursorX, timeCursorY);
    Serial.printf("weatherRow  iconLeft=%d iconTop=%d textX=%d baseline=%d loHiY=%d cityY=%d\n",
                  weatherCursorX, weatherIconY, weatherTextX, weatherCursorY,
                  loHiCursorY, cityNameCursorY);
}

// ---- Draw helpers ----

static void drawDateLine(const tm &nowL)
{
    static const char *WDAY[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char *MON[]  = {"",
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"};

    char dayBuf[12];
    char dateBuf[20];
    snprintf(dayBuf,  sizeof(dayBuf),  "%s", WDAY[nowL.tm_wday % 7]);
    snprintf(dateBuf, sizeof(dateBuf), "%s %d, %d",
             MON[(nowL.tm_mon + 1) % 13],
             nowL.tm_mday,
             nowL.tm_year + 1900);

    display.setTextSize(1);
    display.setTextColor(FG);

    // Line 1: full day name, bold, centered
    int16_t bx, by; uint16_t bw, bh;
    display.setFont(&FreeSansBold18pt7b);
    display.getTextBounds(dayBuf, 0, 0, &bx, &by, &bw, &bh);
    display.setCursor((int16_t)(display.width() / 2) - (int16_t)(bw / 2) - bx, dayNameCursorY);
    display.print(dayBuf);

    // Line 2: date only, regular, centered
    int16_t rx, ry; uint16_t rw, rh;
    display.setFont(&FreeSans18pt7b);
    display.getTextBounds(dateBuf, 0, 0, &rx, &ry, &rw, &rh);
    display.setCursor((int16_t)(display.width() / 2) - (int16_t)(rw / 2) - rx, dateCursorY);
    display.print(dateBuf);
}

static void drawWeatherRow()
{
    // Meteocons icon at 1x scale (38px)
    char sym = hasWeather ? pickMeteoconChar(weatherCode, weatherIsDay) : 'N';
    drawMeteoconGlyph(weatherCursorX, weatherIconY, sym, Meteocons_Plain_36, 1);

    // Temperature at 24pt 1x
    display.setFont(&FreeSansBold24pt7b);
    display.setTextColor(FG);
    display.setCursor(weatherTextX, weatherCursorY);

    if (hasWeather) {
        display.print((int)lroundf(weatherTempF));
    } else {
        display.print("--");
    }

    // Degree circle sized for 24pt
    int16_t cx = display.getCursorX();
    int16_t cy = display.getCursorY();
    display.drawCircle(cx + 4, cy - 30, 3, FG);

    // 'F'
    display.setCursor(cx + 11, cy);
    display.print("F");

    // Lo|Hi for the day
    display.setFont(&FreeSans12pt7b);
    display.setTextColor(FG);

    const int16_t  degR       = 2;
    const int16_t  degCapY    = 15;
    const int16_t  degAdvance = degR * 2 + 3;

    if (!hasWeather) {
        int16_t fx, fy; uint16_t fw, fh;
        display.getTextBounds("Lo -- | Hi --", 0, 0, &fx, &fy, &fw, &fh);
        display.setCursor((int16_t)(display.width() / 2) - (int16_t)(fw / 2) - fx, loHiCursorY);
        display.print("Lo -- | Hi --");
    } else {
        char loNumBuf[6], hiNumBuf[6];
        snprintf(loNumBuf, sizeof(loNumBuf), "%d", (int)lroundf(weatherLowF));
        snprintf(hiNumBuf, sizeof(hiNumBuf), "%d", (int)lroundf(weatherHighF));

        int16_t ax, ay; uint16_t aw, ah;
        display.getTextBounds("Lo ", 0, 0, &ax, &ay, &aw, &ah);
        int16_t bx2, by2; uint16_t bw2, bh2;
        display.getTextBounds(loNumBuf, 0, 0, &bx2, &by2, &bw2, &bh2);
        int16_t sx, sy; uint16_t sw, sh;
        display.getTextBounds(" | Hi ", 0, 0, &sx, &sy, &sw, &sh);
        int16_t dx, dy; uint16_t dw, dh;
        display.getTextBounds(hiNumBuf, 0, 0, &dx, &dy, &dw, &dh);

        int16_t totalW2 = (int16_t)(aw + bw2 + degAdvance + sw + dw + degAdvance);
        int16_t startX = (int16_t)(display.width() / 2) - totalW2 / 2 - ax;

        display.setCursor(startX, loHiCursorY);
        display.print("Lo ");
        display.print(loNumBuf);

        int16_t dcx = display.getCursorX(), dcy = display.getCursorY();
        display.drawCircle(dcx + degR + 1, dcy - degCapY, degR, FG);
        display.setCursor(dcx + degAdvance, dcy);

        display.print(" | Hi ");
        display.print(hiNumBuf);

        dcx = display.getCursorX(); dcy = display.getCursorY();
        display.drawCircle(dcx + degR + 1, dcy - degCapY, degR, FG);
    }

    // City name below Lo|Hi
    display.setFont(&FreeSans12pt7b);
    display.setTextColor(FG);
    int16_t cnx, cny; uint16_t cnw, cnh;
    const char *cn = cityName[0] ? cityName : "---";
    display.getTextBounds(cn, 0, 0, &cnx, &cny, &cnw, &cnh);
    display.setCursor((int16_t)(display.width() / 2) - (int16_t)(cnw / 2) - cnx, cityNameCursorY);
    display.print(cn);
}

// Overlays — crescent moon top-left when dark mode is on
static void drawOverlays()
{
    if (darkMode) {
        const int16_t mx = 13, my = 13, mr = 9;
        display.fillCircle(mx, my, mr, FG);
        display.fillCircle(mx + 5, my - 4, mr - 2, BG);
    }
}


static void drawFull(const tm &nowL)
{
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(BG);

        display.setFont(&FreeSansBold18pt7b);
        display.setTextSize(2);
        display.setTextColor(FG);
        display.setCursor(timeCursorX, timeCursorY);
        display.print(timeBuf);
        display.setTextSize(1);

        drawDateLine(nowL);
        drawWeatherRow();
        drawOverlays();

    } while (display.nextPage());
}

static void drawTimePartial(const tm &nowL)
{
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

    display.setRotation(0);
    display.setPartialWindow(timeRect.x, timeRect.y, timeRect.w, timeRect.h);
    display.firstPage();
    do {
        display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, BG);
        display.setFont(&FreeSansBold18pt7b);
        display.setTextSize(2);
        display.setTextColor(FG);
        display.setCursor(timeCursorX, timeCursorY);
        display.print(buf);
        display.setTextSize(1);
    } while (display.nextPage());
}

static void i2cScan()
{
    Serial.println("Scanning I2C...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Found 0x%02X\n", addr);
            found++;
        }
    }
    if (!found) Serial.println("  No I2C devices found.");
}

// ---- Deep sleep ----
static void goToSleep()
{
    DateTime now = rtc.now();
    uint8_t sec = now.second();
    // Wake at :58 — weather pre-fetch at :58/:59, display refresh at :00,
    // then stay awake until :05 for button press and upload access.
    uint32_t sleepSec = (sec < 58) ? (58 - sec) : (60 - sec + 58);
    if (sleepSec < 1) sleepSec = 1;

    Serial.printf("Sleeping %us (sec=%u)\n", sleepSec, sec);
    display.hibernate();
    // Hold display control pins HIGH during deep sleep.
    // Without this, the ESP32-C6 boot ROM resets all GPIO before Arduino code
    // runs, causing RST (GPIO 21) to glitch LOW — which wakes the SSD1683 from
    // hibernate and produces a visible flash at :58 on every wake cycle.
    gpio_hold_en((gpio_num_t)RES_PIN);
    gpio_hold_en((gpio_num_t)CS_PIN);
    gpio_hold_en((gpio_num_t)DC_PIN);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
    // GPIO 17 is not an LP GPIO on ESP32-C6, so deep sleep GPIO wakeup
    // is not supported — button only works during the :00–:05 active window.
    esp_deep_sleep_start();
}

// ---- Setup screen for portal mode ----
static void drawSetupScreen() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);

        // "shravclock" - title
        display.setFont(&FreeSansBold18pt7b);
        display.setTextSize(1);
        int16_t tx, ty; uint16_t tw, th;
        display.getTextBounds("shravclock", 0, 0, &tx, &ty, &tw, &th);
        display.setCursor((display.width()-tw)/2 - tx, 50);
        display.print("shravclock");

        // Divider line
        display.drawLine(40, 65, 360, 65, GxEPD_BLACK);

        // Info lines in FreeSans12pt7b
        display.setFont(&FreeSans12pt7b);
        const char* lines[] = {"Setup Mode", "", "WiFi: shravclock-setup", "Pass: shravann", "Open: 192.168.4.1"};
        int y = 100;
        for (auto line : lines) {
            if (line[0] != '\0') {
                display.getTextBounds(line, 0, 0, &tx, &ty, &tw, &th);
                display.setCursor((display.width()-tw)/2 - tx, y);
                display.print(line);
            }
            y += 32;
        }
    } while (display.nextPage());
}

// ---- URL encode helper ----
static String urlencode(const String &s) {
    String enc;
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            enc += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            enc += buf;
        }
    }
    return enc;
}

// ---- Timezone IANA name lookup from POSIX string ----
// Finds the IANA name for a given POSIX tz string (best-effort, first match)
static const char* findIanaName(const char* posix) {
    // Small lookup table — same entries as the HTML page TZ array
    static const char* TZ_TABLE[][2] = {
        {"Africa/Abidjan","GMT0"},{"Africa/Accra","GMT0"},{"Africa/Addis_Ababa","EAT-3"},
        {"Africa/Algiers","CET-1"},{"Africa/Cairo","EET-2"},{"Africa/Casablanca","<+01>-1"},
        {"Africa/Johannesburg","SAST-2"},{"Africa/Kampala","EAT-3"},{"Africa/Lagos","WAT-1"},
        {"Africa/Nairobi","EAT-3"},{"Africa/Tunis","CET-1"},
        {"America/Anchorage","AKST9AKDT,M3.2.0,M11.1.0"},
        {"America/Argentina/Buenos_Aires","<-03>3"},
        {"America/Bogota","<-05>5"},
        {"America/Chicago","CST6CDT,M3.2.0,M11.1.0"},
        {"America/Denver","MST7MDT,M3.2.0,M11.1.0"},
        {"America/Halifax","AST4ADT,M3.2.0,M11.1.0"},
        {"America/Havana","CST5CDT,M3.2.0.0/0,M10.5.0/1"},
        {"America/Honolulu","HST10"},
        {"America/Lima","<-05>5"},
        {"America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"},
        {"America/Mexico_City","CST6CDT,M4.1.0,M10.5.0"},
        {"America/New_York","EST5EDT,M3.2.0,M11.1.0"},
        {"America/Phoenix","MST7"},
        {"America/Santiago","<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
        {"America/Sao_Paulo","<-03>3"},
        {"America/St_Johns","NST3:30NDT,M3.2.0,M11.1.0"},
        {"America/Toronto","EST5EDT,M3.2.0,M11.1.0"},
        {"America/Vancouver","PST8PDT,M3.2.0,M11.1.0"},
        {"Asia/Almaty","<+06>-6"},
        {"Asia/Amman","EET-2EEST,M3.5.4/0,M10.5.5/1"},
        {"Asia/Baghdad","<+03>-3"},
        {"Asia/Baku","<+04>-4"},
        {"Asia/Bangkok","<+07>-7"},
        {"Asia/Beirut","EET-2EEST,M3.5.0/0,M10.5.0/0"},
        {"Asia/Colombo","<+0530>-5:30"},
        {"Asia/Dhaka","<+06>-6"},
        {"Asia/Dubai","<+04>-4"},
        {"Asia/Hong_Kong","HKT-8"},
        {"Asia/Jakarta","WIB-7"},
        {"Asia/Jerusalem","IST-2IDT,M3.4.4/26,M10.5.0"},
        {"Asia/Kabul","<+0430>-4:30"},
        {"Asia/Karachi","PKT-5"},
        {"Asia/Kathmandu","<+0545>-5:45"},
        {"Asia/Kolkata","IST-5:30"},
        {"Asia/Kuala_Lumpur","<+08>-8"},
        {"Asia/Kuwait","<+03>-3"},
        {"Asia/Manila","PST-8"},
        {"Asia/Muscat","<+04>-4"},
        {"Asia/Nicosia","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Asia/Qatar","<+03>-3"},
        {"Asia/Riyadh","<+03>-3"},
        {"Asia/Seoul","KST-9"},
        {"Asia/Shanghai","CST-8"},
        {"Asia/Singapore","<+08>-8"},
        {"Asia/Taipei","CST-8"},
        {"Asia/Tashkent","<+05>-5"},
        {"Asia/Tbilisi","<+04>-4"},
        {"Asia/Tehran","<+0330>-3:30<+0430>,80/0,264/0"},
        {"Asia/Tokyo","JST-9"},
        {"Asia/Ulaanbaatar","<+08>-8"},
        {"Asia/Yangon","<+0630>-6:30"},
        {"Asia/Yerevan","<+04>-4"},
        {"Atlantic/Azores","<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
        {"Atlantic/Reykjavik","GMT0"},
        {"Australia/Adelaide","ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
        {"Australia/Brisbane","AEST-10"},
        {"Australia/Darwin","ACST-9:30"},
        {"Australia/Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Perth","AWST-8"},
        {"Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Europe/Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Athens","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Belgrade","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Berlin","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Brussels","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Bucharest","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Budapest","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Copenhagen","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Dublin","IST-1GMT0,M10.5.0,M3.5.0/1"},
        {"Europe/Helsinki","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Istanbul","<+03>-3"},
        {"Europe/Kiev","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Lisbon","WET0WEST,M3.5.0/1,M10.5.0"},
        {"Europe/London","GMT0BST,M3.5.0/1,M10.5.0"},
        {"Europe/Luxembourg","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Madrid","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Moscow","MSK-3"},
        {"Europe/Oslo","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Paris","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Prague","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Rome","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Sofia","EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Stockholm","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Vienna","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Warsaw","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Zurich","CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Pacific/Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3"},
        {"Pacific/Fiji","<+12>-12"},
        {"Pacific/Guam","ChST-10"},
        {"Pacific/Honolulu","HST10"},
        {"Pacific/Noumea","<+11>-11"},
        {"Pacific/Port_Moresby","<+10>-10"},
        {"Pacific/Tahiti","<-10>10"},
        {"UTC","UTC0"},
        {nullptr, nullptr}
    };
    for (int i = 0; TZ_TABLE[i][0] != nullptr; i++) {
        if (strcmp(TZ_TABLE[i][1], posix) == 0) {
            return TZ_TABLE[i][0];
        }
    }
    return nullptr;
}

// ---- Setup portal HTML ----
static const char SETUP_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>shravclock setup</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, sans-serif; background: #f5f5f5; color: #222; padding: 20px; }
h1 { font-size: 1.6em; margin-bottom: 4px; }
.sub { color: #666; font-size: 0.85em; margin-bottom: 24px; }
.card { background: #fff; border-radius: 12px; padding: 20px; margin-bottom: 16px; box-shadow: 0 1px 4px rgba(0,0,0,.1); }
.card h2 { font-size: 1em; font-weight: 600; margin-bottom: 14px; color: #444; text-transform: uppercase; letter-spacing: .05em; }
label { display: block; font-size: 0.85em; color: #555; margin-bottom: 4px; margin-top: 10px; }
label:first-of-type { margin-top: 0; }
input[type=text], input[type=password], select { width: 100%; padding: 10px 12px; border: 1px solid #ddd; border-radius: 8px; font-size: 1em; background: #fafafa; }
input:focus, select:focus { outline: none; border-color: #4a90e2; background: #fff; }
.row { display: flex; gap: 8px; align-items: flex-end; }
.row input { flex: 1; }
button.search { padding: 10px 16px; background: #4a90e2; color: #fff; border: none; border-radius: 8px; font-size: 0.95em; cursor: pointer; white-space: nowrap; }
button.search:hover { background: #357abd; }
#city-results { margin-top: 6px; display: none; }
.wifi-status { padding: 10px 12px; background: #e8f5e9; border: 1px solid #a5d6a7; border-radius: 8px; color: #2e7d32; font-size: 0.9em; }
.toggle-label { display: flex; align-items: center; gap: 10px; cursor: pointer; }
.toggle { position: relative; width: 46px; height: 26px; flex-shrink: 0; }
.toggle input { opacity: 0; width: 0; height: 0; }
.slider { position: absolute; inset: 0; background: #ccc; border-radius: 26px; transition: .3s; }
.slider:before { content: ""; position: absolute; height: 20px; width: 20px; left: 3px; bottom: 3px; background: #fff; border-radius: 50%; transition: .3s; }
input:checked + .slider { background: #333; }
input:checked + .slider:before { transform: translateX(20px); }
button[type=submit] { width: 100%; padding: 14px; background: #222; color: #fff; border: none; border-radius: 10px; font-size: 1.05em; font-weight: 600; cursor: pointer; margin-top: 8px; }
button[type=submit]:hover { background: #000; }
.saved { display: none; text-align: center; padding: 40px 20px; }
.saved h2 { font-size: 1.4em; margin-bottom: 8px; }
.saved p { color: #666; }
</style>
</head>
<body>
<h1>shravclock</h1>
<p class="sub">Device configuration</p>

<div id="main-form">

<div class="card" id="wifi-card">
  <h2>Wi-Fi</h2>
  <div id="wifi-status-div">Loading...</div>
</div>

<div class="card">
  <h2>Location</h2>
  <label>Search city</label>
  <div class="row">
    <input type="text" id="city-input" placeholder="e.g. San Francisco">
    <button type="button" class="search" onclick="searchCity()">Search</button>
  </div>
  <select id="city-results" size="5" onchange="selectCity(this.value)"></select>
  <input type="hidden" name="lat" id="lat" form="config-form">
  <input type="hidden" name="lon" id="lon" form="config-form">
  <div id="city-chosen" style="margin-top:8px;font-size:.9em;color:#555;"></div>
</div>

<div class="card">
  <h2>Timezone</h2>
  <label>Search timezone</label>
  <input type="text" id="tz-search" placeholder="Filter timezones..." oninput="filterTz(this.value)">
  <label style="margin-top:8px">Select timezone</label>
  <select name="tz" id="tz-select" form="config-form" size="6" style="margin-top:4px"></select>
</div>

<div class="card">
  <h2>Appearance</h2>
  <label class="toggle-label">
    <span>Dark mode</span>
    <label class="toggle">
      <input type="checkbox" name="dark_mode" id="dark-toggle" form="config-form" value="1">
      <span class="slider"></span>
    </label>
  </label>
</div>

<form id="config-form" action="/save" method="POST">
  <input type="hidden" name="ssid" id="f-ssid">
  <input type="hidden" name="pass" id="f-pass">
  <button type="submit">Save &amp; Restart</button>
</form>

</div>

<div class="saved" id="saved-div">
  <h2>Saved!</h2>
  <p>shravclock is restarting...</p>
</div>

<script>
// ---- Timezones ----
const TZ = [
  ["Africa/Abidjan","GMT0"],["Africa/Accra","GMT0"],["Africa/Addis_Ababa","EAT-3"],
  ["Africa/Algiers","CET-1"],["Africa/Cairo","EET-2"],["Africa/Casablanca","<+01>-1"],
  ["Africa/Johannesburg","SAST-2"],["Africa/Kampala","EAT-3"],["Africa/Lagos","WAT-1"],
  ["Africa/Nairobi","EAT-3"],["Africa/Tunis","CET-1"],
  ["America/Anchorage","AKST9AKDT,M3.2.0,M11.1.0"],
  ["America/Argentina/Buenos_Aires","<-03>3"],
  ["America/Bogota","<-05>5"],
  ["America/Chicago","CST6CDT,M3.2.0,M11.1.0"],
  ["America/Denver","MST7MDT,M3.2.0,M11.1.0"],
  ["America/Halifax","AST4ADT,M3.2.0,M11.1.0"],
  ["America/Havana","CST5CDT,M3.2.0.0/0,M10.5.0/1"],
  ["America/Honolulu","HST10"],
  ["America/Lima","<-05>5"],
  ["America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"],
  ["America/Mexico_City","CST6CDT,M4.1.0,M10.5.0"],
  ["America/New_York","EST5EDT,M3.2.0,M11.1.0"],
  ["America/Phoenix","MST7"],
  ["America/Santiago","<-04>4<-03>,M9.1.6/24,M4.1.6/24"],
  ["America/Sao_Paulo","<-03>3"],
  ["America/St_Johns","NST3:30NDT,M3.2.0,M11.1.0"],
  ["America/Toronto","EST5EDT,M3.2.0,M11.1.0"],
  ["America/Vancouver","PST8PDT,M3.2.0,M11.1.0"],
  ["Asia/Almaty","<+06>-6"],
  ["Asia/Amman","EET-2EEST,M3.5.4/0,M10.5.5/1"],
  ["Asia/Baghdad","<+03>-3"],
  ["Asia/Baku","<+04>-4"],
  ["Asia/Bangkok","<+07>-7"],
  ["Asia/Beirut","EET-2EEST,M3.5.0/0,M10.5.0/0"],
  ["Asia/Colombo","<+0530>-5:30"],
  ["Asia/Dhaka","<+06>-6"],
  ["Asia/Dubai","<+04>-4"],
  ["Asia/Hong_Kong","HKT-8"],
  ["Asia/Jakarta","WIB-7"],
  ["Asia/Jerusalem","IST-2IDT,M3.4.4/26,M10.5.0"],
  ["Asia/Kabul","<+0430>-4:30"],
  ["Asia/Karachi","PKT-5"],
  ["Asia/Kathmandu","<+0545>-5:45"],
  ["Asia/Kolkata","IST-5:30"],
  ["Asia/Kuala_Lumpur","<+08>-8"],
  ["Asia/Kuwait","<+03>-3"],
  ["Asia/Manila","PST-8"],
  ["Asia/Muscat","<+04>-4"],
  ["Asia/Nicosia","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Asia/Qatar","<+03>-3"],
  ["Asia/Riyadh","<+03>-3"],
  ["Asia/Seoul","KST-9"],
  ["Asia/Shanghai","CST-8"],
  ["Asia/Singapore","<+08>-8"],
  ["Asia/Taipei","CST-8"],
  ["Asia/Tashkent","<+05>-5"],
  ["Asia/Tbilisi","<+04>-4"],
  ["Asia/Tehran","<+0330>-3:30<+0430>,80/0,264/0"],
  ["Asia/Tokyo","JST-9"],
  ["Asia/Ulaanbaatar","<+08>-8"],
  ["Asia/Yangon","<+0630>-6:30"],
  ["Asia/Yerevan","<+04>-4"],
  ["Atlantic/Azores","<-01>1<+00>,M3.5.0/0,M10.5.0/1"],
  ["Atlantic/Reykjavik","GMT0"],
  ["Australia/Adelaide","ACST-9:30ACDT,M10.1.0,M4.1.0/3"],
  ["Australia/Brisbane","AEST-10"],
  ["Australia/Darwin","ACST-9:30"],
  ["Australia/Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3"],
  ["Australia/Perth","AWST-8"],
  ["Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"],
  ["Europe/Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Athens","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Europe/Belgrade","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Berlin","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Brussels","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Bucharest","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Europe/Budapest","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Copenhagen","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Dublin","IST-1GMT0,M10.5.0,M3.5.0/1"],
  ["Europe/Helsinki","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Europe/Istanbul","<+03>-3"],
  ["Europe/Kiev","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Europe/Lisbon","WET0WEST,M3.5.0/1,M10.5.0"],
  ["Europe/London","GMT0BST,M3.5.0/1,M10.5.0"],
  ["Europe/Luxembourg","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Madrid","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Moscow","MSK-3"],
  ["Europe/Oslo","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Paris","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Prague","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Rome","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Sofia","EET-2EEST,M3.5.0/3,M10.5.0/4"],
  ["Europe/Stockholm","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Vienna","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Warsaw","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Europe/Zurich","CET-1CEST,M3.5.0,M10.5.0/3"],
  ["Pacific/Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3"],
  ["Pacific/Fiji","<+12>-12"],
  ["Pacific/Guam","ChST-10"],
  ["Pacific/Honolulu","HST10"],
  ["Pacific/Noumea","<+11>-11"],
  ["Pacific/Port_Moresby","<+10>-10"],
  ["Pacific/Tahiti","<-10>10"],
  ["UTC","UTC0"]
];

let allTz = [...TZ];

function buildTzSelect(list) {
  const sel = document.getElementById('tz-select');
  const cur = sel.value;
  sel.innerHTML = '';
  list.forEach(([name, posix]) => {
    const o = document.createElement('option');
    o.value = posix;
    o.textContent = name;
    if (posix === cur) o.selected = true;
    sel.appendChild(o);
  });
}

function filterTz(q) {
  const lower = q.toLowerCase();
  const filtered = allTz.filter(([name]) => name.toLowerCase().includes(lower));
  buildTzSelect(filtered.length ? filtered : allTz);
}

buildTzSelect(allTz);

// Pre-select current timezone sent from server
const curTzPosix = "%%CURRENT_TZ%%";
const curDark = %%CURRENT_DARK%%;
if (curDark) document.getElementById('dark-toggle').checked = true;
Array.from(document.getElementById('tz-select').options).forEach(o => {
  if (o.value === curTzPosix) { o.selected = true; }
});

// ---- WiFi status ----
fetch('/wifi-status').then(r=>r.json()).then(data => {
  const div = document.getElementById('wifi-status-div');
  if (data.connected) {
    div.innerHTML = '<div class="wifi-status">Connected to <strong>' + data.ssid + '</strong></div>';
    // Pre-fill hidden ssid/pass with current values so form submits them unchanged
    document.getElementById('f-ssid').value = data.ssid;
    document.getElementById('f-pass').value = '%%KEEP_PASS%%';
  } else {
    div.innerHTML = '<label>Network name (SSID)</label><input type="text" id="ssid-input" placeholder="Your WiFi name">' +
      '<label>Password</label><input type="password" id="pass-input" placeholder="WiFi password">';
  }
});

// ---- City search ----
function searchCity() {
  const q = document.getElementById('city-input').value.trim();
  if (!q) return;
  fetch('/search-city?q=' + encodeURIComponent(q))
    .then(r => r.json())
    .then(results => {
      const sel = document.getElementById('city-results');
      sel.innerHTML = '';
      sel.style.display = results.length ? 'block' : 'none';
      results.forEach(r => {
        const name = r.display_name;
        const lat = parseFloat(r.lat).toFixed(4);
        const lon = parseFloat(r.lon).toFixed(4);
        const o = document.createElement('option');
        o.value = JSON.stringify({name, lat, lon});
        o.textContent = name.length > 60 ? name.substring(0,57)+'...' : name;
        sel.appendChild(o);
      });
    })
    .catch(() => {
      document.getElementById('city-results').style.display = 'none';
      alert('City search unavailable - device may not be connected to WiFi.');
    });
}

function selectCity(val) {
  if (!val) return;
  const d = JSON.parse(val);
  document.getElementById('lat').value = d.lat;
  document.getElementById('lon').value = d.lon;
  document.getElementById('city-chosen').textContent = 'Selected: ' + d.name;
  document.getElementById('city-results').style.display = 'none';
}

// ---- Form submit ----
document.getElementById('config-form').addEventListener('submit', function(e) {
  // Pull ssid/pass from visible inputs if shown
  const ssidIn = document.getElementById('ssid-input');
  const passIn = document.getElementById('pass-input');
  if (ssidIn) document.getElementById('f-ssid').value = ssidIn.value;
  if (passIn) document.getElementById('f-pass').value = passIn.value;
});
</script>
</body>
</html>
)rawhtml";

// ---- Setup portal ----
static void runSetupPortal() {
    bool saved = false;

    // Draw setup screen on e-ink (init display here — may not have been
    // initialized yet if called from a timer wake before the :00 draw)
    display.init(115200, false, 50, false);
    display.cp437(true);
    drawSetupScreen();

    // Start AP+STA
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("shravclock-setup", "shravann");
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    // DNS server: respond to every query with 192.168.4.1 → captive portal
    DNSServer dns;
    dns.start(53, "*", WiFi.softAPIP());

    // Try connecting to saved home WiFi for city search proxy
    bool staConnected = false;
    if (strlen(cfg.ssid) > 0) {
        WiFi.begin(cfg.ssid, cfg.pass);
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) delay(100);
        staConnected = (WiFi.status() == WL_CONNECTED);
        Serial.printf("STA %s\n", staConnected ? "connected" : "failed");
    }

    WebServer server(80);

    // Serve main page
    server.on("/", HTTP_GET, [&]() {
        String html = String(SETUP_HTML);
        // Replace placeholders
        html.replace("%%CURRENT_TZ%%", cfg.tz_posix);
        html.replace("%%CURRENT_DARK%%", cfg.darkMode ? "true" : "false");
        html.replace("%%KEEP_PASS%%", "__keep__");
        server.send(200, "text/html", html);
    });

    // WiFi status
    server.on("/wifi-status", HTTP_GET, [&]() {
        String json = "{\"connected\":";
        json += staConnected ? "true" : "false";
        json += ",\"ssid\":\"";
        json += staConnected ? String(cfg.ssid) : "";
        json += "\"}";
        server.send(200, "application/json", json);
    });

    // City search proxy
    server.on("/search-city", HTTP_GET, [&]() {
        if (!staConnected) { server.send(200, "application/json", "[]"); return; }
        String q = server.arg("q");
        if (q.isEmpty()) { server.send(200, "application/json", "[]"); return; }

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String url = "https://nominatim.openstreetmap.org/search?q=" + urlencode(q) + "&format=json&limit=8&addressdetails=1";
        http.begin(client, url);
        http.addHeader("User-Agent", "shravclock/1.0");
        int code = http.GET();
        if (code == 200) {
            server.send(200, "application/json", http.getString());
        } else {
            server.send(200, "application/json", "[]");
        }
        http.end();
    });

    // Save config
    server.on("/save", HTTP_POST, [&]() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        String tz   = server.arg("tz");
        String latS = server.arg("lat");
        String lonS = server.arg("lon");
        bool dark   = server.hasArg("dark_mode");

        // Update cfg
        if (ssid.length() > 0) {
            strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid)-1);
            cfg.ssid[sizeof(cfg.ssid)-1] = '\0';
        }
        if (pass.length() > 0 && pass != "__keep__") {
            strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass)-1);
            cfg.pass[sizeof(cfg.pass)-1] = '\0';
        }
        if (tz.length() > 0) {
            strncpy(cfg.tz_posix, tz.c_str(), sizeof(cfg.tz_posix)-1);
            cfg.tz_posix[sizeof(cfg.tz_posix)-1] = '\0';
            // Find IANA name matching this POSIX string
            const char* ianaName = findIanaName(cfg.tz_posix);
            if (ianaName != nullptr) {
                strncpy(cfg.tz_name, ianaName, sizeof(cfg.tz_name)-1);
                cfg.tz_name[sizeof(cfg.tz_name)-1] = '\0';
            }
        }
        if (latS.length() > 0) cfg.lat = latS.toFloat();
        if (lonS.length() > 0) cfg.lon = lonS.toFloat();
        cfg.darkMode = dark;

        // Also update RTC_DATA_ATTR dark mode
        _darkMode = cfg.darkMode;

        saveConfig();
        saved = true;

        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
            "<h2>Saved!</h2><p>shravclock is restarting...</p>"
            "</body></html>");

        delay(1500);
    });

    // Captive portal detection URLs — iOS, Android, Windows all hit these
    // to check for internet. Redirecting them to / triggers the portal popup.
    auto redirect = [&]() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); };
    server.on("/hotspot-detect.html",          HTTP_GET, redirect);  // iOS
    server.on("/library/test/success.html",    HTTP_GET, redirect);  // iOS
    server.on("/generate_204",                 HTTP_GET, redirect);  // Android
    server.on("/gen_204",                      HTTP_GET, redirect);  // Android
    server.on("/connecttest.txt",              HTTP_GET, redirect);  // Windows
    server.on("/ncsi.txt",                     HTTP_GET, redirect);  // Windows
    server.onNotFound([&]() { redirect(); });                        // catch-all

    server.begin();

    uint32_t startMs = millis();
    const uint32_t TIMEOUT_MS = 5UL * 60 * 1000;

    while (!saved && (millis() - startMs < TIMEOUT_MS)) {
        dns.processNextRequest();
        server.handleClient();
        delay(5);
    }

    dns.stop();
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (saved) {
        Serial.println("Config saved - restarting.");
        delay(500);
        ESP.restart();
    } else {
        Serial.println("Portal timed out - resuming sleep.");
        // Re-init display for dark mode and go back to normal operation
        display.init(115200, false, 50, false);
        goToSleep();
    }
}

// =========================================================
void setup()
{
    Serial.begin(115200);
    delay(100);

    // Load config from NVS first — before anything else
    loadConfig();

    // Determine wake cause
    esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
    bool timerWake  = (wakeupCause == ESP_SLEEP_WAKEUP_TIMER);
    bool buttonWake = (wakeupCause == ESP_SLEEP_WAKEUP_GPIO);
    bool coldBoot   = (!timerWake && !buttonWake) || !_rtcValid;

    Serial.printf("Wake: cause=%d timer=%d button=%d cold=%d rtcValid=%d\n",
                  (int)wakeupCause, (int)timerWake, (int)buttonWake,
                  (int)coldBoot, (int)_rtcValid);

    // Basic hardware
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    // Read button immediately — before display.init() or any other slow operation
    // so a press at wake time isn't missed by the time we check it later.
    bool buttonEarlyRead = (digitalRead(BUTTON_PIN) == LOW);
    setenv("TZ", cfg.tz_posix, 1);
    tzset();

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);

    // Release GPIO holds set by the previous goToSleep(). gpio_hold_en() kept
    // RST, CS, and DC stable through the boot ROM phase on timer wake. By the
    // time setup() runs that job is done — release them so the SPI peripheral
    // and display driver can drive those pins normally. Safe no-op on cold boot
    // (chip reset already cleared all holds).
    gpio_hold_dis((gpio_num_t)RES_PIN);
    gpio_hold_dis((gpio_num_t)CS_PIN);
    gpio_hold_dis((gpio_num_t)DC_PIN);

    // SPI bus init (needed for RTC-adjacent code paths; display init deferred)
    SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);
    // Display init: cold boot only here — timer wakes defer init to just before
    // drawing at :00 so the RST pulse doesn't cause a visible artifact at :58.
    if (coldBoot) {
        display.init(115200, true, 50, false);
        display.cp437(true);
    }

    // RTC init
    if (coldBoot) {
        i2cScan();
    }
    if (!rtc.begin()) {
        Serial.println("RTC not found on I2C.");
    } else {
        Serial.println("RTC detected.");
    }

    // Restore working globals from RTC_DATA_ATTR (if valid non-cold-boot wake)
    if (!coldBoot && _rtcValid) {
        hasWeather   = _hasWeather;
        weatherCode  = _weatherCode;
        weatherIsDay = _weatherIsDay;
        weatherTempF = _weatherTempF;
        weatherLowF  = _weatherLowF;
        weatherHighF = _weatherHighF;
        darkMode     = _darkMode;
        memcpy(cityName, _cityName, CITY_NAME_MAX);
    } else {
        // Cold boot: initialize darkMode from cfg and update RTC_DATA_ATTR
        darkMode  = cfg.darkMode;
        _darkMode = cfg.darkMode;
    }

    // Set FG/BG from darkMode
    FG = darkMode ? GxEPD_WHITE : GxEPD_BLACK;
    BG = darkMode ? GxEPD_BLACK : GxEPD_WHITE;

    // Handle button press — require 1-second hold to avoid accidental triggers
    bool buttonInitial = buttonWake || buttonEarlyRead || (digitalRead(BUTTON_PIN) == LOW);
    if (buttonInitial) {
        uint32_t holdStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW && (millis() - holdStart) < 1000) delay(10);
        bool buttonHeld = (millis() - holdStart) >= 1000;
        if (buttonHeld) {
            runSetupPortal();
            return;
        }
    }

    computeLayout();

    // Get current time
    tm nowL; time_t epochUtc;
    rtcNowLocal(nowL, epochUtc);

    bool doFullRefresh = coldBoot;

    if (coldBoot) {
        Serial.println("Cold boot - connecting WiFi for NTP + weather.");
        if (wifiConnect()) {
            syncRTCFromNTP_UTC();
            rtcNowLocal(nowL, epochUtc);  // re-read after NTP

            fetchCityName();
            memcpy(_cityName, cityName, CITY_NAME_MAX);

            if (fetchWeatherNow()) {
                _lastWxFetch = epochUtc;  // only advance on success
            }

            wifiDisconnect();
        } else {
            Serial.println("WiFi failed on cold boot - no weather or city name.");
        }

        _rtcValid     = true;
        doFullRefresh = true;

    } else if (timerWake) {
        // Woke at :58. Fetch weather before :00 if it's the hourly boundary.
        if (nowL.tm_min == (int)(FULL_REFRESH_EVERY_N_MIN - 1) && (epochUtc - _lastWxFetch >= 120)) {
            Serial.println("Hourly weather update.");
            if (wifiConnect()) {
                if (fetchWeatherNow()) {
                    _lastWxFetch = epochUtc;  // only advance on success
                }
                wifiDisconnect();
            }
            doFullRefresh = true;
        }

        // Full refresh at the top of each hour
        if ((nowL.tm_min + 1) % FULL_REFRESH_EVERY_N_MIN == 0) {
            doFullRefresh = true;
        }

        // Persist weather state now — before entering the loop — so it isn't
        // lost if the fetch ran past :00 and the loop exits without drawing.
        _hasWeather   = hasWeather;
        _weatherCode  = weatherCode;
        _weatherIsDay = weatherIsDay;
        _weatherTempF = weatherTempF;
        _weatherLowF  = weatherLowF;
        _weatherHighF = weatherHighF;
        _darkMode     = darkMode;

        // Active window: :58 through ~:05.
        // Wait for :00 to draw (clean minute boundary), or draw immediately if
        // the weather fetch already pushed us past :05. Go to sleep once the
        // draw is done and curSec is safely past the window.
        bool refreshDone = false;
        while (true) {
            if (digitalRead(BUTTON_PIN) == LOW) {
                uint32_t holdStart = millis();
                while (digitalRead(BUTTON_PIN) == LOW && (millis() - holdStart) < 1000) delay(10);
                if ((millis() - holdStart) >= 1000) {
                    runSetupPortal();
                    return;
                }
            }

            uint8_t curSec = rtc.now().second();

            // Refresh at :00–:05. If the weather fetch ran past :05 (WiFi + HTTPS
            // easily takes 10–20 s), draw immediately rather than skipping the
            // full refresh entirely. Skip only if curSec >= 55 (too close to the
            // next :58 wake — display init would cause a visible flash).
            if (!refreshDone && curSec < 55) {
                // Init display now (deferred from setup so :58 wake is silent)
                display.init(115200, false, 50, false);
                display.cp437(true);
                rtcNowLocal(nowL, epochUtc);
                if (doFullRefresh) {
                    Serial.println("Full refresh.");
                    drawFull(nowL);
                } else {
                    Serial.println("Partial time refresh.");
                    drawTimePartial(nowL);
                }
                refreshDone = true;
            }

            // Exit once past :05 and safely away from the next :58 wake
            if (curSec > 5 && curSec < 55) break;

            delay(50);
        }

        goToSleep();
        return;
    }

    // Save state to RTC memory
    _hasWeather   = hasWeather;
    _weatherCode  = weatherCode;
    _weatherIsDay = weatherIsDay;
    _weatherTempF = weatherTempF;
    _weatherLowF  = weatherLowF;
    _weatherHighF = weatherHighF;
    _darkMode     = darkMode;

    // Draw
    if (doFullRefresh) {
        Serial.println("Full refresh.");
        drawFull(nowL);
    } else {
        Serial.println("Partial time refresh.");
        drawTimePartial(nowL);
    }

    goToSleep();
}

void loop()
{
    // Should not normally be reached with deep sleep architecture
    goToSleep();
}
