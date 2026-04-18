#include "Settings.h"
#include "Wallpaper.h"
#include "Theme.h"
#include <time.h>

extern bool isSdReady;
extern bool sysWiFiEnabled;
extern bool sysBTEnabled;
extern int sysBrightness;
extern bool sysWallpaperEnabled;
extern int  sysTheme;
extern BluetoothSerial SerialBT;

#include "../Services/NotificationService.h"
extern NotificationService notifyService;

#define TFT_BL 21

static uint16_t read16_s(File &f) {
    uint16_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    return result;
}

static uint32_t read32_s(File &f) {
    uint32_t result;
    ((uint8_t *)&result)[0] = f.read();
    ((uint8_t *)&result)[1] = f.read();
    ((uint8_t *)&result)[2] = f.read();
    ((uint8_t *)&result)[3] = f.read();
    return result;
}

SettingsApp::SettingsApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;

    name = "Settings";
    iconColor = tft->color565(142, 142, 147);
    isApp = true;

    kbd = new OSKeyboard(tft, ts);
    currentState = STATE_MENU;

    sliderW = 160; sliderH = 4; sliderX = 40; sliderY = 185;
    brightness = sysBrightness; thumbX = sliderX + map(brightness, 10, 255, 0, sliderW);

    wifiEnabled = sysWiFiEnabled;
    bluetoothEnabled = sysBTEnabled;
    networkCount = 0; showPassword = false; connectionStartTime = 0;

    setHour = 12; setMin = 0; setDay = 1; setMonth = 1; setYear = 2024;
    wallpaperCount = 0; wallpaperPage = 0; wallpaperEnabled = true;

    scrollOffset = 0; lastTouchY = -1; touchStartY = -1;
    isScrollingList = false; isDragging = false; wasTouched = false;

    systemPassword = ""; passcodeTemp = ""; passcodeInput = "";
    passcodeEnabled = false; passcodeShow = false;
    loadSystemPassword();

    pinMode(TFT_BL, OUTPUT);
    setBrightness(brightness);
}

SettingsApp::~SettingsApp() {
    delete kbd;
    if (bluetoothEnabled) SerialBT.end();
}

bool SettingsApp::ensureUserFolder() {
    if (!isSdReady) return false;
    if (!SD.exists("/user")) return SD.mkdir("/user");
    return true;
}

void SettingsApp::loadSystemPassword() {
    systemPassword = "";
    passcodeEnabled = false;
    if (!isSdReady) return;
    if (!SD.exists("/user/pwd.txt")) return;

    File f = SD.open("/user/pwd.txt", FILE_READ);
    if (!f) return;
    systemPassword = f.readStringUntil('\n');
    systemPassword.trim();
    f.close();

    if (systemPassword.length() > 0) passcodeEnabled = true;
}

bool SettingsApp::saveSystemPassword(const String& newPassword) {
    if (!isSdReady) return false;
    if (!ensureUserFolder()) return false;

    SD.remove("/user/pwd.txt");
    File f = SD.open("/user/pwd.txt", FILE_WRITE);
    if (!f) return false;
    if (newPassword.length() > 0) f.println(newPassword);
    f.close();

    systemPassword = newPassword;
    passcodeEnabled = (systemPassword.length() > 0);
    return true;
}

void SettingsApp::resetPasscodeEntry(bool keepShow) {
    passcodeInput = "";
    passcodeTemp = "";
    if (!keepShow) passcodeShow = false;
}

void SettingsApp::setBrightness(int val) {
    if (val < 10) val = 10;
    if (val > 255) val = 255;
    brightness = val;
    sysBrightness = val;
    analogWrite(TFT_BL, val);
}

void SettingsApp::show() {
    wifiEnabled = sysWiFiEnabled;
    bluetoothEnabled = sysBTEnabled;
    brightness = sysBrightness;
    thumbX = sliderX + map(brightness, 10, 255, 0, sliderW);

    if (currentState == STATE_MENU) { loadSystemPassword(); loadWallpaperEnabled(); }

    tft->fillScreen(Theme::bg());

    if (currentState == STATE_MENU) drawMenu();
    else if (currentState == STATE_DISPLAY) drawDisplaySettings();
    else if (currentState == STATE_WIFI) drawWiFiSettings();
    else if (currentState == STATE_WIFI_PASSWORD) drawWiFiPasswordScreen();
    else if (currentState == STATE_WIFI_CONNECTING) drawWiFiConnectingScreen();
    else if (currentState == STATE_TIME) drawTimeSettings();
    else if (currentState == STATE_SDCARD) drawSDCardSettings();
    else if (currentState == STATE_SDCARD_CONFIRM) { drawSDCardSettings(); drawSDCardConfirm(); }
    else if (currentState == STATE_WALLPAPER_CONFIRM) { drawWallpapers(); drawWallpaperConfirm(); }
    else if (currentState == STATE_BLUETOOTH) drawBluetoothSettings();
    else if (currentState == STATE_WALLPAPERS) drawWallpapers();
    else if (currentState == STATE_ABOUT) drawAboutScreen();
    else if (currentState == STATE_PASSCODE_GATE) drawPasscodeInputScreen("Passcode", "Enter current password", "Open");
    else if (currentState == STATE_PASSCODE_MANAGE) drawPasscodeManageScreen();
    else if (currentState == STATE_PASSCODE_SET) drawPasscodeInputScreen("New Passcode", "Type a new password", "Save");
    else if (currentState == STATE_PASSCODE_CONFIRM) drawPasscodeInputScreen("Confirm Passcode", "Type the new password again", "Save");
    else if (currentState == STATE_PASSCODE_DISABLE_VERIFY) drawPasscodeInputScreen("Turn Off Passcode", "Enter current password", "Turn Off");
    else if (currentState == STATE_PASSCODE_RESET_VERIFY) drawPasscodeInputScreen("Reset Passcode", "Enter current password", "Verify");
}

