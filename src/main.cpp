#define ENABLE_GxEPD2_GFX 0

#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

//#include "WeatherStationImages.h"

// ====== E-ink SPI pins ======
#define MOSI_PIN 18
#define SCK_PIN 19
#define CS_PIN 1
#define DC_PIN 2
#define RES_PIN 21
#define BUSY_PIN 16

// ====== RTC I2C pins ======
// DS3231: D=SDA, C=SCL
#define SDA_PIN 22
#define SCL_PIN 23

// ====== WiFi creds ======
#include "secrets.h"

// Pacific Time (US): PST/PDT with DST rules
#define TZ_PST "PST8PDT,M3.2.0/2,M11.1.0/2"

static const char *NTP1 = "pool.ntp.org";
static const char *NTP2 = "time.nist.gov";

static const float LAT = 35.2828f;
static const float LON = -120.6596f;

// Refresh policy
static const uint32_t FULL_REFRESH_EVERY_N_MIN = 30;  // full refresh every 30 minutes
static const uint32_t WEATHER_FETCH_EVERY_N_HR = 2; // weather refresh

// 4.2" BW, SSD1683
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
display(GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

RTC_DS3231 rtc;

// --- Layout for partial updates (time-only area) ---
struct Rect
{
    uint16_t x, y, w, h;
};
// 8-pixel align X/width for safer partial updates
static inline uint16_t align8(uint16_t v) { return (v / 8) * 8; }
static inline uint16_t ceil8(uint16_t v) { return ((v + 7) / 8) * 8; }

Rect timeRect;
Rect weatherRect;

int16_t timeCursorX = 0, timeCursorY = 0;
int16_t dateCursorX = 0, dateCursorY = 0;
int16_t weatherCursorX = 0, weatherCursorY = 0;

static uint32_t partial_count = 0;

// Weather state
static bool hasWeather = false;
static int weatherCode = 0; // WMO weather_code
static float weatherTempF = 0.0f;
static time_t lastWeatherFetchUtc = 0;

// ---------- tiny 1-bit icons (24x24) ----------
static const uint8_t ICON_SUN_24[] PROGMEM = {
    0x00,0x00,0x00, 0x01,0x80,0x00, 0x01,0x80,0x00, 0x00,0x00,0x00,
    0x18,0x06,0x00, 0x0F,0xF8,0x00, 0x3F,0xFE,0x00, 0x3C,0x3E,0x00,
    0x7C,0x3F,0x00, 0x7C,0x3F,0x00, 0x3C,0x3E,0x00, 0x3F,0xFE,0x00,
    0x0F,0xF8,0x00, 0x18,0x06,0x00, 0x00,0x00,0x00, 0x01,0x80,0x00,
    0x01,0x80,0x00, 0x00,0x00,0x00,
};
static const uint8_t ICON_CLOUD_24[] PROGMEM = {
    0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x03,0xE0,0x00,
    0x0F,0xF8,0x00, 0x1C,0x1C,0x00, 0x18,0x0C,0x00, 0x38,0x0E,0x00,
    0x70,0x07,0x00, 0x7F,0xFF,0x00, 0xFF,0xFF,0x80, 0xFF,0xFF,0x80,
    0xFF,0xFF,0x80, 0x7F,0xFF,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
    0x00,0x00,0x00, 0x00,0x00,0x00,
};
static const uint8_t ICON_RAIN_24[] PROGMEM = {
    0x00,0x00,0x00, 0x03,0xE0,0x00, 0x0F,0xF8,0x00, 0x1C,0x1C,0x00,
    0x38,0x0E,0x00, 0x70,0x07,0x00, 0x7F,0xFF,0x00, 0xFF,0xFF,0x80,
    0xFF,0xFF,0x80, 0x7F,0xFF,0x00, 0x00,0x00,0x00, 0x04,0x10,0x00,
    0x02,0x08,0x00, 0x04,0x10,0x00, 0x02,0x08,0x00, 0x04,0x10,0x00,
    0x00,0x00,0x00, 0x00,0x00,0x00,
};
static const uint8_t ICON_SNOW_24[] PROGMEM = {
    0x00,0x00,0x00, 0x03,0xE0,0x00, 0x0F,0xF8,0x00, 0x1C,0x1C,0x00,
    0x38,0x0E,0x00, 0x70,0x07,0x00, 0x7F,0xFF,0x00, 0xFF,0xFF,0x80,
    0xFF,0xFF,0x80, 0x7F,0xFF,0x00, 0x00,0x00,0x00, 0x02,0x08,0x00,
    0x05,0x14,0x00, 0x02,0x08,0x00, 0x05,0x14,0x00, 0x02,0x08,0x00,
    0x00,0x00,0x00, 0x00,0x00,0x00,
};
static const uint8_t ICON_STORM_24[] PROGMEM = {
    0x00,0x00,0x00, 0x03,0xE0,0x00, 0x0F,0xF8,0x00, 0x1C,0x1C,0x00,
    0x38,0x0E,0x00, 0x70,0x07,0x00, 0x7F,0xFF,0x00, 0xFF,0xFF,0x80,
    0xFF,0xFF,0x80, 0x7F,0xFF,0x00, 0x01,0x80,0x00, 0x03,0x00,0x00,
    0x06,0x00,0x00, 0x0C,0x00,0x00, 0x18,0x00,0x00, 0x10,0x00,0x00,
    0x00,0x00,0x00, 0x00,0x00,0x00,
};

String weekdayName(uint8_t dow)
{
    // RTClib: Sunday=0 ... Saturday=6
    static const char *names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return names[dow % 7];
}
String monthName(uint8_t m)
{
    static const char *names[] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    if (m < 1 || m > 12)
    return "Month?";
    return names[m];
}

void wifiConnect()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    Serial.print("WiFi connecting");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000)
    {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi failed (continuing with RTC time if available).");
    }
    else
    {
        Serial.print("WiFi OK, IP: ");
        Serial.println(WiFi.localIP());
    }
}

