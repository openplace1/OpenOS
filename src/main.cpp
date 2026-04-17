#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <sys/time.h>
#include <SD.h>
#include <WiFi.h>
#include "BluetoothSerial.h"

#include "Applications/Lockscreen.h"
#include "Applications/Home.h"
#include "Applications/Calculator.h"
#include "Applications/TestKeyboard.h"
#include "Applications/Settings.h"
#include "Applications/FilesApp.h"
#include "Applications/ControlCenter.h"
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

NotificationService notifyService(&tft);


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
        File wpf = SD.open("/user/wp_enabled.txt", FILE_READ);
        if (wpf) {
            String val = wpf.readStringUntil('\n');
            wpf.close(); val.trim();
            if (val == "0") sysWallpaperEnabled = false;
        }
        File thf = SD.open("/user/theme.txt", FILE_READ);
        if (thf) {
            String val = thf.readStringUntil('\n');
            thf.close(); val.trim();
            if (val == "1") sysTheme = 1;
        }
    } else {
        Serial.println("No SD Card found at startup!");
    }
    

    applySystemState();

    home.addApp(&calcApp);
    home.addApp(&notesApp);
    home.addApp(&settingsApp);
    home.addApp(&filesApp);

    lockscreen.show();
    notifyService.push("Hello");
}

void loop() {
    
    
    









    

    
    int notifChange = notifyService.update();
    if (notifChange == 2) { // banner dismissed — restore covered area
        if (currentState == STATE_HOMESCREEN) {
            home.show(false);
        } else if (currentState == STATE_IN_APP && activeApp != nullptr) {
            activeApp->show();
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
        }
    }
}