void SettingsApp::drawMenu() {
    tft->fillRect(0, 0, 240, 40, Theme::header());
    tft->drawFastHLine(0, 40, 240, Theme::divider());

    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(Theme::text());
    tft->setTextDatum(MC_DATUM);
    tft->drawString("Settings", 120, 20);

    const int rowH = 35;
    auto drawRow = [&](int y, uint16_t iconColor, String iconText, String title, String valText, bool last) {
        tft->fillRect(0, y, 240, rowH, Theme::surface());
        if (!last) tft->drawFastHLine(0, y + rowH, 240, Theme::divider2());

        tft->fillRoundRect(15, y + 4, 26, 26, 6, iconColor);
        tft->setTextColor(TFT_WHITE);
        tft->setTextDatum(MC_DATUM);
        tft->drawString(iconText, 28, y + 17);

        tft->setTextColor(Theme::text());
        tft->setTextDatum(ML_DATUM);
        tft->drawString(title, 50, y + 17);

        tft->setTextColor(Theme::hint());
        tft->setTextDatum(MR_DATUM);
        tft->drawString(valText, 225, y + 17);
    };

    int y = 40;
    drawRow(y, tft->color565(0, 122, 255), "W", "Wi-Fi", wifiEnabled ? "ON >" : "OFF >", false); y += rowH;
    drawRow(y, tft->color565(0, 122, 255), "B", "Bluetooth", bluetoothEnabled ? "ON >" : "OFF >", false); y += rowH;
    drawRow(y, tft->color565(142, 142, 147), "D", "Display & Brightness", ">", false); y += rowH;
    drawRow(y, tft->color565(255, 45, 85), "P", "Passcode", passcodeEnabled ? "ON >" : "OFF >", false); y += rowH;
    drawRow(y, tft->color565(175, 82, 222), "W", "Wallpapers", ">", false); y += rowH;
    drawRow(y, tft->color565(255, 149, 0), "T", "Time & Date", ">", false); y += rowH;
    drawRow(y, tft->color565(52, 199, 89), "SD", "SD Card", ">", false); y += rowH;
    drawRow(y, tft->color565(142, 142, 147), "i", "About", ">", true);
}

void SettingsApp::drawAboutScreen() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("About Device", 120, 25);

    tft->fillRoundRect(10, 70, 220, 80, 10, Theme::surface());
    tft->drawRoundRect(10, 70, 220, 80, 10, Theme::divider());
    tft->fillRoundRect(20, 90, 40, 40, 8, tft->color565(0, 122, 255));
    tft->setTextColor(TFT_WHITE); tft->setTextFont(4); tft->drawString("OS", 40, 110);

    tft->setTextColor(Theme::text()); tft->setTextFont(2); tft->setTextDatum(ML_DATUM);
    tft->drawString("OpenOS 1.0a", 75, 100);
    tft->setTextColor(Theme::hint());
    tft->drawString("Alpha Release", 75, 120);

    tft->fillRoundRect(10, 165, 220, 110, 10, Theme::surface());
    tft->drawRoundRect(10, 165, 220, 110, 10, Theme::divider());

    int statY = 180;
    auto drawStat = [&](String label, String value) {
        tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString(label, 20, statY);
        tft->setTextColor(Theme::subtext()); tft->setTextDatum(MR_DATUM); tft->drawString(value, 220, statY);
        statY += 30;
    };

    drawStat("CPU Clock:", String(ESP.getCpuFreqMHz()) + " MHz");
    drawStat("Free RAM:", String(ESP.getFreeHeap() / 1024) + " KB");
    drawStat("Uptime:", String(millis() / 60000) + " min");
}

void SettingsApp::drawDisplaySettings() {
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Display", 120, 25);

    // ── Appearance ──────────────────────────────────────────────────────────
    tft->setTextColor(Theme::subtext()); tft->setTextDatum(BL_DATUM);
    tft->drawString("APPEARANCE", 15, 62);
    tft->fillRect(0, 65, 240, 55, Theme::surface());
    tft->drawFastHLine(0, 65, 240, Theme::divider());
    tft->drawFastHLine(0, 120, 240, Theme::divider());

    // Segmented control background
    tft->fillRoundRect(20, 72, 200, 36, 10, tft->color565(228, 228, 228));

    // Light button
    if (sysTheme == 0) {
        tft->fillRoundRect(22, 74, 96, 32, 9, TFT_WHITE);
        tft->setTextColor(TFT_BLACK);
    } else {
        tft->setTextColor(tft->color565(140, 140, 140));
    }
    tft->setTextDatum(MC_DATUM); tft->drawString("Light", 70, 90);

    // Dark button
    if (sysTheme == 1) {
        tft->fillRoundRect(122, 74, 96, 32, 9, TFT_WHITE);
        tft->setTextColor(TFT_BLACK);
    } else {
        tft->setTextColor(tft->color565(140, 140, 140));
    }
    tft->drawString("Dark", 170, 90);

    // ── Brightness ──────────────────────────────────────────────────────────
    tft->setTextColor(Theme::subtext()); tft->setTextDatum(BL_DATUM);
    tft->drawString("BRIGHTNESS", 15, 132);
    tft->fillRect(0, 135, 240, 80, Theme::surface());
    tft->drawFastHLine(0, 135, 240, Theme::divider());
    tft->drawFastHLine(0, 215, 240, Theme::divider());
    tft->fillCircle(20, 185, 4, Theme::hint());
    tft->fillCircle(220, 185, 8, Theme::hint());
    drawSlider();
}

void SettingsApp::drawSlider() {
    
    tft->fillRoundRect(sliderX, sliderY - (sliderH / 2), sliderW, sliderH, 2, Theme::divider2());
    int fillW = thumbX - sliderX;
    if (fillW > 0) {
        tft->fillRoundRect(sliderX, sliderY - (sliderH / 2), fillW, sliderH, 2, tft->color565(52, 199, 89));
    }
    tft->fillCircle(thumbX, sliderY, 12, TFT_WHITE);
    tft->drawCircle(thumbX, sliderY, 12, Theme::divider());
    tft->drawCircle(thumbX, sliderY, 11, Theme::divider2());
}

void SettingsApp::drawToggle(int x, int y, bool state) {
    if (state) {
        tft->fillRoundRect(x, y, 50, 30, 15, tft->color565(52, 199, 89));
        tft->fillCircle(x + 35, y + 15, 13, TFT_WHITE);
    } else {
        tft->fillRoundRect(x, y, 50, 30, 15, Theme::toggleOff());
        tft->fillCircle(x + 15, y + 15, 13, TFT_WHITE);
    }
}