bool rtcNowLocal(struct tm &local_tm, time_t &epochUtc)
{
    DateTime utc = rtc.now();
    epochUtc = (time_t)utc.unixtime();
    return localtime_r(&epochUtc, &local_tm);
}

bool fetchUtcEpochFromNTP(time_t &outUtc)
{
    if (WiFi.status() != WL_CONNECTED)
    return false;
    
    // Configure SNTP (UTC, no offset). TZ is handled later via localtime_r().
    configTime(0, 0, NTP1, NTP2);
    
    Serial.println("Waiting for NTP time (UTC epoch)...");
    for (int i = 0; i < 60; i++)
    {
        time_t now = time(nullptr);
        if (now > 1700000000)
        {
            outUtc = now;
            // Set Time-Zone env
            setenv("TZ", TZ_PST, 1);
            tzset();
            Serial.println("NTP time acquired.");
            return true;
        }
        delay(250);
    }
    
    Serial.println("NTP timeout.");
    return false;
}

void syncRTCFromNTP_UTC()
{
    time_t utc;
    if (!fetchUtcEpochFromNTP(utc))
    return;
    
    // Store UTC in DS3231
    rtc.adjust(DateTime((uint32_t)utc));
    Serial.println("RTC adjusted from NTP (stored as UTC).");
}

const uint8_t *pickWeatherIcon24(int code)
{
    // Based on Open-Meteo WMO code table (doc lists the mappings).  [oai_citation:2‡Open Meteo](https://open-meteo.com/en/docs)
    if (code == 0)
    return ICON_SUN_24;
    if (code == 1 || code == 2 || code == 3)
    return ICON_CLOUD_24;
    if (code == 45 || code == 48)
    return ICON_CLOUD_24;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
    return ICON_RAIN_24;
    if ((code >= 71 && code <= 77) || (code == 85 || code == 86))
    return ICON_SNOW_24;
    if (code == 95 || code == 96 || code == 99)
    return ICON_STORM_24;
    return ICON_CLOUD_24;
}

