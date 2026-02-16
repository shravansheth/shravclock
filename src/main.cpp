#define ENABLE_GxEPD2_GFX 0

#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

// ====== E-ink SPI pins ======
#define MOSI_PIN 18
#define SCK_PIN  19
#define CS_PIN   1
#define DC_PIN   2
#define RES_PIN  21
#define BUSY_PIN 16

// ====== RTC I2C pins ======
// DS3231: D=SDA, C=SCL
#define SDA_PIN 22
#define SCL_PIN 23

// ====== WiFi creds ======
#include "secrets.h"

// Pacific Time (US): PST/PDT with DST rules
#define TZ_PST "PST8PDT,M3.2.0/2,M11.1.0/2"

static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.nist.gov";

// 4.2" BW, SSD1683
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
display(GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

RTC_DS3231 rtc;

static uint32_t partial_count = 0;

// --- Layout for partial updates (time-only area) ---
struct Rect { uint16_t x,y,w,h; };
Rect timeRect;
Rect dateRect;
// fixed pixel placement for the TIME text
int16_t timeCursorX = 0;
int16_t timeCursorY = 0;

// 8-pixel align X/width for safer partial updates
static inline uint16_t align8(uint16_t v) { return (v / 8) * 8; }
static inline uint16_t ceil8(uint16_t v)  { return ((v + 7) / 8) * 8; }

String weekdayName(uint8_t dow) {
	// RTClib: Sunday=0 ... Saturday=6
	static const char* names[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
	return names[dow % 7];
}
String monthName(uint8_t m) {
	static const char* names[] = {"","January","February","March","April","May","June","July","August","September","October","November","December"};
	if (m < 1 || m > 12) return "Month?";
	return names[m];
}

void wifiConnect()
{
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	
	Serial.print("WiFi connecting");
	uint32_t start = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
		delay(250);
		Serial.print(".");
	}
	Serial.println();
	
	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("WiFi failed (continuing with RTC time if available).");
	} 
	else {
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
	if (WiFi.status() != WL_CONNECTED) return false;
	
	// Configure SNTP (UTC, no offset). TZ is handled later via localtime_r().
	configTime(0, 0, NTP1, NTP2);
	
	Serial.println("Waiting for NTP time (UTC epoch)...");
	for (int i = 0; i < 60; i++) {
		time_t now = time(nullptr);
		if (now > 1700000000){
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
	if (!fetchUtcEpochFromNTP(utc)) return;
	
	// Store UTC in DS3231
	rtc.adjust(DateTime((uint32_t)utc));
	Serial.println("RTC adjusted from NTP (stored as UTC).");
}

void computeRects()
{
	display.setRotation(0);
	
	// TIME metrics (use a fixed sample string)
	display.setFont(&FreeMonoBold24pt7b);
	const char* sampleTime = "23:59";
	int16_t tbx, tby; uint16_t tbw, tbh;
	display.getTextBounds(sampleTime, 0, 0, &tbx, &tby, &tbw, &tbh);
	
	uint16_t cx = display.width() / 2;
	uint16_t cy = display.height() / 2;
	
	// Choose a fixed baseline Y for time
	int16_t baselineY = cy - 10;
	
	// Canonical cursor position (baseline coords) used by BOTH full and partial
	timeCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
	timeCursorY = baselineY;
	
	// Partial window around the time text
	int16_t winX = timeCursorX + tbx - 10;
	int16_t winY = timeCursorY + tby - 10;
	uint16_t winW = tbw + 20;
	uint16_t winH = tbh + 20;
	
	if (winX < 0) winX = 0;
	if (winY < 0) winY = 0;
	
	// Align X/W to 8 pixels (recommended for BW partial updates)
	timeRect.x = align8((uint16_t)winX);
	timeRect.y = (uint16_t)winY;
	timeRect.w = ceil8(winW);
	timeRect.h = winH;
	
	// DATE metrics
	display.setFont(&FreeMonoBold12pt7b);
	const char* sampleDate = "Tuesday, February 15 2026";
	display.getTextBounds(sampleDate, 0, 0, &tbx, &tby, &tbw, &tbh);
	
	int16_t dateBaselineY = cy + 35;
	int16_t dateCursorX = ((int16_t)cx - (int16_t)(tbw / 2)) - tbx;
	int16_t dateCursorY = dateBaselineY;
	
	int16_t dateWinX = dateCursorX + tbx - 10;
	int16_t dateWinY = dateCursorY + tby - 10;
	uint16_t dateWinW = tbw + 20;
	uint16_t dateWinH = tbh + 20;
	
	if (dateWinX < 0) dateWinX = 0;
	if (dateWinY < 0) dateWinY = 0;
	
	dateRect.x = (uint16_t)dateWinX;
	dateRect.y = (uint16_t)dateWinY;
	dateRect.w = dateWinW;
	dateRect.h = dateWinH;
	
	Serial.printf("timeRect x=%u y=%u w=%u h=%u  timeCursor=(%d,%d)\n",
		timeRect.x, timeRect.y, timeRect.w, timeRect.h,
		timeCursorX, timeCursorY);
	}
	
	void drawFull(const tm& nowL)
	{
		char timeBuf[6];
		snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);
		
		String dateStr = weekdayName(nowL.tm_wday) + ", " +
		monthName(nowL.tm_mon + 1) + " " +
		String(nowL.tm_mday) + " " +
		String(nowL.tm_year + 1900);
		
		display.setFullWindow();
		display.firstPage();
		do {
			display.fillScreen(GxEPD_WHITE);
			
			// Time
			display.setFont(&FreeMonoBold24pt7b);
			display.setTextColor(GxEPD_BLACK);
			int16_t tbx, tby; uint16_t tbw, tbh;
			//display.getTextBounds(timeBuf, 0, 0, &tbx, &tby, &tbw, &tbh);
			uint16_t x = ((display.width() - tbw) / 2) - tbx;
			uint16_t y = (display.height() / 2) - 10;
			//display.setCursor(x, y);
			display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, GxEPD_WHITE);
			display.setCursor(timeCursorX, timeCursorY);
			display.print(timeBuf);
			
			// Date
			display.setFont(&FreeMonoBold12pt7b);
			display.getTextBounds(dateStr, 0, 0, &tbx, &tby, &tbw, &tbh);
			x = ((display.width() - tbw) / 2) - tbx;
			y = (display.height() / 2) + 35;
			display.setCursor(x, y);
			display.print(dateStr);
			
		} while (display.nextPage());
	}
	
	void drawTimePartial(const tm& nowL)
	{
		char buf[6];
		snprintf(buf, sizeof(buf), "%02d:%02d", nowL.tm_hour, nowL.tm_min);
		
		display.setRotation(0);
		display.setPartialWindow(timeRect.x, timeRect.y, timeRect.w, timeRect.h);
		
		display.firstPage();
		do {
			display.fillRect(timeRect.x, timeRect.y, timeRect.w, timeRect.h, GxEPD_WHITE);
			
			display.setFont(&FreeMonoBold24pt7b);
			display.setTextColor(GxEPD_BLACK);
			
			int16_t tbx, tby; uint16_t tbw, tbh;
			display.getTextBounds(buf, 0, 0, &tbx, &tby, &tbw, &tbh);
			
			// uint16_t x = timeRect.x + ((timeRect.w - tbw) / 2) - tbx;
			// uint16_t y = timeRect.y + ((timeRect.h + tbh) / 2) - 5;
			// display.setCursor(x, y);
			// display.print(buf);
			display.setCursor(timeCursorX, timeCursorY);
			display.print(buf);
			
		} while (display.nextPage());
	}
	
	
	
	void i2cScan() {
		Serial.println("Scanning I2C...");
		int found = 0;
		for (uint8_t addr = 1; addr < 127; addr++) {
			Wire.beginTransmission(addr);
			if (Wire.endTransmission() == 0) {
				Serial.printf("  Found 0x%02X\n", addr);
				found++;
			}
		}
		if (!found) Serial.println("  No I2C devices found");
	}
	
	void setup()
	{
		Serial.begin(115200);
		delay(200);
		Wire.begin(SDA_PIN, SCL_PIN);
		Wire.setClock(100000);
		i2cScan();
		
		// SPI initialize
		SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);
		
		display.init(115200, true, 50, false);
		
		// I2C for DS3231
		Wire.begin(SDA_PIN, SCL_PIN);
		
		if (!rtc.begin()) {
			Serial.println("RTC not found on I2C (check SDA/SCL wiring + pins).");
		} else {
			Serial.println("RTC detected.");
		}
		
		// Boot sequence: WiFi -> NTP -> set RTC -> draw full
		wifiConnect();
		//syncRTCFromNTP();
		syncRTCFromNTP_UTC();
		
		
		computeRects();
		tm nowL; time_t epoch;
		if (rtcNowLocal(nowL, epoch)) {
			drawFull(nowL);
		}
	}
	
	void loop()
	{
		static int last_second = -1;
		static int last_yday = -1;
		
		tm nowL; time_t epoch;
		if (!rtcNowLocal(nowL, epoch)) { delay(100); return; }
		
		if (nowL.tm_sec == last_second) { delay(50); return; }
		last_second = nowL.tm_sec;
		
		// Full refresh when local day changes (midnight / DST day rollover safe)
		if (last_yday == -1) last_yday = nowL.tm_yday;
		bool dayChanged = (nowL.tm_yday != last_yday);
		if (dayChanged) last_yday = nowL.tm_yday;
		
		if (nowL.tm_sec == 0) {
			if (dayChanged || (partial_count > 0 && (partial_count % 30 == 0))) {
				Serial.println("Full refresh.");
				drawFull(nowL);
			}
			else {
				Serial.println("Partial refresh (time).");
				drawTimePartial(nowL);
			}
			partial_count++;
		}
		
		delay(50);
	}