void SettingsApp::drawWiFiSettings() {
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Wi-Fi", 120, 25);

    int toggleY = 70;
    tft->fillRect(0, toggleY, 240, 45, Theme::surface());
    tft->drawFastHLine(0, toggleY, 240, Theme::divider());
    tft->drawFastHLine(0, toggleY + 45, 240, Theme::divider());
    tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString("Wi-Fi", 15, toggleY + 22);
    drawToggle(175, toggleY + 7, wifiEnabled);

    int statusY = 130;
    tft->fillRoundRect(10, statusY, 220, 45, 8, Theme::surface());
    tft->drawRoundRect(10, statusY, 220, 45, 8, Theme::divider());
    tft->setTextDatum(ML_DATUM); tft->setTextColor(Theme::text()); tft->drawString("Status:", 20, statusY + 22);
    tft->setTextDatum(MR_DATUM);
    if (!wifiEnabled) {
        tft->setTextColor(Theme::hint()); tft->drawString("Disabled", 220, statusY + 22);
    } else if (WiFi.status() == WL_CONNECTED) {
        tft->setTextColor(tft->color565(52, 199, 89));
        String ssidText = WiFi.SSID(); if(ssidText.length() > 10) ssidText = ssidText.substring(0, 8) + "..";
        tft->drawString(ssidText, 220, statusY + 22);
    } else {
        tft->setTextColor(Theme::text()); tft->drawString("Scanning...", 220, statusY + 22);
    }

    if (wifiEnabled) {
        tft->setTextColor(Theme::subtext()); tft->setTextDatum(BL_DATUM); tft->drawString("CHOOSE NETWORK...", 15, 195);
        drawWiFiList();
    }
}

void SettingsApp::drawWiFiList() {
    tft->setViewport(0, 200, 240, 120);
    tft->fillScreen(Theme::bg());
    if (networkCount == 0) {
        tft->setTextColor(tft->color565(150, 150, 150)); tft->setTextDatum(MC_DATUM); tft->drawString("Searching...", 120, 40);
    } else {
        for (int i = 0; i < networkCount; i++) {
            int rowY = (i * 45) - scrollOffset;
            if (rowY > -45 && rowY < 120) {
                tft->fillRect(0, rowY, 240, 45, Theme::surface());
                tft->drawFastHLine(0, rowY, 240, Theme::divider());
                String dispSSID = networks[i]; if (dispSSID.length() > 18) dispSSID = dispSSID.substring(0, 15) + "...";
                tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString(dispSSID, 15, rowY + 22);
                if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == networks[i]) {
                    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(MR_DATUM); tft->drawString("V", 225, rowY + 22);
                } else {
                    tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM); tft->drawString("*", 225, rowY + 22);
                }
            }
        }
        int bottomY = (networkCount * 45) - scrollOffset; if (bottomY > 0 && bottomY < 120) tft->drawFastHLine(0, bottomY, 240, Theme::divider());
    }
    tft->resetViewport();
}

void SettingsApp::drawWiFiPasswordScreen() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1); tft->setTextColor(tft->color565(0, 122, 255));
    tft->setTextDatum(ML_DATUM); tft->drawString("< Cancel", 10, 25);
    tft->setTextDatum(MR_DATUM); tft->drawString("Join", 230, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Password", 120, 25);

    tft->setTextColor(Theme::subtext()); tft->setTextDatum(MC_DATUM);
    String dispSSID = selectedSSID; if (dispSSID.length() > 20) dispSSID = dispSSID.substring(0, 17) + "..."; tft->drawString(dispSSID, 120, 65);

    tft->fillRect(10, 85, 160, 40, Theme::surface()); tft->drawRect(10, 85, 160, 40, Theme::divider());
    tft->fillRect(175, 85, 55, 40, Theme::divider2()); tft->drawRect(175, 85, 55, 40, Theme::divider());
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString(showPassword ? "Hide" : "Show", 202, 105);

    tft->setTextDatum(ML_DATUM);
    if (wifiPassword.length() == 0) {
        tft->setTextColor(Theme::hint()); tft->drawString("Password...", 20, 105);
    } else {
        tft->setTextColor(Theme::text());
        if (showPassword) tft->drawString(wifiPassword + "_", 20, 105);
        else {
            String masked = ""; for(int i=0; i<wifiPassword.length(); i++) masked += "*";
            tft->drawString(masked + "_", 20, 105);
        }
    }
    kbd->draw();
}

void SettingsApp::drawWiFiConnectingScreen() {
    tft->fillScreen(Theme::bg());
    tft->setTextFont(2); tft->setTextSize(1); tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString("Connecting to:", 120, 120);
    String dispSSID = selectedSSID; if (dispSSID.length() > 20) dispSSID = dispSSID.substring(0, 17) + "...";
    tft->setTextColor(tft->color565(0, 122, 255)); tft->drawString(dispSSID, 120, 145);
    tft->setTextColor(Theme::hint()); tft->drawString("Please wait...", 120, 180);
}

void SettingsApp::scanWiFi() {
    int n = WiFi.scanNetworks();
    if (n > 0) {
        networkCount = (n > MAX_NETWORKS) ? MAX_NETWORKS : n;
        for (int i = 0; i < networkCount; i++) networks[i] = WiFi.SSID(i);
    } else networkCount = 0;
    scrollOffset = 0; show();
}

void SettingsApp::drawBluetoothSettings() {
    tft->fillScreen(Theme::bg()); tft->fillRect(0, 0, 240, 50, Theme::header()); tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1); tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Bluetooth", 120, 25);

    int toggleY = 70; tft->fillRect(0, toggleY, 240, 45, Theme::surface()); tft->drawFastHLine(0, toggleY, 240, Theme::divider()); tft->drawFastHLine(0, toggleY + 45, 240, Theme::divider());
    tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString("Bluetooth", 15, toggleY + 22); drawToggle(175, toggleY + 7, bluetoothEnabled);

    int statusY = 130; tft->fillRoundRect(10, statusY, 220, 45, 8, Theme::surface()); tft->drawRoundRect(10, statusY, 220, 45, 8, Theme::divider());
    tft->setTextDatum(ML_DATUM); tft->setTextColor(Theme::text()); tft->drawString("Status:", 20, statusY + 22); tft->setTextDatum(MR_DATUM);
    if (bluetoothEnabled) { tft->setTextColor(tft->color565(52, 199, 89)); tft->drawString("Discoverable", 220, statusY + 22); }
    else { tft->setTextColor(Theme::hint()); tft->drawString("Disabled", 220, statusY + 22); }

    if (bluetoothEnabled) {
        tft->setTextColor(Theme::subtext()); tft->setTextDatum(BL_DATUM); tft->drawString("DEVICE VISIBLE AS:", 15, 205);
        tft->fillRect(0, 210, 240, 45, Theme::surface()); tft->drawFastHLine(0, 210, 240, Theme::divider()); tft->drawFastHLine(0, 255, 240, Theme::divider());
        tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString("OpenOS", 15, 232);
    }
}