bool fetchWeatherNow()
{
    if (WiFi.status() != WL_CONNECTED)
    return false;
    
    // Open-Meteo: current=temperature_2m,weather_code and Fahrenheit unit.  [oai_citation:3‡Open Meteo](https://open-meteo.com/en/docs)
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(LAT, 4) +
    "&longitude=" + String(LON, 4) +
    "&current=temperature_2m,weather_code" +
    "&temperature_unit=fahrenheit" +
    "&timezone=auto";
    
    WiFiClientSecure client;
    client.setInsecure();
    
    HTTPClient http;
    if (!http.begin(client, url))
    {
        Serial.println("Weather: http.begin failed");
        return false;
    }
    
    int code = http.GET();
    if (code != 200)
    {
        Serial.printf("Weather: HTTP %d\n", code);
        http.end();
        return false;
    }
    
    String body = http.getString();
    http.end();
    
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        Serial.printf("Weather: JSON parse error: %s\n", err.c_str());
        return false;
    }
    
    // { "current": { "temperature_2m": ..., "weather_code": ... } }
    weatherTempF = doc["current"]["temperature_2m"].as<float>();
    weatherCode = doc["current"]["weather_code"].as<int>();
    hasWeather = true;
    
    Serial.printf("Weather OK: %.1fF code=%d\n", weatherTempF, weatherCode);
    return true;
}

// void computeRects()
// {
//     display.setRotation(0);

//     // TIME metrics (use a fixed sample string)
//     display.setFont(&FreeMonoBold24pt7b);
//     const char *sampleTime = "23:59";
//     int16_t tbx, tby;
//     uint16_t tbw, tbh;
//     display.getTextBounds(sampleTime, 0, 0, &tbx, &tby, &tbw, &tbh);

//     uint16_t cx = display.width() / 2;
//     uint16_t cy = display.height() / 2;

//     // Choose a fixed baseline Y for time
//     int16_t baselineY = cy - 10;

//     // Canonical cursor position (baseline coords) used by BOTH full and partial
//     timeCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
//     timeCursorY = baselineY;

//     // Partial window around the time text
//     int16_t winX = timeCursorX + tbx - 10;
//     int16_t winY = timeCursorY + tby - 10;
//     uint16_t winW = tbw + 20;
//     uint16_t winH = tbh + 20;

//     if (winX < 0)
//         winX = 0;
//     if (winY < 0)
//         winY = 0;

//     // Align X/W to 8 pixels (recommended for BW partial updates)
//     timeRect.x = align8((uint16_t)winX);
//     timeRect.y = (uint16_t)winY;
//     timeRect.w = ceil8(winW);
//     timeRect.h = winH;

//     // DATE metrics
//     display.setFont(&FreeMonoBold12pt7b);
//     const char *sampleDate = "Tuesday, February 15 2026";
//     display.getTextBounds(sampleDate, 0, 0, &tbx, &tby, &tbw, &tbh);

//     int16_t dateBaselineY = cy + 35;
//     int16_t dateCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
//     int16_t dateCursorY = dateBaselineY;

//     int16_t dateWinX = dateCursorX + tbx - 10;
//     int16_t dateWinY = dateCursorY + tby - 10;
//     uint16_t dateWinW = tbw + 20;
//     uint16_t dateWinH = tbh + 20;

//     if (dateWinX < 0)
//         dateWinX = 0;
//     if (dateWinY < 0)
//         dateWinY = 0;

//     dateRect.x = (uint16_t)dateWinX;
//     dateRect.y = (uint16_t)dateWinY;
//     dateRect.w = dateWinW;
//     dateRect.h = dateWinH;

//     Serial.printf("timeRect x=%u y=%u w=%u h=%u  timeCursor=(%d,%d)\n",
//                   timeRect.x, timeRect.y, timeRect.w, timeRect.h,
//                   timeCursorX, timeCursorY);
// }

