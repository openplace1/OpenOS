#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <sys/time.h>
#include <SD.h>
#include <WiFi.h>
#include "BluetoothSerial.h"

#include "Config.h"
#include "Applications/Lockscreen.h"
#include "Applications/Home.h"
#include "Applications/Calculator.h"
#include "Applications/TestKeyboard.h"
#include "Applications/Settings.h"
#include "Applications/FilesApp.h"
#include "Applications/ControlCenter.h"
#include "Applications/Clock.h"
#include "Applications/Crypto.h"
#include "Applications/Wallpaper.h"
#include "Applications/Theme.h"
#include "Applications/OSAApp.h"
#include "Services/NotificationService.h"


#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33


#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18

#define TFT_BL 21


SPIClass sdSPI = SPIClass(HSPI);

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

Lockscreen lockscreen(&tft, &ts);
Home home(&tft, &ts);
Calculator calcApp(&tft, &ts);
TestKeyboard notesApp(&tft, &ts);
SettingsApp settingsApp(&tft, &ts);
FilesApp filesApp(&tft, &ts);
ControlCenter controlCenter(&tft, &ts);
ClockApp clockApp(&tft, &ts);

NotificationService notifyService(&tft);
OSAApp osaApp(&tft, &ts);


bool sysWiFiEnabled = false;
bool sysBTEnabled = false;
int sysBrightness = 255;
BluetoothSerial SerialBT;


enum AppState {
    STATE_LOCKSCREEN,
    STATE_HOMESCREEN,
    STATE_IN_APP,
    STATE_CONTROLCENTER
};

AppState currentState = STATE_LOCKSCREEN;
AppState previousState = STATE_HOMESCREEN;
App* activeApp = nullptr;

bool isGlobalSwiping = false;
int globalStartY = 0;

bool isSwipeDown = false;
int startSwipeDownY = 0;

bool isSdReady = false;
bool sysWallpaperEnabled = true;
int  sysTheme = 0; // 0 = light, 1 = dark
bool sysNtpSynced = false;
time_t sysLastNtpSync = 0;

static void applySystemState() {
    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, sysBrightness);

    if (sysWiFiEnabled) {
        WiFi.mode(WIFI_STA);
    } else {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }

    if (sysBTEnabled) {
        SerialBT.begin("OpenOS");
    } else {
        SerialBT.end();
    }
}

static void openControlCenter(AppState returnState) {
    previousState = returnState;
    currentState = STATE_CONTROLCENTER;
    isSwipeDown = false;
    isGlobalSwiping = false;
    controlCenter.show();
}

static void closeControlCenter() {
    currentState = previousState;

    if (previousState == STATE_HOMESCREEN) {
        home.show(false);
    } else if (previousState == STATE_IN_APP && activeApp != nullptr) {
        activeApp->show();
    } else {
        currentState = STATE_HOMESCREEN;
        home.show(false);
    }
}

void setup() {
    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    delay(1000);

    
    struct tm timeinfo;
    timeinfo.tm_hour = 12;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    timeinfo.tm_year = 124; 
    timeinfo.tm_mon = 0;
    timeinfo.tm_mday = 1;

    struct timeval tv;
    tv.tv_sec = mktime(&timeinfo);
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    tft.init();
    tft.invertDisplay(true);
    tft.setRotation(0);

    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, sysBrightness);

    
    
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);

    if (!ts.begin()) {
        Serial.println("Touch not found!");
        tft.fillScreen(TFT_RED);
        return;
    }
    ts.setRotation(0);
    Serial.println("Touch init OK");
    

    
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sdSPI, 20000000)) {
        Serial.println("SD Card mounted perfectly!");
        isSdReady = true;
        Config::load();
        sysWallpaperEnabled = (Config::getInt("wallpaper", 1) != 0);
        sysTheme            = Config::getInt("theme", 0);
        sysWiFiEnabled      = (Config::getInt("wifi", 0) != 0);
    } else {
        Serial.println("No SD Card found at startup!");
    }

    applySystemState();

    // Auto-connect to last saved WiFi network (background, non-blocking)
    if (sysWiFiEnabled && isSdReady) {
        String enc = Config::get("net_0", "");
        if (enc.length() > 0) {
            String dec = Crypto::decrypt(enc);
            int sep = dec.indexOf('|');
            if (sep > 0) {
                String ssid = dec.substring(0, sep);
                String pass = dec.substring(sep + 1);
                WiFi.begin(ssid.c_str(), pass.c_str());
                Serial.println("[WiFi] Auto-connecting to: " + ssid);
            }
        }
    }

    home.addApp(&clockApp);
    home.addApp(&calcApp);
    home.addApp(&notesApp);
    home.addApp(&settingsApp);
    home.addApp(&filesApp);

    lockscreen.show();
    notifyService.push("Hello");
}