void SettingsApp::drawTimeControls() {
    tft->fillRect(40, 105, 160, 30, Theme::bg());
    tft->fillRect(10, 225, 220, 30, Theme::bg());
    tft->setTextFont(4); tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    char hStr[3]; sprintf(hStr, "%02d", setHour); tft->drawString(hStr, 80, 120);
    char mStr[3]; sprintf(mStr, "%02d", setMin); tft->drawString(mStr, 160, 120);
    char dStr[5]; sprintf(dStr, "%02d", setDay); tft->drawString(dStr, 45, 240);
    sprintf(dStr, "%02d", setMonth); tft->drawString(dStr, 120, 240);
    sprintf(dStr, "%04d", setYear); tft->drawString(dStr, 195, 240);
}

void SettingsApp::drawTimeSettings() {
    tft->fillScreen(Theme::bg()); tft->fillRect(0, 0, 240, 50, Theme::header()); tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1); tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25); tft->setTextDatum(MR_DATUM); tft->drawString("Save", 230, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Time & Date", 120, 25); tft->setTextFont(4);

    tft->setTextColor(Theme::text());
    tft->fillRoundRect(50, 70, 60, 35, 8, Theme::divider2()); tft->drawString("+", 80, 87);
    tft->fillRoundRect(130, 70, 60, 35, 8, Theme::divider2()); tft->drawString("+", 160, 87);
    tft->fillRoundRect(50, 135, 60, 35, 8, Theme::divider2()); tft->drawString("-", 80, 152);
    tft->fillRoundRect(130, 135, 60, 35, 8, Theme::divider2()); tft->drawString("-", 160, 152);
    tft->drawString(":", 120, 120);

    tft->fillRoundRect(15, 190, 60, 35, 8, Theme::divider2()); tft->drawString("+", 45, 207);
    tft->fillRoundRect(90, 190, 60, 35, 8, Theme::divider2()); tft->drawString("+", 120, 207);
    tft->fillRoundRect(165, 190, 60, 35, 8, Theme::divider2()); tft->drawString("+", 195, 207);
    tft->fillRoundRect(15, 255, 60, 35, 8, Theme::divider2()); tft->drawString("-", 45, 272);
    tft->fillRoundRect(90, 255, 60, 35, 8, Theme::divider2()); tft->drawString("-", 120, 272);
    tft->fillRoundRect(165, 255, 60, 35, 8, Theme::divider2()); tft->drawString("-", 195, 272);

    drawTimeControls();
}

void SettingsApp::wipeSDCard(String path) {
    File dir = SD.open(path); if (!dir) return;
    while (true) {
        File entry = dir.openNextFile(); if (!entry) break;
        String eName = entry.name(); bool isDir = entry.isDirectory(); entry.close();
        String fullPath = path; if (!fullPath.endsWith("/")) fullPath += "/"; fullPath += eName;
        if (isDir) { wipeSDCard(fullPath); SD.rmdir(fullPath); } else SD.remove(fullPath);
        dir.rewindDirectory();
    }
    dir.close();
}

void SettingsApp::drawSDCardSettings() {
    
    tft->fillScreen(Theme::bg()); tft->fillRect(0, 0, 240, 50, Theme::header()); tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1); tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("SD Card", 120, 25);

    tft->fillRoundRect(10, 70, 220, 80, 10, Theme::surface()); tft->drawRoundRect(10, 70, 220, 80, 10, Theme::divider());
    tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString("Status:", 25, 90); tft->drawString("Capacity:", 25, 125); tft->setTextDatum(MR_DATUM);
    if (isSdReady) {
        tft->setTextColor(tft->color565(52, 199, 89)); tft->drawString("Inserted", 215, 90);
        tft->setTextColor(Theme::text()); int sizeMB = SD.cardSize() / (1024 * 1024); tft->drawString(String(sizeMB) + " MB", 215, 125);
    } else {
        tft->setTextColor(tft->color565(255, 59, 48)); tft->drawString("Not Found", 215, 90);
        tft->setTextColor(Theme::text()); tft->drawString("--", 215, 125);
    }

    if (isSdReady) {
        tft->fillRoundRect(10, 170, 220, 50, 10, tft->color565(255, 59, 48));
        tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM); tft->drawString("ERASE CARD", 120, 195);
        tft->setTextColor(Theme::hint()); tft->drawString("Warning: Wipes everything!", 120, 235);
    }
}

void SettingsApp::loadWallpaperEnabled() {
    wallpaperEnabled = true;
    if (!isSdReady) return;
    File f = SD.open("/user/wp_enabled.txt", FILE_READ);
    if (!f) return;
    String val = f.readStringUntil('\n');
    f.close();
    val.trim();
    wallpaperEnabled = (val != "0");
    sysWallpaperEnabled = wallpaperEnabled;
}

void SettingsApp::saveWallpaperEnabled() {
    if (!isSdReady) return;
    ensureUserFolder();
    SD.remove("/user/wp_enabled.txt");
    File f = SD.open("/user/wp_enabled.txt", FILE_WRITE);
    if (!f) return;
    f.println(wallpaperEnabled ? "1" : "0");
    f.close();
}

void SettingsApp::saveTheme() {
    if (!isSdReady) return;
    ensureUserFolder();
    SD.remove("/user/theme.txt");
    File f = SD.open("/user/theme.txt", FILE_WRITE);
    if (!f) return;
    f.println(sysTheme);
    f.close();
}

void SettingsApp::loadWallpapers() {
    wallpaperCount = 0; if (!isSdReady) return;
    File dir = SD.open("/system/assets/wallpapers"); if (!dir) return;
    File file = dir.openNextFile();
    while (file && wallpaperCount < MAX_WALLPAPERS) {
        String fname = file.name();
        int lastSlash = fname.lastIndexOf('/'); if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
        String lowerName = fname; lowerName.toLowerCase();
        if (lowerName.endsWith(".bmp") && !file.isDirectory()) { wallpapers[wallpaperCount] = fname; wallpaperCount++; }
        file = dir.openNextFile();
    }
    dir.close();
}