void computeLayout()
{
    display.setRotation(0);

    // We anchor everything near the top, still horizontally centered.
    const int16_t topY = 70; // move block toward top (tweak this)
    const int16_t gapDate = 38;
    const int16_t gapWeather = 100;

    uint16_t cx = display.width() / 2;

    // ---- TIME ----
    display.setFont(&FreeMonoBold24pt7b);
    const char *sampleTime = "23:59";
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(sampleTime, 0, 0, &tbx, &tby, &tbw, &tbh);

    timeCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
    timeCursorY = topY;

    int16_t winX = timeCursorX + tbx - 10;
    int16_t winY = timeCursorY + tby - 10;
    uint16_t winW = tbw + 20;
    uint16_t winH = tbh + 20;

    if (winX < 0)
        winX = 0;
    if (winY < 0)
        winY = 0;

    timeRect.x = align8((uint16_t)winX);
    timeRect.y = (uint16_t)winY;
    timeRect.w = ceil8(winW);
    timeRect.h = winH;

    // ---- DATE ----
    display.setFont(&FreeMonoBold12pt7b);
    const char *sampleDate = "Tuesday, February 15 2026";
    display.getTextBounds(sampleDate, 0, 0, &tbx, &tby, &tbw, &tbh);

    dateCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
    dateCursorY = topY + gapDate;

    // ---- WEATHER ROW (icon + temp, same font size as time) ----
    display.setFont(&FreeMonoBold24pt7b);
    const char *sampleWx = "72°F"; // sample for width
    display.getTextBounds(sampleWx, 0, 0, &tbx, &tby, &tbw, &tbh);

    // icon is 24px wide, add 10px gap
    uint16_t totalW = 24 + 10 + tbw;
    weatherCursorX = (int16_t)cx - (int16_t)(totalW / 2);
    weatherCursorY = topY + gapWeather;

    // partial window for weather row (covers icon + temp)
    int16_t wxWinX = weatherCursorX - 10;
    int16_t wxWinY = weatherCursorY + tby - 10;
    uint16_t wxWinW = totalW + 20;
    uint16_t wxWinH = tbh + 20;

    if (wxWinX < 0)
        wxWinX = 0;
    if (wxWinY < 0)
        wxWinY = 0;

    weatherRect.x = align8((uint16_t)wxWinX);
    weatherRect.y = (uint16_t)wxWinY;
    weatherRect.w = ceil8(wxWinW);
    weatherRect.h = wxWinH;

    Serial.printf("timeRect  x=%u y=%u w=%u h=%u cursor=(%d,%d)\n",
                  timeRect.x, timeRect.y, timeRect.w, timeRect.h, timeCursorX, timeCursorY);
    Serial.printf("weatherRect x=%u y=%u w=%u h=%u cursor=(%d,%d)\n",
                  weatherRect.x, weatherRect.y, weatherRect.w, weatherRect.h, weatherCursorX, weatherCursorY);
}

void drawDateLine(const tm &nowL)
{
    String dateStr = weekdayName(nowL.tm_wday) + ", " +
                     monthName(nowL.tm_mon + 1) + " " +
                     String(nowL.tm_mday) + " " +
                     String(nowL.tm_year + 1900);

    display.setFont(&FreeMonoBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(dateCursorX, dateCursorY);
    display.print(dateStr);
}

void drawWeatherRow()
{
    display.setFont(&FreeMonoBold24pt7b);
    display.setTextColor(GxEPD_BLACK);

    // icon
    const uint8_t *icon = pickWeatherIcon24(weatherCode);
    display.drawBitmap(weatherCursorX, weatherCursorY - 24, icon, 24, 24, GxEPD_BLACK);

    // temp string
    // char tbuf[9];
    // if (hasWeather)
    // {
    //     snprintf(tbuf, sizeof(tbuf), "%dF", (int)lroundf(weatherTempF));
    // }
    // else
    // {
    //     snprintf(tbuf, sizeof(tbuf), "--F");
    // }

    // // print to right of icon
    // display.setCursor(weatherCursorX + 24 + 10, weatherCursorY);
    // display.print(tbuf);
    display.setCursor(weatherCursorX + 24 + 10, weatherCursorY);

    if (hasWeather)
    {
        display.print((int)lroundf(weatherTempF));
    }
    else
    {
        display.print("--");
    }

    //display.write((uint8_t)248); // ° in CP437
    //display.print((char)247);
    //display.print("F");
    int16_t x = display.getCursorX();
    int16_t y = display.getCursorY();
    display.drawCircle(x + 4, y - 26, 3, GxEPD_BLACK); // Adjust coordinates and radius as needed
    display.setCursor(x + 12, y); // Manually advance cursor past the symbol
    display.print("F");
}

// void drawFull(const tm &nowL)
// {
//     char timeBuf[6];
//     snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

//     String dateStr = weekdayName(nowL.tm_wday) + ", " +
//                      monthName(nowL.tm_mon + 1) + " " +
//                      String(nowL.tm_mday) + " " +
//                      String(nowL.tm_year + 1900);

//     display.setFullWindow();
//     display.firstPage();
//     do
//     {
//         display.fillScreen(GxEPD_WHITE);

//         // Time
//         display.setFont(&FreeMonoBold24pt7b);
//         display.setTextColor(GxEPD_BLACK);
//         int16_t tbx, tby;
//         uint16_t tbw, tbh;
//         // display.getTextBounds(timeBuf, 0, 0, &tbx, &tby, &tbw, &tbh);
//         uint16_t x = ((display.width() - tbw) / 2) - tbx;
//         uint16_t y = (display.height() / 2) - 10;
//         // display.setCursor(x, y);
//         display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, GxEPD_WHITE);
//         display.setCursor(timeCursorX, timeCursorY);
//         display.print(timeBuf);

//         // Date
//         display.setFont(&FreeMonoBold12pt7b);
//         display.getTextBounds(dateStr, 0, 0, &tbx, &tby, &tbw, &tbh);
//         x = ((display.width() - tbw) / 2) - tbx;
//         y = (display.height() / 2) + 35;
//         display.setCursor(x, y);
//         display.print(dateStr);

//     } while (display.nextPage());
// }

void drawFull(const tm &nowL)
{
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);

        // Time
        display.setFont(&FreeMonoBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(timeCursorX, timeCursorY);
        display.print(timeBuf);

        // Date
        drawDateLine(nowL);

        // Weather row
        drawWeatherRow();

    } while (display.nextPage());
}

