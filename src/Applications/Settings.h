#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <sys/time.h>
#include "../App.h"
#include "OSKeyboard.h"
#include "BluetoothSerial.h"

class SettingsApp : public App {
private:
    enum SettingsState {
        STATE_MENU,
        STATE_DISPLAY,
        STATE_WIFI,
        STATE_WIFI_PASSWORD,
        STATE_WIFI_CONNECTING,
        STATE_TIME,
        STATE_SDCARD,
        STATE_BLUETOOTH,
        STATE_WALLPAPERS,
        STATE_ABOUT,
        STATE_PASSCODE_GATE,
        STATE_PASSCODE_MANAGE,
        STATE_PASSCODE_SET,
        STATE_PASSCODE_CONFIRM,
        STATE_PASSCODE_DISABLE_VERIFY,
        STATE_PASSCODE_RESET_VERIFY
    };

    static const int MAX_NETWORKS = 20;
    static const int MAX_WALLPAPERS = 12;

    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;
    OSKeyboard* kbd;

    SettingsState currentState;

    int sliderX, sliderY, sliderW, sliderH;
    int thumbX;
    int brightness;

    bool wifiEnabled;
    bool bluetoothEnabled;

    String networks[MAX_NETWORKS];
    int networkCount;
    String selectedSSID;
    String wifiPassword;
    bool showPassword;
    unsigned long connectionStartTime;

    int setHour, setMin, setDay, setMonth, setYear;

    String wallpapers[MAX_WALLPAPERS];
    int wallpaperCount;
    int wallpaperPage;

    int scrollOffset;
    int lastTouchY;
    int touchStartY;
    bool isScrollingList;
    bool isDragging;
    bool wasTouched;

    String systemPassword;
    String passcodeTemp;
    String passcodeInput;
    bool passcodeEnabled;
    bool passcodeShow;

    void setBrightness(int val);
    void drawMenu();
    void drawAboutScreen();
    void drawDisplaySettings();
    void drawSlider();
    void drawWiFiSettings();
    void drawWiFiList();
    void drawWiFiPasswordScreen();
    void drawWiFiConnectingScreen();
    void drawBluetoothSettings();
    void drawTimeSettings();
    void drawTimeControls();
    void drawSDCardSettings();
    void drawWallpapers();
    void drawBmpThumbnail(String path, int x, int y, int w, int h);
    void drawToggle(int x, int y, bool state);
    void scanWiFi();
    void loadWallpapers();
    void wipeSDCard(String path);

    void drawPasscodeInputScreen(const String& title, const String& subtitle, const String& rightAction);
    void drawPasscodeManageScreen();
    void drawDangerButton(int x, int y, int w, int h, const String& label, bool lighter);
    void drawNumericButton(int x, int y, int r, const String& label);
    void drawPasscodeDots(int y);
    void redrawPasscodeNumericScreen();
    bool handlePasscodePadTouch(int touchX, int touchY);

    void loadSystemPassword();
    bool saveSystemPassword(const String& newPassword);
    bool ensureUserFolder();
    void resetPasscodeEntry(bool keepShow = false);

public:
    SettingsApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    ~SettingsApp();

    void show() override;
    void update() override;
};

#endif