void SettingsApp::drawBmpThumbnail(String path, int x, int y, int w, int h) {
    File bmpFile = SD.open(path); if (!bmpFile) return;
    if (read16_s(bmpFile) == 0x4D42) {
        read32_s(bmpFile); read32_s(bmpFile); uint32_t imageOffset = read32_s(bmpFile); read32_s(bmpFile);
        int32_t width = read32_s(bmpFile); int32_t height = read32_s(bmpFile);
        bool flip = true; if (height < 0) { height = -height; flip = false; }
        if (read16_s(bmpFile) == 1 && read16_s(bmpFile) == 24) {
            uint32_t rowSize = (width * 3 + 3) & ~3; float stepX = (float)width / w; float stepY = (float)height / h;
            uint8_t* sdbuffer = new uint8_t[rowSize]; uint16_t* lcdbuffer = new uint16_t[w];
            if (sdbuffer && lcdbuffer) {
                for (int thumbY = 0; thumbY < h; thumbY++) {
                    int srcY = thumbY * stepY; if (srcY >= height) srcY = height - 1;
                    int fileRow = flip ? (height - 1 - srcY) : srcY;
                    bmpFile.seek(imageOffset + fileRow * rowSize); bmpFile.read(sdbuffer, rowSize);
                    for (int thumbX = 0; thumbX < w; thumbX++) {
                        int srcX = thumbX * stepX; if (srcX >= width) srcX = width - 1;
                        uint8_t b = sdbuffer[srcX * 3]; uint8_t g = sdbuffer[srcX * 3 + 1]; uint8_t r = sdbuffer[srcX * 3 + 2];
                        lcdbuffer[thumbX] = tft->color565(r, g, b);
                    }
                    tft->startWrite();
                    tft->setAddrWindow(x, y + thumbY, w, 1);
                    tft->pushColors(lcdbuffer, w, true);
                    tft->endWrite();
                }
            }
            if (sdbuffer) delete[] sdbuffer; if (lcdbuffer) delete[] lcdbuffer;
        }
    }
    bmpFile.close();
}

void SettingsApp::drawWallpapers() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Wallpapers", 120, 25);

    // Toggle row
    tft->fillRect(0, 55, 240, 45, Theme::surface());
    tft->drawFastHLine(0, 55, 240, Theme::divider());
    tft->drawFastHLine(0, 100, 240, Theme::divider());
    tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM); tft->drawString("Wallpaper", 15, 77);
    drawToggle(175, 62, wallpaperEnabled);

    if (!wallpaperEnabled) {
        tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
        tft->drawString("Wallpaper is disabled", 120, 200);
        return;
    }

    if (wallpaperCount == 0) {
        tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
        tft->drawString("No .bmp files found in", 120, 175);
        tft->drawString("/system/assets/wallpapers/", 120, 195);
        return;
    }

    int startIdx = wallpaperPage * 4; int endIdx = startIdx + 4; if (endIdx > wallpaperCount) endIdx = wallpaperCount;
    for (int i = startIdx; i < endIdx; i++) {
        int localIdx = i - startIdx; int col = localIdx % 2; int row = localIdx / 2;
        int x = 26 + col * 107; int y = 108 + row * 105;
        tft->fillRoundRect(x - 2, y - 2, 84, 94, 4, Theme::divider());
        drawBmpThumbnail("/system/assets/wallpapers/" + wallpapers[i], x, y, 80, 90);
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        String dName = wallpapers[i]; if (dName.length() > 10) dName = dName.substring(0, 8) + "..";
        tft->drawString(dName, x + 40, y + 99);
    }
    tft->setTextColor(tft->color565(0, 122, 255));
    if (wallpaperPage > 0) tft->drawString("< Previous", 50, 310);
    if (endIdx < wallpaperCount) tft->drawString("Next >", 190, 310);
}

void SettingsApp::drawWallpaperConfirm() {
    for (int row = 0; row < 320; row += 2)
        tft->drawFastHLine(0, row, 240, tft->color565(0, 0, 0));

    int cx = 20, cy = 90, cw = 200, ch = 140;
    tft->fillRoundRect(cx, cy, cw, ch, 14, TFT_WHITE);
    tft->drawRoundRect(cx, cy, cw, ch, 14, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(Theme::text());
    tft->drawString("Experimental Feature", 120, cy + 22);

    tft->setTextColor(Theme::subtext());
    tft->drawString("Wallpaper may slow down", 120, cy + 46);
    tft->drawString("the device significantly.", 120, cy + 62);
    tft->drawString("Not recommended for", 120, cy + 78);
    tft->drawString("daily use.", 120, cy + 94);

    tft->drawFastHLine(cx, cy + 110, cw, tft->color565(220, 220, 220));
    tft->drawFastVLine(cx + cw / 2, cy + 110, ch - 110, tft->color565(220, 220, 220));

    tft->setTextColor(tft->color565(150, 150, 150));
    tft->drawString("Cancel", cx + cw / 4, cy + 125);
    tft->setTextColor(tft->color565(255, 149, 0));
    tft->drawString("Enable anyway", cx + cw * 3 / 4, cy + 125);
}

void SettingsApp::drawSDCardConfirm() {
    // Dim overlay
    for (int row = 0; row < 320; row += 2)
        tft->drawFastHLine(0, row, 240, tft->color565(0, 0, 0));

    // Card
    int cx = 20, cy = 100, cw = 200, ch = 120;
    tft->fillRoundRect(cx, cy, cw, ch, 14, TFT_WHITE);
    tft->drawRoundRect(cx, cy, cw, ch, 14, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(Theme::text());
    tft->drawString("Erase SD Card?", 120, cy + 22);

    tft->setTextColor(Theme::subtext());
    tft->drawString("This will delete all files.", 120, cy + 46);
    tft->drawString("This cannot be undone.", 120, cy + 62);

    // Divider
    tft->drawFastHLine(cx, cy + 78, cw, tft->color565(220, 220, 220));

    // Cancel button (left half)
    tft->setTextColor(tft->color565(0, 122, 255));
    tft->drawString("Cancel", cx + cw / 4, cy + 99);

    // Vertical divider
    tft->drawFastVLine(cx + cw / 2, cy + 78, ch - 78, tft->color565(220, 220, 220));

    // Erase button (right half)
    tft->setTextColor(tft->color565(255, 59, 48));
    tft->drawString("Erase", cx + cw * 3 / 4, cy + 99);
}

void SettingsApp::drawPasscodeInputScreen(const String& title, const String& subtitle, const String& rightAction) {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextDatum(MR_DATUM); tft->drawString(rightAction, 230, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString(title, 120, 25);

    tft->setTextColor(Theme::subtext()); tft->drawString(subtitle, 120, 72);

    drawPasscodeDots(105);

    drawNumericButton(60, 145, 20, "1");
    drawNumericButton(120, 145, 20, "2");
    drawNumericButton(180, 145, 20, "3");
    drawNumericButton(60, 185, 20, "4");
    drawNumericButton(120, 185, 20, "5");
    drawNumericButton(180, 185, 20, "6");
    drawNumericButton(60, 225, 20, "7");
    drawNumericButton(120, 225, 20, "8");
    drawNumericButton(180, 225, 20, "9");
    drawNumericButton(120, 265, 20, "0");

    tft->setTextColor(tft->color565(255, 59, 48)); tft->setTextDatum(ML_DATUM); tft->drawString("Clear", 20, 265);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(MR_DATUM); tft->drawString("Delete", 220, 265);
}

void SettingsApp::drawDangerButton(int x, int y, int w, int h, const String& label, bool lighter) {
    uint16_t fill = lighter ? tft->color565(255, 110, 110) : tft->color565(235, 60, 60);
    tft->fillRoundRect(x, y, w, h, 14, fill);
    tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM); tft->setTextFont(2); tft->setTextSize(1);
    tft->drawString(label, x + w / 2, y + h / 2);
}

void SettingsApp::drawPasscodeManageScreen() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM); tft->drawString("< Back", 10, 25);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->drawString("Passcode", 120, 25);

    tft->fillRoundRect(12, 70, 216, 58, 14, Theme::surface());
    tft->drawRoundRect(12, 70, 216, 58, 14, Theme::divider2());
    tft->setTextColor(Theme::text()); tft->drawString("Passcode is On", 120, 92);

    drawDangerButton(20, 160, 200, 50, "Turn Off Password", false);
    drawDangerButton(20, 225, 200, 50, "Reset Password", true);
}

