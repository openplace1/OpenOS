#include "Clock.h"
#include "Theme.h"
#include <WiFi.h>
#include <time.h>

extern bool sysNtpSynced;
extern time_t sysLastNtpSync;

static const char* NTP_TZ     = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char* NTP_SERVER = "pool.ntp.org";

static const char* dayNames[]   = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char* monthNames[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

ClockApp::ClockApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts  = tsInstance;
    name      = "Clock";
    iconColor = tft->color565(88, 86, 214);
    isApp     = true;
    wasTouched   = false;
    lastDrawnMin = -1;
    lastDrawnSec = -1;
}

void ClockApp::show() {
    tft->fillScreen(Theme::bg());
    drawFace();
    drawTime(true);
    drawDate();
    drawNTPSection();
}

void ClockApp::drawHeader() {
    tft->fillRect(0, 0, 240, 52, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM);
    tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString("Clock", 120, 25);
}

void ClockApp::drawFace() {
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM);
    tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString("Clock", 120, 25);
}

void ClockApp::drawTime(bool fullRedraw) {
    time_t now; time(&now);
    struct tm t; localtime_r(&now, &t);

    if (!fullRedraw && t.tm_min == lastDrawnMin && t.tm_sec == lastDrawnSec) return;

    // Clear time area
    tft->fillRect(0, 60, 240, 110, Theme::bg());

    // HH:MM  — font 7 (48-px 7-seg digits), centered
    char hm[6]; sprintf(hm, "%02d:%02d", t.tm_hour, t.tm_min);
    tft->setTextFont(7); tft->setTextSize(1);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString(hm, 120, 100);

    // Seconds — smaller, below
    char ss[4]; sprintf(ss, ":%02d", t.tm_sec);
    tft->setTextFont(4); tft->setTextSize(1);
    tft->setTextColor(Theme::subtext()); tft->setTextDatum(MC_DATUM);
    tft->drawString(ss, 120, 148);

    lastDrawnMin = t.tm_min;
    lastDrawnSec = t.tm_sec;
}

void ClockApp::drawDate() {
    tft->fillRect(0, 168, 240, 22, Theme::bg());

    time_t now; time(&now);
    struct tm t; localtime_r(&now, &t);

    char buf[32];
    sprintf(buf, "%s, %d %s %04d",
            dayNames[t.tm_wday], t.tm_mday,
            monthNames[t.tm_mon], t.tm_year + 1900);

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
    tft->drawString(buf, 120, 178);
}

void ClockApp::drawNTPSection() {
    // Card
    tft->fillRoundRect(15, 200, 210, 75, 10, Theme::surface());
    tft->drawRoundRect(15, 200, 210, 75, 10, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1);

    // Status label
    tft->setTextColor(Theme::subtext()); tft->setTextDatum(ML_DATUM);
    tft->drawString("NTP Sync", 28, 218);

    if (sysNtpSynced) {
        struct tm t; localtime_r(&sysLastNtpSync, &t);
        char buf[24]; sprintf(buf, "Last: %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        tft->setTextColor(tft->color565(52, 199, 89)); tft->setTextDatum(MR_DATUM);
        tft->drawString(buf, 222, 218);
    } else {
        tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
        tft->drawString("Never synced", 222, 218);
    }

    tft->drawFastHLine(15, 235, 210, Theme::divider());

    // Sync button
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    uint16_t btnColor = wifiOk ? tft->color565(0, 122, 255) : Theme::toggleOff();
    tft->fillRoundRect(25, 243, 190, 26, 8, btnColor);
    tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
    tft->drawString(wifiOk ? "Sync NTP now" : "No Wi-Fi", 120, 256);

    // Timezone note
    tft->setTextFont(2);
    tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
    tft->drawString("CET / CEST  (Warsaw)", 120, 290);
}

void ClockApp::triggerSync() {
    configTzTime(NTP_TZ, NTP_SERVER, "time.cloudflare.com");

    struct tm t;
    if (getLocalTime(&t, 5000)) {
        sysNtpSynced   = true;
        time(&sysLastNtpSync);
    }

    show();
}

void ClockApp::update() {
    // Redraw seconds every loop, minutes when changed
    drawTime(false);

    // Once per minute also refresh date (handles midnight rollover)
    {
        time_t now; time(&now);
        struct tm t; localtime_r(&now, &t);
        if (t.tm_sec == 0) drawDate();
    }

    if (ts->touched()) {
        TS_Point p = ts->getPoint();
        int tx = map(p.x, 300, 3800, 0, 240);
        int ty = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;

            // Back button
            if (ty < 50 && tx < 80) {
                return; // swipe-up in main.cpp handles app exit; header tap ignored
            }

            // Sync button
            if (ty >= 243 && ty <= 269 && tx >= 25 && tx <= 215) {
                if (WiFi.status() == WL_CONNECTED) {
                    tft->fillRoundRect(25, 243, 190, 26, 8, tft->color565(0, 80, 200));
                    tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
                    tft->setTextFont(2);
                    tft->drawString("Syncing...", 120, 256);
                    delay(80);
                    triggerSync();
                }
            }
        }
    } else {
        wasTouched = false;
    }
}