void loop() {
    
    
    









    

    
    // Detect background WiFi auto-connect → sync NTP
    static bool autoNtpDone = false;
    if (!autoNtpDone && sysNtpSynced == false && WiFi.status() == WL_CONNECTED) {
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
        struct tm t;
        if (getLocalTime(&t, 3000)) { sysNtpSynced = true; time(&sysLastNtpSync); notifyService.push("Time synced"); }
        autoNtpDone = true;
    }

    int notifChange = notifyService.update();
    if (notifChange == 2) { // banner dismissed — restore only the banner area (top 50px)
        if (currentState == STATE_HOMESCREEN) {
            // Restore wallpaper from cache, then redraw status bar on top
            if (!Wallpaper::drawRegion(&tft, 0, 0, 240, 52)) {
                tft.fillRect(0, 0, 240, 52, Theme::bg());
            }
            home.drawStatusBar();
        } else if (currentState == STATE_IN_APP && activeApp != nullptr) {
            activeApp->drawHeader();
        } else if (currentState == STATE_LOCKSCREEN) {
            lockscreen.show();
        } else if (currentState == STATE_CONTROLCENTER) {
            controlCenter.show();
        }
    }
    // notifChange == 1: banner drew itself on top, no redraw needed

    if (currentState == STATE_LOCKSCREEN) {
        if (lockscreen.update()) {
            currentState = STATE_HOMESCREEN;
            home.show(false);
        }
        return;
    }

    if (currentState == STATE_CONTROLCENTER) {
        if (controlCenter.update()) {
            closeControlCenter();
        }
        return;
    }

    
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!isSwipeDown && touchY < 22) {
            isSwipeDown = true;
            startSwipeDownY = touchY;
        } else if (isSwipeDown) {
            if (touchY - startSwipeDownY > 45) {
                openControlCenter(currentState == STATE_HOMESCREEN ? STATE_HOMESCREEN : STATE_IN_APP);
                delay(120);
                return;
            }
        }
    } else {
        isSwipeDown = false;
    }

    if (currentState == STATE_HOMESCREEN) {
        App* tappedApp = home.update();

        if (tappedApp != nullptr) {
            activeApp = tappedApp;
            currentState = STATE_IN_APP;
            activeApp->show();
        }
        return;
    }

    if (currentState == STATE_IN_APP) {
        bool blockApp = false;

        if (ts.touched()) {
            TS_Point p = ts.getPoint();
            int touchY = map(p.y, 300, 3800, 0, 320);

            if (!isGlobalSwiping && touchY > 300) {
                isGlobalSwiping = true;
                globalStartY = touchY;
                blockApp = true;
            } else if (isGlobalSwiping) {
                blockApp = true;
                if (globalStartY - touchY > 50) {
                    activeApp = nullptr;
                    currentState = STATE_HOMESCREEN;
                    isGlobalSwiping = false;
                    delay(200);
                    home.show(false);
                    return;
                }
            }
        } else {
            isGlobalSwiping = false;
        }

        if (!blockApp && activeApp != nullptr) {
            activeApp->update();

            // Check if FilesApp wants to launch a .osa script
            if (activeApp == &filesApp) {
                String osaPath = filesApp.getPendingOSA();
                if (osaPath.length() > 0) {
                    if (osaApp.loadScript(osaPath)) {
                        activeApp = &osaApp;
                        activeApp->show();
                    } else {
                        notifyService.push("Failed to load script");
                    }
                }
            }
        }
    }
}