void SettingsApp::drawNumericButton(int x, int y, int r, const String& label) {
    tft->fillCircle(x, y, r, Theme::dark() ? Theme::c(60,60,65) : tft->color565(235, 235, 240));
    tft->drawCircle(x, y, r, Theme::dark() ? Theme::c(40,40,45) : tft->color565(205, 205, 210));
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM); tft->setTextFont(4); tft->setTextSize(1);
    tft->drawString(label, x, y - 2);
}

void SettingsApp::drawPasscodeDots(int y) {
    int count = passcodeInput.length() > 4 ? passcodeInput.length() : 4;
    if (count > 8) count = 8;
    int spacing = 20;
    int totalW = (count - 1) * spacing;
    int startX = 120 - totalW / 2;

    for (int i = 0; i < count; i++) {
        int x = startX + i * spacing;
        if (i < passcodeInput.length()) tft->fillCircle(x, y, 5, Theme::text());
        else tft->drawCircle(x, y, 5, Theme::hint());
    }
}

void SettingsApp::redrawPasscodeNumericScreen() {
    show();
}

bool SettingsApp::handlePasscodePadTouch(int touchX, int touchY) {
    struct Btn { int x; int y; const char* label; };
    const Btn buttons[] = {
        {60,145,"1"},{120,145,"2"},{180,145,"3"},
        {60,185,"4"},{120,185,"5"},{180,185,"6"},
        {60,225,"7"},{120,225,"8"},{180,225,"9"},
        {120,265,"0"}
    };

    for (const auto& btn : buttons) {
        int dx = touchX - btn.x, dy = touchY - btn.y;
        if (dx*dx + dy*dy <= 20*20) {
            if (passcodeInput.length() < 24) passcodeInput += btn.label;
            redrawPasscodeNumericScreen();
            return true;
        }
    }

    if (touchY > 252 && touchY < 278 && touchX > 10 && touchX < 70) {
        passcodeInput = "";
        redrawPasscodeNumericScreen();
        return true;
    }

    if (touchY > 252 && touchY < 278 && touchX > 150 && touchX < 230) {
        if (passcodeInput.length() > 0) passcodeInput.remove(passcodeInput.length() - 1);
        redrawPasscodeNumericScreen();
        return true;
    }

    return false;
}