// void drawTimePartial(const tm &nowL)
// {
//     char buf[6];
//     snprintf(buf, sizeof(buf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

//     display.setRotation(0);
//     display.setPartialWindow(timeRect.x, timeRect.y, timeRect.w, timeRect.h);

//     display.firstPage();
//     do
//     {
//         display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, GxEPD_WHITE);

//         display.setFont(&FreeMonoBold24pt7b);
//         display.setTextColor(GxEPD_BLACK);

//         int16_t tbx, tby;
//         uint16_t tbw, tbh;
//         display.getTextBounds(buf, 0, 0, &tbx, &tby, &tbw, &tbh);

//         // uint16_t x = timeRect.x + ((timeRect.w - tbw) / 2) - tbx;
//         // uint16_t y = timeRect.y + ((timeRect.h + tbh) / 2) - 5;
//         // display.setCursor(x, y);
//         // display.print(buf);
//         display.setCursor(timeCursorX, timeCursorY);
//         display.print(buf);

//     } while (display.nextPage());
// }

void drawTimePartial(const tm &nowL)
{
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);

    display.setRotation(0);
    display.setPartialWindow(timeRect.x, timeRect.y, timeRect.w, timeRect.h);

    display.firstPage();
    do
    {
        display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, GxEPD_WHITE);
        display.setFont(&FreeMonoBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(timeCursorX, timeCursorY);
        display.print(buf);
    } while (display.nextPage());
}

void drawWeatherPartial()
{
    display.setRotation(0);
    display.setPartialWindow(weatherRect.x, weatherRect.y, weatherRect.w, weatherRect.h);

    display.firstPage();
    do
    {
        display.fillRect(weatherRect.x, weatherRect.y, weatherRect.w, weatherRect.h, GxEPD_WHITE);
        drawWeatherRow();
    } while (display.nextPage());
}

void i2cScan()
{
    Serial.println("Scanning I2C...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            Serial.printf("  Found 0x%02X\n", addr);
            found++;
        }
    }
    if (!found)
        Serial.println("  No I2C devices found");
}

// void setup()
// {
//     Serial.begin(115200);
//     delay(200);
//     Wire.begin(SDA_PIN, SCL_PIN);
//     Wire.setClock(100000);
//     i2cScan();

//     // SPI initialize
//     SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);

//     display.init(115200, true, 50, false);

//     // I2C for DS3231
//     Wire.begin(SDA_PIN, SCL_PIN);

//     if (!rtc.begin())
//     {
//         Serial.println("RTC not found on I2C (check SDA/SCL wiring + pins).");
//     }
//     else
//     {
//         Serial.println("RTC detected.");
//     }

//     // Boot sequence: WiFi -> NTP -> set RTC -> draw full
//     wifiConnect();
//     // syncRTCFromNTP();
//     syncRTCFromNTP_UTC();

//     computeRects();
//     tm nowL;
//     time_t epoch;
//     if (rtcNowLocal(nowL, epoch))
//     {
//         drawFull(nowL);
//     }
// }

// void loop()
// {
//     static int last_second = -1;
//     static int last_yday = -1;

//     tm nowL;
//     time_t epoch;
//     if (!rtcNowLocal(nowL, epoch))
//     {
//         delay(100);
//         return;
//     }

