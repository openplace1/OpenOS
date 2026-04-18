#include "ControlCenter.h"
#include "Theme.h"
#include "../Config.h"
#include "Crypto.h"
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include "BluetoothSerial.h"

extern bool sysWiFiEnabled;
extern bool sysBTEnabled;
extern int  sysBrightness;
extern bool sysWallpaperEnabled;
extern int  sysTheme;
extern bool sysNtpSynced;
extern time_t sysLastNtpSync;
extern BluetoothSerial SerialBT;

#define TFT_BL 21

// Small-tile icon types
enum { ICON_WIFI=0, ICON_BT=1, ICON_AIRPLANE=2, ICON_CLOCK=3, ICON_MOON=4, ICON_SUN=5 };

// ─── Constructor ─────────────────────────────────────────────────────────────

ControlCenter::ControlCenter(TFT_eSPI* t, XPT2046_Touchscreen* ts_)
    : tft(t), ts(ts_), isDragging(false), wasTouched(false), airplaneMode(false)
{
    sliderW = 156; sliderH = 6;
    sliderX = 38;  sliderY = 199;
    thumbX = sliderX + map(sysBrightness, 10, 255, 0, sliderW);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

void ControlCenter::setBrightness(int val) {
    if (val < 10)  val = 10;
    if (val > 255) val = 255;
    sysBrightness = val;
    analogWrite(TFT_BL, val);
}

void ControlCenter::triggerNTPSync() {
    if (WiFi.status() != WL_CONNECTED) return;
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
    struct tm t;
    if (getLocalTime(&t, 2000)) {
        sysNtpSynced = true;
        time(&sysLastNtpSync);
    }
}

void ControlCenter::applyThemeToggle() {
    sysTheme = (sysTheme == 0) ? 1 : 0;
    Config::setInt("theme", sysTheme);
    Config::save();
}

// ─── Icon painters ───────────────────────────────────────────────────────────

void ControlCenter::iconWiFi(int cx, int cy, uint16_t color, uint16_t bgc) {
    tft->fillCircle(cx, cy + 12, 3, color);
    tft->drawCircle(cx, cy + 12, 8,  color);
    tft->drawCircle(cx, cy + 12, 14, color);
    tft->drawCircle(cx, cy + 12, 20, color);
    tft->fillRect(cx - 22, cy + 12, 44, 22, bgc); // mask lower half
}

void ControlCenter::iconBT(int cx, int cy, uint16_t color, uint16_t bgc) {
    // Vertical bar
    tft->fillRect(cx - 2, cy - 11, 3, 22, color);
    // Upper bump
    tft->fillCircle(cx + 2, cy - 5, 5, color);
    tft->fillCircle(cx + 2, cy - 5, 3, bgc);
    // Lower bump
    tft->fillCircle(cx + 2, cy + 5, 5, color);
    tft->fillCircle(cx + 2, cy + 5, 3, bgc);
    // Diagonals (BT logo spikes)
    tft->drawLine(cx - 2, cy - 11, cx + 6, cy - 5, color);
    tft->drawLine(cx - 2, cy + 11, cx + 6, cy + 5, color);
}

void ControlCenter::iconAirplane(int cx, int cy, uint16_t color) {
    // Fuselage
    tft->fillRoundRect(cx - 2, cy - 14, 4, 26, 2, color);
    // Wings (wide horizontal bar in the middle)
    tft->fillTriangle(cx - 16, cy + 2, cx + 16, cy + 2, cx, cy - 6, color);
    // Tail fin
    tft->fillTriangle(cx - 8, cy + 12, cx + 8, cy + 12, cx, cy + 4, color);
}

void ControlCenter::iconClock(int cx, int cy, uint16_t color, uint16_t bgc) {
    tft->drawCircle(cx, cy, 10, color);
    tft->fillCircle(cx, cy, 9, bgc);
    tft->drawCircle(cx, cy, 10, color);
    tft->drawLine(cx, cy, cx,     cy - 7, color); // 12-hand
    tft->drawLine(cx, cy, cx + 5, cy + 3, color); // 3-hand
    tft->fillCircle(cx, cy, 2, color);
}

void ControlCenter::iconMoon(int cx, int cy, uint16_t color, uint16_t bgc) {
    tft->fillCircle(cx, cy, 9, color);
    tft->fillCircle(cx + 5, cy - 3, 7, bgc); // bite out of the circle = crescent
}

void ControlCenter::iconSun(int cx, int cy, uint16_t color) {
    tft->fillCircle(cx, cy, 6, color);
    // Rays
    for (int a = 0; a < 8; a++) {
        float ang = a * 3.14159f / 4.0f;
        int x1 = cx + (int)(9  * cosf(ang));
        int y1 = cy + (int)(9  * sinf(ang));
        int x2 = cx + (int)(13 * cosf(ang));
        int y2 = cy + (int)(13 * sinf(ang));
        tft->drawLine(x1, y1, x2, y2, color);
    }
}

// ─── Tile drawers ────────────────────────────────────────────────────────────

void ControlCenter::drawBigTile(int x, int y, int w, int h, bool active,
                                uint16_t activeColor, const char* label,
                                const char* status, int iconType) {
    uint16_t bgc      = active ? activeColor : card();
    uint16_t iconClr  = active ? TFT_WHITE   : dim();
    uint16_t labelClr = active ? TFT_WHITE   : tft->color565(200, 200, 200);
    uint16_t subClr   = active ? tft->color565(190, 220, 255) : tft->color565(110, 110, 110);

    tft->fillRoundRect(x, y, w, h, 16, bgc);

    // Icon centered in upper 60% of tile
    int icx = x + w / 2 - 14, icy = y + 22;
    if (iconType == ICON_WIFI) iconWiFi(icx, icy, iconClr, bgc);
    if (iconType == ICON_BT)   iconBT  (icx, icy, iconClr, bgc);

    // Text in lower part
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextDatum(BL_DATUM);
    tft->setTextColor(labelClr);
    tft->drawString(label, x + 10, y + h - 22);

    String st(status);
    if (st.length() > 12) st = st.substring(0, 10) + "..";
    tft->setTextFont(1);
    tft->setTextColor(subClr);
    tft->drawString(st.c_str(), x + 10, y + h - 7);
}

void ControlCenter::drawSmallTile(int x, int y, int w, int h, bool active,
                                  uint16_t activeColor, const char* label, int iconType) {
    uint16_t bgc     = active ? activeColor : card();
    uint16_t iconClr = active ? TFT_WHITE   : dim();
    uint16_t txtClr  = active ? TFT_WHITE   : dim();

    tft->fillRoundRect(x, y, w, h, 12, bgc);

    int icx = x + w / 2, icy = y + h / 2 - 7;
    if (iconType == ICON_AIRPLANE) iconAirplane(icx, icy, iconClr);
    if (iconType == ICON_CLOCK)    iconClock   (icx, icy, iconClr, bgc);
    if (iconType == ICON_MOON)     iconMoon    (icx, icy, iconClr, bgc);
    if (iconType == ICON_SUN)      iconSun     (icx, icy, iconClr);

    tft->setTextFont(1);
    tft->setTextSize(1);
    tft->setTextDatum(BC_DATUM);
    tft->setTextColor(txtClr);
    tft->drawString(label, x + w / 2, y + h - 3);
}

// ─── Brightness card ─────────────────────────────────────────────────────────

void ControlCenter::drawSlider() {
    tft->fillRoundRect(sliderX, sliderY - sliderH / 2, sliderW, sliderH, 3,
                       tft->color565(70, 70, 75));
    int fillW = thumbX - sliderX;
    if (fillW > 0)
        tft->fillRoundRect(sliderX, sliderY - sliderH / 2, fillW, sliderH, 3, TFT_WHITE);
    tft->fillCircle(thumbX, sliderY, 12, TFT_WHITE);
    tft->drawCircle(thumbX, sliderY, 12, tft->color565(80, 80, 85));
}

void ControlCenter::drawBrightnessCard() {
    tft->fillRoundRect(8, 174, 224, 52, 14, card());

    // Dim icon (left)
    iconMoon(22, 199, dim(), card());

    // Bright icon (right)
    iconSun(218, 199, TFT_WHITE);

    drawSlider();
}

// ─── show / update ───────────────────────────────────────────────────────────

void ControlCenter::show() {
    thumbX = sliderX + map(sysBrightness, 10, 255, 0, sliderW);

    tft->fillScreen(bg());

    // Swipe pill
    tft->fillRoundRect(104, 6, 32, 4, 2, tft->color565(75, 75, 80));

    // WiFi status string
    String wifiSt = "Off";
    if (sysWiFiEnabled) {
        wifiSt = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Searching...";
    }
    bool wifiActive = sysWiFiEnabled && (WiFi.status() == WL_CONNECTED);

    // Big tiles (y=14, h=94)
    drawBigTile(8,   14, 108, 94, wifiActive,   blue(), "Wi-Fi",     wifiSt.c_str(), ICON_WIFI);
    drawBigTile(124, 14, 108, 94, sysBTEnabled, blue(), "Bluetooth", sysBTEnabled ? "OpenOS" : "Off", ICON_BT);

    // Small tiles (y=116, h=50): Airplane | NTP | Theme | (placeholder)
    bool themeIsDark   = (sysTheme == 1);
    bool canNtp        = (WiFi.status() == WL_CONNECTED);
    drawSmallTile(8,   116, 52, 50, airplaneMode, org(),  "Airplane",           ICON_AIRPLANE);
    drawSmallTile(66,  116, 52, 50, canNtp,       blue(), sysNtpSynced ? "Synced" : "NTP", ICON_CLOCK);
    drawSmallTile(124, 116, 52, 50, themeIsDark,  pur(),  themeIsDark ? "Dark" : "Light",  ICON_MOON);
    // 4th tile — decorative or future use
    tft->fillRoundRect(182, 116, 52, 50, 12, tft->color565(38, 38, 40));

    // Brightness card
    drawBrightnessCard();

    // Network info card (y=234)
    tft->fillRoundRect(8, 234, 224, 50, 14, card());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(200, 200, 200)); tft->setTextDatum(ML_DATUM);
    tft->drawString("Network", 22, 252);
    tft->setTextFont(1);
    if (WiFi.status() == WL_CONNECTED) {
        tft->setTextColor(tft->color565(100, 200, 100));
        String ip = WiFi.localIP().toString();
        tft->drawString(ip.c_str(), 22, 268);
        tft->setTextColor(dim()); tft->setTextDatum(MR_DATUM);
        int rssi = WiFi.RSSI();
        tft->drawString((String(rssi) + " dBm").c_str(), 226, 259);
    } else {
        tft->setTextColor(dim());
        tft->drawString(sysWiFiEnabled ? "Not connected" : "Wi-Fi off", 22, 268);
    }

    // Bottom swipe hint
    tft->fillRoundRect(104, 300, 32, 4, 2, tft->color565(55, 55, 60));
}