void SettingsApp::update() {
    if (ts->touched()) {
        TS_Point p = ts->getPoint();
        int touchX = map(p.x, 300, 3800, 0, 240);
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;
            touchStartY = touchY;
            lastTouchY = touchY;

            if (currentState == STATE_MENU) {
                const int rowH = 35;
                if (touchY > 40 && touchY < 40 + rowH) { currentState = STATE_WIFI; show(); if (wifiEnabled) scanWiFi(); }
                else if (touchY > 40 + rowH && touchY < 40 + rowH * 2) { currentState = STATE_BLUETOOTH; show(); }
                else if (touchY > 40 + rowH * 2 && touchY < 40 + rowH * 3) { currentState = STATE_DISPLAY; show(); }
                else if (touchY > 40 + rowH * 3 && touchY < 40 + rowH * 4) {
                    loadSystemPassword();
                    resetPasscodeEntry();
                    currentState = passcodeEnabled ? STATE_PASSCODE_GATE : STATE_PASSCODE_SET;
                    show();
                }
                else if (touchY > 40 + rowH * 4 && touchY < 40 + rowH * 5) { loadWallpaperEnabled(); loadWallpapers(); wallpaperPage = 0; currentState = STATE_WALLPAPERS; show(); }
                else if (touchY > 40 + rowH * 5 && touchY < 40 + rowH * 6) {
                    struct tm timeinfo;
                    if (getLocalTime(&timeinfo, 10)) {
                        setHour = timeinfo.tm_hour; setMin = timeinfo.tm_min; setDay = timeinfo.tm_mday;
                        setMonth = timeinfo.tm_mon + 1; setYear = timeinfo.tm_year + 1900;
                    }
                    currentState = STATE_TIME; show();
                }
                else if (touchY > 40 + rowH * 6 && touchY < 40 + rowH * 7) { currentState = STATE_SDCARD; show(); }
                else if (touchY > 40 + rowH * 7 && touchY < 320) { currentState = STATE_ABOUT; show(); }
            }
            else if (currentState == STATE_ABOUT) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
            }
            else if (currentState == STATE_WALLPAPERS) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY > 55 && touchY < 100) {
                    if (!wallpaperEnabled) {
                        // Enabling — show warning first
                        currentState = STATE_WALLPAPER_CONFIRM; show();
                    } else {
                        // Disabling — instant
                        wallpaperEnabled = false;
                        sysWallpaperEnabled = false;
                        saveWallpaperEnabled();
                        Wallpaper::invalidate();
                        show();
                    }
                }
                else if (wallpaperEnabled && touchY > 295) {
                    if (touchX < 120 && wallpaperPage > 0) { wallpaperPage--; show(); }
                    else if (touchX > 120 && (wallpaperPage + 1) * 4 < wallpaperCount) { wallpaperPage++; show(); }
                }
                else if (wallpaperEnabled && touchY > 100 && touchY < 295) {
                    int startIdx = wallpaperPage * 4; int endIdx = startIdx + 4; if (endIdx > wallpaperCount) endIdx = wallpaperCount;
                    for (int i = startIdx; i < endIdx; i++) {
                        int localIdx = i - startIdx; int col = localIdx % 2; int row = localIdx / 2;
                        int x = 26 + col * 107; int y = 108 + row * 105;
                        if (touchX > x && touchX < x + 80 && touchY > y && touchY < y + 90) {
                            File cfg = SD.open("/system/wp.txt", FILE_WRITE);
                            if (cfg) { cfg.println("/system/assets/wallpapers/" + wallpapers[i]); cfg.close(); notifyService.push("Wallpaper updated!"); }
                            Wallpaper::invalidate();
                            currentState = STATE_MENU; show();
                        }
                    }
                }
            }
            else if (currentState == STATE_DISPLAY) {
                if (touchY > 10 && touchY < 45 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY > 65 && touchY < 120) {
                    int newTheme = (touchX < 120) ? 0 : 1;
                    if (newTheme != sysTheme) {
                        sysTheme = newTheme;
                        saveTheme();
                        Wallpaper::invalidate();
                        drawDisplaySettings();
                    }
                }
                else if (touchY > sliderY - 25 && touchY < sliderY + 25) { isDragging = true; }
            }
            else if (currentState == STATE_BLUETOOTH) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY > 70 && touchY < 115) {
                    bluetoothEnabled = !bluetoothEnabled;
                    sysBTEnabled = bluetoothEnabled;
                    if (bluetoothEnabled) { SerialBT.begin("OpenOS"); notifyService.push("BT Enabled!"); }
                    else { SerialBT.end(); notifyService.push("BT Disabled"); }
                    show();
                }
            }
            else if (currentState == STATE_SDCARD) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (isSdReady && touchY > 170 && touchY < 220) {
                    currentState = STATE_SDCARD_CONFIRM; show();
                }
            }
            else if (currentState == STATE_WALLPAPER_CONFIRM) {
                // Card: x=20-220, y=90-230; buttons y=200-230
                if (touchY > 200 && touchY < 230) {
                    if (touchX > 20 && touchX < 120) {
                        currentState = STATE_WALLPAPERS; show();
                    } else if (touchX > 120 && touchX < 220) {
                        wallpaperEnabled = true;
                        sysWallpaperEnabled = true;
                        saveWallpaperEnabled();
                        Wallpaper::invalidate();
                        currentState = STATE_WALLPAPERS; show();
                    }
                }
            }
            else if (currentState == STATE_SDCARD_CONFIRM) {
                // Card occupies x=20-220, y=100-220; buttons in bottom row y=178-220
                if (touchY > 178 && touchY < 220) {
                    if (touchX > 20 && touchX < 120) {
                        // Cancel
                        currentState = STATE_SDCARD; show();
                    } else if (touchX > 120 && touchX < 220) {
                        // Erase
                        tft->fillRoundRect(10, 170, 220, 50, 10, tft->color565(200, 30, 25));
                        tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM); tft->drawString("WAIT...", 120, 195);
                        delay(100); wipeSDCard("/");
                        SD.mkdir("/system"); SD.mkdir("/system/assets"); SD.mkdir("/system/assets/wallpapers"); SD.mkdir("/user"); SD.mkdir("/apps");
                        notifyService.push("SD Card wiped!"); currentState = STATE_MENU; show();
                    }
                }
            }
            else if (currentState == STATE_TIME) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY < 50 && touchX > 160) {
                    struct tm timeinfo; getLocalTime(&timeinfo, 10);
                    timeinfo.tm_hour = setHour; timeinfo.tm_min = setMin; timeinfo.tm_sec = 0;
                    timeinfo.tm_mday = setDay; timeinfo.tm_mon = setMonth - 1; timeinfo.tm_year = setYear - 1900;
                    struct timeval tv; tv.tv_sec = mktime(&timeinfo); tv.tv_usec = 0; settimeofday(&tv, NULL);
                    currentState = STATE_MENU; show();
                }
                else if (touchY > 70 && touchY < 105) {
                    if (touchX > 50 && touchX < 110) { setHour = (setHour + 1) % 24; drawTimeControls(); delay(150); }
                    else if (touchX > 130 && touchX < 190) { setMin = (setMin + 1) % 60; drawTimeControls(); delay(150); }
                }
                else if (touchY > 135 && touchY < 170) {
                    if (touchX > 50 && touchX < 110) { setHour = (setHour == 0) ? 23 : setHour - 1; drawTimeControls(); delay(150); }
                    else if (touchX > 130 && touchX < 190) { setMin = (setMin == 0) ? 59 : setMin - 1; drawTimeControls(); delay(150); }
                }
                else if (touchY > 190 && touchY < 225) {
                    if (touchX > 15 && touchX < 75) { setDay = (setDay % 31) + 1; drawTimeControls(); delay(150); }
                    else if (touchX > 90 && touchX < 150) { setMonth = (setMonth % 12) + 1; drawTimeControls(); delay(150); }
                    else if (touchX > 165 && touchX < 225) { setYear++; drawTimeControls(); delay(150); }
                }
                else if (touchY > 255 && touchY < 290) {
                    if (touchX > 15 && touchX < 75) { setDay = (setDay == 1) ? 31 : setDay - 1; drawTimeControls(); delay(150); }
                    else if (touchX > 90 && touchX < 150) { setMonth = (setMonth == 1) ? 12 : setMonth - 1; drawTimeControls(); delay(150); }
                    else if (touchX > 165 && touchX < 225) { if (setYear > 2000) setYear--; drawTimeControls(); delay(150); }
                }
            }
            else if (currentState == STATE_WIFI) {
                if (touchY > 10 && touchY < 45 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY > 70 && touchY < 115) {
                    wifiEnabled = !wifiEnabled; sysWiFiEnabled = wifiEnabled; networkCount = 0; scrollOffset = 0; show();
                    if (wifiEnabled) { WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100); scanWiFi(); }
                    else { WiFi.disconnect(); WiFi.mode(WIFI_OFF); }
                }
                else if (wifiEnabled && touchY > 190) isScrollingList = true;
            }
            else if (currentState == STATE_WIFI_PASSWORD) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_WIFI; showPassword = false; show(); }
                else if (touchY < 50 && touchX > 160) {
                    currentState = STATE_WIFI_CONNECTING; showPassword = false; show();
                    WiFi.disconnect(); delay(10); WiFi.begin(selectedSSID.c_str(), wifiPassword.c_str()); connectionStartTime = millis();
                }
                else if (touchY > 85 && touchY < 125 && touchX > 175) { showPassword = !showPassword; drawWiFiPasswordScreen(); delay(150); }
            }
            else if (currentState == STATE_PASSCODE_GATE || currentState == STATE_PASSCODE_SET || currentState == STATE_PASSCODE_CONFIRM ||
                     currentState == STATE_PASSCODE_DISABLE_VERIFY || currentState == STATE_PASSCODE_RESET_VERIFY) {
                if (touchY < 50 && touchX < 80) {
                    if (currentState == STATE_PASSCODE_CONFIRM && passcodeEnabled) currentState = STATE_PASSCODE_MANAGE;
                    else currentState = STATE_MENU;
                    resetPasscodeEntry();
                    show();
                }
                else if (touchY < 50 && touchX > 150) {
                    if (!isSdReady) { notifyService.push("Need SD card"); return; }

                    if (currentState == STATE_PASSCODE_GATE) {
                        if (passcodeInput == systemPassword) { resetPasscodeEntry(); currentState = STATE_PASSCODE_MANAGE; show(); }
                        else { notifyService.push("Wrong password"); passcodeInput = ""; show(); }
                    }
                    else if (currentState == STATE_PASSCODE_SET) {
                        if (passcodeInput.length() == 0) notifyService.push("Type passcode");
                        else { passcodeTemp = passcodeInput; passcodeInput = ""; currentState = STATE_PASSCODE_CONFIRM; show(); }
                    }
                    else if (currentState == STATE_PASSCODE_CONFIRM) {
                        if (passcodeInput.length() == 0) notifyService.push("Type passcode");
                        else if (passcodeInput != passcodeTemp) { notifyService.push("Passwords differ"); passcodeInput = ""; show(); }
                        else if (saveSystemPassword(passcodeInput)) { notifyService.push("Passcode saved"); resetPasscodeEntry(); currentState = STATE_PASSCODE_MANAGE; show(); }
                        else notifyService.push("Save failed");
                    }
                    else if (currentState == STATE_PASSCODE_DISABLE_VERIFY) {
                        if (passcodeInput != systemPassword) { notifyService.push("Wrong password"); passcodeInput = ""; show(); }
                        else if (saveSystemPassword("")) { notifyService.push("Passcode removed"); resetPasscodeEntry(); currentState = STATE_MENU; show(); }
                        else notifyService.push("Save failed");
                    }
                    else if (currentState == STATE_PASSCODE_RESET_VERIFY) {
                        if (passcodeInput != systemPassword) { notifyService.push("Wrong password"); passcodeInput = ""; show(); }
                        else { resetPasscodeEntry(); currentState = STATE_PASSCODE_SET; show(); }
                    }
                }
                else {
                    handlePasscodePadTouch(touchX, touchY);
                }
            }
            else if (currentState == STATE_PASSCODE_MANAGE) {
                if (touchY < 50 && touchX < 80) { currentState = STATE_MENU; show(); }
                else if (touchY > 160 && touchY < 210 && touchX > 20 && touchX < 220) { resetPasscodeEntry(); currentState = STATE_PASSCODE_DISABLE_VERIFY; show(); }
                else if (touchY > 225 && touchY < 275 && touchX > 20 && touchX < 220) { resetPasscodeEntry(); currentState = STATE_PASSCODE_RESET_VERIFY; show(); }
            }
        }

        if (isDragging && currentState == STATE_DISPLAY) {
            int newThumbX = touchX;
            if (newThumbX < sliderX) newThumbX = sliderX;
            if (newThumbX > sliderX + sliderW) newThumbX = sliderX + sliderW;
            if (newThumbX != thumbX) {
                tft->fillRect(sliderX - 15, sliderY - 15, sliderW + 30, 30, Theme::surface());
                thumbX = newThumbX; drawSlider();
                brightness = map(thumbX, sliderX, sliderX + sliderW, 10, 255); setBrightness(brightness);
            }
        }
        else if (isScrollingList && currentState == STATE_WIFI) {
            int delta = lastTouchY - touchY;
            if (abs(delta) > 2) {
                int maxScroll = (networkCount * 45) - 120; if (maxScroll < 0) maxScroll = 0;
                int oldScroll = scrollOffset; scrollOffset += delta;
                if (scrollOffset < 0) scrollOffset = 0; if (scrollOffset > maxScroll) scrollOffset = maxScroll;
                if (oldScroll != scrollOffset) drawWiFiList();
                lastTouchY = touchY;
            }
        }
    } else {
        if (wasTouched) {
            if (currentState == STATE_WIFI && wifiEnabled && !isDragging) {
                if (abs(lastTouchY - touchStartY) < 10 && touchStartY > 190) {
                    int tappedRow = (touchStartY - 200 + scrollOffset) / 45;
                    if (tappedRow >= 0 && tappedRow < networkCount) {
                        selectedSSID = networks[tappedRow];
                        if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != selectedSSID) {
                            wifiPassword = ""; showPassword = false; currentState = STATE_WIFI_PASSWORD; show();
                        }
                    }
                }
            }
        }
        wasTouched = false; isDragging = false; isScrollingList = false;
    }

    if (currentState == STATE_WIFI_PASSWORD) {
        char c = kbd->update();
        if (c != '\0') {
            if (c == '\b') { if (wifiPassword.length() > 0) wifiPassword.remove(wifiPassword.length() - 1); }
            else if (c == '\n') {
                currentState = STATE_WIFI_CONNECTING; showPassword = false; show();
                WiFi.disconnect(); delay(10); WiFi.begin(selectedSSID.c_str(), wifiPassword.c_str()); connectionStartTime = millis(); return;
            } else if (wifiPassword.length() < 32) wifiPassword += c;
            show();
        }
    }
    else if (currentState == STATE_WIFI_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) { currentState = STATE_WIFI; show(); }
        else if (millis() - connectionStartTime > 10000) { WiFi.disconnect(); currentState = STATE_WIFI; show(); }
    }
}