//     if (nowL.tm_sec == last_second)
//     {
//         delay(50);
//         return;
//     }
//     last_second = nowL.tm_sec;

//     // Full refresh when local day changes (midnight / DST day rollover safe)
//     if (last_yday == -1)
//         last_yday = nowL.tm_yday;
//     bool dayChanged = (nowL.tm_yday != last_yday);
//     if (dayChanged)
//         last_yday = nowL.tm_yday;
//     if (nowL.tm_sec == 0)
//     {
//         // if (dayChanged || (partial_count > 0 && (partial_count % 30 == 0))) {
//         // 	Serial.println("Full refresh.");
//         // 	drawFull(nowL);
//         // }
//         if (dayChanged || (nowL.tm_min % 30 == 0))
//         {
//             Serial.println("Full refresh.");
//             drawFull(nowL);
//         }
//         else
//         {
//             Serial.println("Partial refresh (time).");
//             drawTimePartial(nowL);
//         }
//         partial_count++;
//     }

//     delay(50);
// }
void setup()
{
    Serial.begin(115200);
    delay(200);

    // TZ must be set before localtime_r()
    setenv("TZ", TZ_PST, 1);
    tzset();

    // I2C
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);

    // SPI
    SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);

    display.init(115200, true, 50, false);
    display.cp437(true);

    if (!rtc.begin())
    {
        Serial.println("RTC not found on I2C (check SDA/SCL wiring + pins).");
    }
    else
    {
        Serial.println("RTC detected.");
    }

    wifiConnect();
    syncRTCFromNTP_UTC();

    // Initial weather fetch (best effort)
    if (fetchWeatherNow())
    {
        time_t utcNow = rtc.now().unixtime();
        lastWeatherFetchUtc = utcNow;
    }

    computeLayout();

    tm nowL;
    time_t epoch;
    if (rtcNowLocal(nowL, epoch))
    {
        drawFull(nowL);
    }
}

void loop()
{
    static int last_second = -1;
    static int last_yday = -1;

    tm nowL;
    time_t epochUtc;
    if (!rtcNowLocal(nowL, epochUtc))
    {
        delay(100);
        return;
    }

    if (nowL.tm_sec == last_second)
    {
        delay(50);
        return;
    }
    last_second = nowL.tm_sec;

    // Detect day change (midnight / DST rollover safe)
    if (last_yday == -1)
        last_yday = nowL.tm_yday;
    bool dayChanged = (nowL.tm_yday != last_yday);
    if (dayChanged)
        last_yday = nowL.tm_yday;

    // Weather fetch cadence: every WEATHER_FETCH_EVERY_N_MIN at :00
    // if (nowL.tm_sec == 0 && (nowL.tm_hour % WEATHER_FETCH_EVERY_N_HR == 0))
    // {
    //     // avoid re-fetching if loop jitters within the same minute
    //     if (epochUtc - lastWeatherFetchUtc >= (time_t)(WEATHER_FETCH_EVERY_N_HR * 60 - 5))
    //     {
    //         if (fetchWeatherNow())
    //         {
    //             lastWeatherFetchUtc = epochUtc;
    //             // Update weather row immediately (partial)
    //             drawWeatherPartial();
    //         }
    //     }
    // }
    if (nowL.tm_min == 0 && nowL.tm_sec == 0 &&
        (nowL.tm_hour % WEATHER_FETCH_EVERY_N_HR == 0))
    {
        // Avoid re-fetching if loop jitters within the same minute
        const time_t FETCH_INTERVAL_SEC =
            (time_t)WEATHER_FETCH_EVERY_N_HR * 3600;

        if (epochUtc - lastWeatherFetchUtc >= FETCH_INTERVAL_SEC - 10)
        {
            if (fetchWeatherNow())
            {
                lastWeatherFetchUtc = epochUtc;

                // Update weather row immediately (partial)
                drawWeatherPartial();
            }
        }
    }

    // Screen update on the 0th second of each minute
    if (nowL.tm_sec == 0)
    {
        bool doFull = dayChanged || (nowL.tm_min % FULL_REFRESH_EVERY_N_MIN == 0);

        if (doFull)
        {
            Serial.println("Full refresh.");
            drawFull(nowL);
            partial_count = 0;
        }
        else
        {
            Serial.println("Partial refresh (time).");
            drawTimePartial(nowL);
            partial_count++;
        }
    }

    delay(50);
}