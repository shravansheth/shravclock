// ============================================================
// Battery measuring backup — add back to main.cpp once the
// 200k/200k resistor divider is wired to BATT_ADC_PIN (GPIO4).
//
// Wiring reminder:
//   BAT+ ──[200k]──┬── GPIO4 (D2 on XIAO ESP32C6)
//                  [200k]
//                  │
//                 GND
// ============================================================


// ====== Defines to restore in main.cpp ======

#define BATT_DEAD_THRESH   3.0f                       // volts — below this = show dead screen
#define BATT_DEAD_SLEEP_US (5ULL * 60 * 1000000)      // sleep 5 min when dead


// ====== RTC_DATA_ATTR globals to restore ======

RTC_DATA_ATTR static int  _battPct    = -1;
RTC_DATA_ATTR static bool _isCharging = false;


// ====== Working globals to restore ======

static int  battPct    = -1;
static bool isCharging = false;


// ====== Li-ion discharge curve + readBattery() ======

static int battVoltToPct(float v) {
    static const float V[] = {4.20f,4.10f,4.00f,3.90f,3.80f,3.70f,3.60f,3.50f,3.40f,3.30f,3.00f};
    static const float P[] = {100,  90,   80,   70,   60,   50,   40,   30,   20,   10,   0   };
    const int n = 11;
    if (v >= V[0])   return 100;
    if (v <= V[n-1]) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (v <= V[i] && v > V[i+1]) {
            float t = (v - V[i+1]) / (V[i] - V[i+1]);
            return (int)lroundf(P[i+1] + t * (P[i] - P[i+1]));
        }
    }
    return 0;
}

static void readBattery() {
    uint32_t mV = 0;
    for (int i = 0; i < 8; i++) { mV += analogReadMilliVolts(BATT_ADC_PIN); delay(2); }
    mV /= 8;
    float battV = (float)mV * BATT_DIVIDER / 1000.0f;
    isCharging  = (battV >= BATT_CHG_THRESH);
    battPct     = battVoltToPct(battV);
    Serial.printf("Battery: %.2fV → %d%% charging=%d\n", battV, battPct, (int)isCharging);
}


// ====== drawBatteryIndicator() — replaces the stub in main.cpp ======
// When charging: lightning bolt. When on battery: fill level + percentage text.

static void drawBatteryIndicator()
{
    const int16_t bx = 366, by = 4;
    display.drawRect(bx, by, 24, 14, FG);
    display.fillRect(bx + 24, by + 4, 4, 6, FG);

    if (isCharging) {
        // Lightning bolt
        display.drawLine(bx + 14, by + 1,  bx + 9,  by + 7,  FG);
        display.drawLine(bx + 9,  by + 7,  bx + 15, by + 7,  FG);
        display.drawLine(bx + 15, by + 7,  bx + 10, by + 12, FG);
    } else {
        // Fill body proportionally (interior: 22×12 px starting at bx+1, by+1)
        int fillW = (battPct >= 0) ? (int)((float)battPct / 100.0f * 22.0f + 0.5f) : 0;
        if (fillW > 0) display.fillRect(bx + 1, by + 1, fillW, 12, FG);

        // Percentage text in built-in 5×7 font, to the left of battery
        char pctBuf[5];
        snprintf(pctBuf, sizeof(pctBuf), (battPct < 0) ? "--%" : "%d%%", battPct);
        display.setFont(nullptr);
        display.setTextSize(1);
        display.setTextColor(FG);
        int16_t tx, ty; uint16_t tw, th;
        display.getTextBounds(pctBuf, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor(bx - 4 - (int16_t)tw - tx, by + (14 - (int16_t)th) / 2 - ty);
        display.print(pctBuf);
    }
}


// ====== drawBatteryPartial() — partial refresh of battery area only ======
// battRect should be declared at file scope:
//   Rect battRect = { 328, 0, 72, 20 };

static void drawBatteryPartial()
{
    display.setPartialWindow(battRect.x, battRect.y, battRect.w, battRect.h);
    display.firstPage();
    do {
        display.fillRect(battRect.x, battRect.y, battRect.w, battRect.h, BG);
        drawBatteryIndicator();
    } while (display.nextPage());
}


// ====== drawBatteryDeadScreen() ======
// Show on white background regardless of dark mode.

static void drawBatteryDeadScreen()
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Battery icon top-right (empty = dead)
        const int16_t bx = 366, by = 4;
        display.drawRect(bx, by, 24, 14, GxEPD_BLACK);
        display.fillRect(bx + 24, by + 4, 4, 6, GxEPD_BLACK);

        // "Battery Dead."
        display.setFont(&FreeSansBold18pt7b);
        display.setTextSize(1);
        display.setTextColor(GxEPD_BLACK);
        {
            int16_t tx, ty; uint16_t tw, th;
            const char *msg1 = "Battery Dead.";
            display.getTextBounds(msg1, 0, 0, &tx, &ty, &tw, &th);
            display.setCursor((int16_t)(display.width() / 2) - (int16_t)(tw / 2) - tx, 120);
            display.print(msg1);
        }

        // "Please Charge."
        {
            int16_t tx, ty; uint16_t tw, th;
            const char *msg2 = "Please Charge.";
            display.getTextBounds(msg2, 0, 0, &tx, &ty, &tw, &th);
            display.setCursor((int16_t)(display.width() / 2) - (int16_t)(tw / 2) - tx, 165);
            display.print(msg2);
        }

    } while (display.nextPage());
}


// ====== Dead battery check — paste at top of setup(), before SPI/display init ======
// Also restore: battPct=0 and isCharging=false restore lines in the RTC memory
// restore block, and readBattery() + _battPct/_isCharging save lines in cold boot
// and hourly refresh blocks.

    {
        uint32_t rawMv = 0;
        for (int i = 0; i < 8; i++) { rawMv += analogReadMilliVolts(BATT_ADC_PIN); delay(2); }
        float rawV      = (float)(rawMv / 8) * BATT_DIVIDER / 1000.0f;
        bool rawCharging = (rawV >= BATT_CHG_THRESH);
        Serial.printf("Pre-init battery: %.2fV charging=%d\n", rawV, (int)rawCharging);

        if (rawV < BATT_DEAD_THRESH && !rawCharging) {
            Serial.println("Battery dead — showing dead screen.");
            SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);
            display.init(115200, true, 50, false);
            display.cp437(true);
            battPct    = 0;
            isCharging = false;
            drawBatteryDeadScreen();
            display.hibernate();
            esp_sleep_enable_timer_wakeup(BATT_DEAD_SLEEP_US);
            esp_deep_sleep_start();
        }
    }


// ====== Lines to restore in the RTC memory restore block (non-cold-boot) ======

        battPct    = _battPct;
        isCharging = _isCharging;


// ====== Lines to restore in cold boot block (after wifiDisconnect) ======

        readBattery();
        _battPct    = battPct;
        _isCharging = isCharging;


// ====== Lines to restore in hourly refresh block (after doFullRefresh check) ======

        if (doFullRefresh) {
            readBattery();
            _battPct    = battPct;
            _isCharging = isCharging;
        }


// ====== Loop: battery partial refresh on charging state change ======
// This was removed with deep sleep. Instead, on each hourly full refresh,
// readBattery() is called before drawFull(), so the indicator updates automatically.
// drawBatteryPartial() can be called manually if you want a mid-cycle update.