void ControlCenter::drawHeader() {
    // CC has no fixed header — just redraw the swipe pill area
    tft->fillRect(0, 0, 240, 14, bg());
    tft->fillRoundRect(104, 6, 32, 4, 2, tft->color565(75, 75, 80));
}

bool ControlCenter::update() {
    // Refresh WiFi status every ~2s so "Searching..." resolves to SSID
    static unsigned long lastRefresh = 0;
    static wl_status_t lastWlStatus = WL_IDLE_STATUS;
    wl_status_t curStatus = WiFi.status();
    if (curStatus != lastWlStatus || millis() - lastRefresh > 2000) {
        lastWlStatus = curStatus;
        lastRefresh  = millis();
        if (sysWiFiEnabled) {
            // Redraw only the WiFi tile + NTP tile (avoids full screen redraw)
            String wifiSt = (curStatus == WL_CONNECTED) ? WiFi.SSID() : "Searching...";
            bool wifiActive = (curStatus == WL_CONNECTED);
            drawBigTile(8, 14, 108, 94, wifiActive, blue(), "Wi-Fi", wifiSt.c_str(), ICON_WIFI);
            bool canNtp = (curStatus == WL_CONNECTED);
            drawSmallTile(66, 116, 52, 50, canNtp, blue(),
                          sysNtpSynced ? "Synced" : "NTP", ICON_CLOCK);
            tft->fillRoundRect(8, 234, 224, 50, 14, card());
            tft->setTextFont(2); tft->setTextSize(1);
            tft->setTextColor(tft->color565(200,200,200)); tft->setTextDatum(ML_DATUM);
            tft->drawString("Network", 22, 252);
            tft->setTextFont(1);
            if (curStatus == WL_CONNECTED) {
                tft->setTextColor(tft->color565(100,200,100));
                tft->drawString(WiFi.localIP().toString().c_str(), 22, 268);
                tft->setTextColor(dim()); tft->setTextDatum(MR_DATUM);
                tft->drawString((String(WiFi.RSSI()) + " dBm").c_str(), 226, 259);
            } else {
                tft->setTextColor(dim()); tft->setTextDatum(ML_DATUM);
                tft->drawString(sysWiFiEnabled ? "Connecting..." : "Wi-Fi off", 22, 268);
            }
        }
    }

    if (ts->touched()) {
        TS_Point p = ts->getPoint();
        int touchX = map(p.x, 300, 3800, 0, 240);
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;

            // WiFi tile
            if (touchX >= 8 && touchX <= 116 && touchY >= 14 && touchY <= 108) {
                if (!airplaneMode) {
                    sysWiFiEnabled = !sysWiFiEnabled;
                    if (sysWiFiEnabled) {
                        WiFi.mode(WIFI_STA);
                        // Auto-connect to last saved network
                        String enc = Config::get("net_0", "");
                        if (enc.length() > 0) {
                            String dec = Crypto::decrypt(enc);
                            int sep = dec.indexOf('|');
                            if (sep > 0)
                                WiFi.begin(dec.substring(0, sep).c_str(),
                                           dec.substring(sep + 1).c_str());
                        }
                    } else {
                        WiFi.disconnect(true);
                        WiFi.mode(WIFI_OFF);
                    }
                    Config::setInt("wifi", sysWiFiEnabled ? 1 : 0);
                    Config::save();
                }
                show(); return false;
            }
            // BT tile
            if (touchX >= 124 && touchX <= 232 && touchY >= 14 && touchY <= 108) {
                if (!airplaneMode) {
                    sysBTEnabled = !sysBTEnabled;
                    if (sysBTEnabled) SerialBT.begin("OpenOS"); else SerialBT.end();
                }
                show(); return false;
            }
            // Airplane tile
            if (touchX >= 8 && touchX <= 60 && touchY >= 116 && touchY <= 166) {
                airplaneMode = !airplaneMode;
                if (airplaneMode) {
                    sysWiFiEnabled = false; sysBTEnabled = false;
                    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
                    SerialBT.end();
                } else {
                    // Restore from config
                    sysWiFiEnabled = (Config::getInt("wifi", 0) != 0);
                    if (sysWiFiEnabled) WiFi.mode(WIFI_STA);
                }
                show(); return false;
            }
            // NTP tile
            if (touchX >= 66 && touchX <= 118 && touchY >= 116 && touchY <= 166) {
                triggerNTPSync();
                show(); return false;
            }
            // Theme tile
            if (touchX >= 124 && touchX <= 176 && touchY >= 116 && touchY <= 166) {
                applyThemeToggle();
                show(); return false;
            }
            // Brightness slider click
            if (touchY >= sliderY - 25 && touchY <= sliderY + 25) {
                isDragging = true;
            }
            // Bottom swipe-up area → close
            if (touchY >= 292) {
                wasTouched = false;
                return true;
            }
        }

        if (isDragging) {
            int nx = touchX;
            if (nx < sliderX) nx = sliderX;
            if (nx > sliderX + sliderW) nx = sliderX + sliderW;
            if (nx != thumbX) {
                tft->fillRect(sliderX - 14, sliderY - 14, sliderW + 28, 28, card());
                thumbX = nx;
                drawSlider();
                setBrightness(map(thumbX, sliderX, sliderX + sliderW, 10, 255));
            }
        }
    } else {
        wasTouched = false;
        isDragging = false;
    }
    return false;
}
