#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <sys/time.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_system.h>
#include "BluetoothSerial.h"

#include "Config.h"
// Lockscreen is now an OSA script (/system/apps/lockscreen.osa) that runs in
// osaApp on boot — no native class. See LOCKSCREEN_SCRIPT below.

// Override the weak symbol defined in arduino-esp32's main.cpp.
// The default loop task gets 8 KB which is not enough for the OSA runtime's
// recursive expression evaluator + nested user-function calls. 32 KB has lots
// of headroom and the ESP32 has 320 KB of RAM total — it's cheap.
size_t getArduinoLoopTaskStackSize() {
    return 32768;
}

#include "Applications/Home.h"
#include "Applications/Crypto.h"
#include "Applications/Wallpaper.h"
#include "Applications/Theme.h"
#include "Applications/OSAApp.h"
#include "Runtime/OSARuntime.h"


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

// Lockscreen lives in osaApp (loaded with /system/apps/lockscreen.osa at boot).
Home home(&tft, &ts);

OSAApp osaApp(&tft, &ts);
// Second runtime dedicated to overlays (Control Center). Allocated lazily —
// the inline arrays inside OSARuntime (lines[512], vars[96], funcs[24]) plus
// any loaded script content add up to 20-30 KB of permanent heap. Allocating
// on demand frees that memory back to other consumers (sprites!) when CC
// isn't open.
static OSAApp* osaOverlayApp = nullptr;
static OSAApp* g_underlyingApp = nullptr;  // activeApp captured before CC opened


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
OSAApp* activeApp = nullptr;

bool isGlobalSwiping = false;
int globalStartY = 0;

bool isSwipeDown = false;
int startSwipeDownY = 0;

bool isSdReady = false;
bool sysWallpaperEnabled = true;
int  sysTheme = 0; // 0 = light, 1 = dark
bool sysNtpSynced = false;
time_t sysLastNtpSync = 0;

// ─── Crash-recovery screen ───────────────────────────────────────────────────
// Shown when the *previous* boot ended in a panic / watchdog / brownout, so
// the device doesn't silently bounce back to the lockscreen leaving the user
// guessing whether anything just blew up.

static const char* crashReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:    return "EXCEPTION";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WATCHDOG";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        default:               return "UNKNOWN";
    }
}

static bool isCrashReset(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC    || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT     ||
           r == ESP_RST_BROWNOUT;
}

static void showCrashScreen(esp_reset_reason_t reason) {
    // Minimal hardware bring-up — enough for the panel and backlight only.
    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    Serial.printf("[CRASH] previous boot ended with reason=%d (%s)\n",
                  (int)reason, crashReasonName(reason));

    tft.init();
    tft.invertDisplay(true);
    tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, 220);

    const uint16_t bg     = tft.color565(110, 18, 18);   // muted dark red
    const uint16_t panel  = tft.color565(70, 10, 10);
    const uint16_t accent = tft.color565(255, 220, 220);

    tft.fillScreen(bg);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(accent);
    tft.setTextFont(4); tft.setTextSize(1);
    tft.drawString("System crashed", 120, 40);

    tft.setTextFont(2);
    tft.setTextColor(tft.color565(255, 180, 180));
    tft.drawString("OpenOS hit an error and", 120, 80);
    tft.drawString("will restart automatically.", 120, 98);

    // Error-code chip
    tft.fillRoundRect(40, 132, 160, 32, 8, panel);
    tft.drawRoundRect(40, 132, 160, 32, 8, accent);
    tft.setTextColor(accent);
    tft.drawString(String("Code: ") + crashReasonName(reason), 120, 148);

    // Countdown — repaints just the bottom strip to avoid flicker.
    for (int s = 8; s >= 1; s--) {
        tft.fillRect(0, 196, 240, 32, bg);
        tft.setTextColor(accent);
        tft.drawString(String("Restarting in ") + s + "s", 120, 212);
        delay(1000);
    }

    ESP.restart();
}



// Walks dirPath one level deep, registering every `.osa` script that opted in
// with `#isApp true` as a home tile via home.addScript.
static void scanDirForScripts(const String& dirPath, int depth) {
    if (home.appCount >= Home::MAX_APPS || depth > 1) return;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && home.appCount < Home::MAX_APPS) {
        String basename = f.name();
        int slash = basename.lastIndexOf('/');
        if (slash >= 0) basename = basename.substring(slash + 1);
        String full = dirPath;
        if (!full.endsWith("/")) full += "/";
        full += basename;
        if (full.startsWith("/apps/")) { f = dir.openNextFile(); continue; }

        if (f.isDirectory()) {
            if (depth == 0) scanDirForScripts(full, 1);
        } else {
            String lower = full; lower.toLowerCase();
            if (lower.endsWith(".osa") && OSARuntime::readIsAppFromFile(full)) {
                home.addScript(full,
                               OSARuntime::readAppNameFromFile(full),
                               OSARuntime::readIconColorFromFile(full, tft.color565(255, 149, 0)));
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
}

static void registerOsaShortcuts() {
    if (!isSdReady) return;
    scanDirForScripts("/", 0);
    scanDirForScripts("/system/apps", 1);
}

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

// ─── Folder helpers (operate on HomeTile values) ────────────────────────────
static int g_folderCounter = 0;

static void createFolderFromIcon(int idx) {
    if (idx < 0 || idx >= home.appCount) return;
    HomeTile& victim = home.tiles[idx];
    if (victim.isFolder) return;

    HomeTile leaf = victim;        // copy out the leaf data
    g_folderCounter++;

    HomeTile& slot = home.tiles[idx];
    slot.freeChildren();
    slot.name       = String("Folder ") + g_folderCounter;
    slot.scriptPath = "";
    slot.color      = tft.color565(70, 70, 86);
    slot.isFolder   = true;
    slot.children   = nullptr;
    slot.childCount = 0;
    slot.childCap   = 0;
    slot.addChild(leaf);

    home.saveOrder();
}

static void addAppToFolderImpl(int folderIdx, int appIdx) {
    if (folderIdx < 0 || folderIdx >= home.appCount) return;
    if (appIdx    < 0 || appIdx    >= home.appCount) return;
    if (folderIdx == appIdx) return;
    HomeTile& dst = home.tiles[folderIdx];
    HomeTile& src = home.tiles[appIdx];
    if (!dst.isFolder || src.isFolder) return;
    if (!dst.addChild(src)) return;

    // Remove src from grid (folderIdx shifts if appIdx < folderIdx — fine,
    // saveOrder rewrites positions anyway).
    src.freeChildren();
    for (int i = appIdx; i < home.appCount - 1; i++) home.tiles[i] = home.tiles[i + 1];
    home.tiles[--home.appCount] = HomeTile();
    home.saveOrder();
}

static void deleteFolderAt(int idx) {
    if (idx < 0 || idx >= home.appCount) return;
    HomeTile& f = home.tiles[idx];
    if (!f.isFolder) return;

    // Stash children so we can append them after we collapse the gap.
    HomeTile stash[16];
    int stashCount = f.childCount;
    for (int i = 0; i < stashCount; i++) stash[i] = f.children[i];
    f.freeChildren();

    for (int i = idx; i < home.appCount - 1; i++) home.tiles[i] = home.tiles[i + 1];
    home.tiles[--home.appCount] = HomeTile();
    for (int i = 0; i < stashCount && home.appCount < Home::MAX_APPS; i++) {
        home.tiles[home.appCount++] = stash[i];
    }
    home.saveOrder();
}

// ─── extern wrappers — bridge to OSARuntime's home.* builtins ───────────────
// Runtime sees these via extern declarations and forwards OSA-side calls into
// our file-local static helpers.
void osaMakeFolder(int idx)                      { createFolderFromIcon(idx); }
void osaDeleteFolder(int idx)                    { deleteFolderAt(idx); }
void osaAddToFolder(int folderIdx, int appIdx)   { addAppToFolderImpl(folderIdx, appIdx); }
// anim.openTile() in OSA used to play a zoom-in animation here; it now just
// records which tile launched so a future close anim could find its position.
// The animation calls themselves are gone — they didn't feel snappy enough.
void osaPlayOpenAnim(int idx) {
    if (idx < 0 || idx >= home.appCount) return;
    home.lastLaunchX     = 12 + (idx % 4) * 55 + 23;
    home.lastLaunchY     = 30 + (idx / 4) * 80 + 23;
    home.lastLaunchColor = home.tiles[idx].color;
}

// TEMPORARY: loads /main.osa into osaApp on every "return to home" transition
// (boot, lockscreen unlock, every app exit). No fallback. Restore the normal
// home.osa path when done testing.
static void loadHomeScript() {
    osaApp.recycle();
    if (osaApp.loadScript("/main.osa")) {
        activeApp = &osaApp;
        osaApp.show();
    } else {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED); tft.setTextDatum(MC_DATUM); tft.setTextFont(2);
        tft.drawString("/main.osa missing", 120, 150);
        tft.drawString("Copy it to the SD root", 120, 175);
        activeApp = nullptr;
    }
}

static void openControlCenter(AppState returnState) {
    previousState = returnState;
    g_underlyingApp = activeApp;
    isSwipeDown = false;
    isGlobalSwiping = false;
    if (!osaOverlayApp) osaOverlayApp = new OSAApp(&tft, &ts);
    if (osaOverlayApp && osaOverlayApp->loadScript("/system/apps/controlcenter.osa")) {
        activeApp    = osaOverlayApp;
        currentState = STATE_CONTROLCENTER;
        activeApp->show();
    } else {
        delete osaOverlayApp;
        osaOverlayApp = nullptr;
    }
}

static void closeControlCenter() {
    activeApp    = g_underlyingApp;
    g_underlyingApp = nullptr;
    currentState = previousState;
    // Free the overlay runtime so its 20-30 KB of inline arrays + loaded
    // script content goes back to the heap (sprites need every byte).
    if (osaOverlayApp) {
        delete osaOverlayApp;
        osaOverlayApp = nullptr;
    }
    if (activeApp != nullptr) {
        // Underlying app (or home script) is still loaded — just repaint.
        activeApp->show();
    } else {
        // Lost the underlying activeApp somehow — bounce to home.
        currentState = STATE_HOMESCREEN;
        loadHomeScript();
    }
}

void setup() {
    // Crash-recovery: if the *previous* boot ended in a panic/watchdog/brownout,
    // show the BSOD-style screen and reboot. Never returns in that branch.
    esp_reset_reason_t resetReason = esp_reset_reason();
    if (isCrashReset(resetReason)) {
        showCrashScreen(resetReason);
        // unreachable — ESP.restart() inside showCrashScreen
    }

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
        tft.fillScreen(TFT_RED);
        return;
    }
    ts.setRotation(0);
    

    
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sdSPI, 20000000)) {
        isSdReady = true;
        Config::load();
        sysWallpaperEnabled = (Config::getInt("wallpaper", 1) != 0);
        sysTheme            = Config::getInt("theme", 0);
        sysWiFiEnabled      = (Config::getInt("wifi", 0) != 0);
    }

    applySystemState();

    // Auto-connect to last saved WiFi network (background, non-blocking)
    if (sysWiFiEnabled && isSdReady) {
        String enc = Config::get("net_0", "");
        if (enc.length() > 0) {
            String dec = Crypto::decrypt(enc);
            int sep = dec.indexOf('|');
            if (sep > 0) {
                WiFi.begin(dec.substring(0, sep).c_str(),
                           dec.substring(sep + 1).c_str());
            }
        }
    }

    registerOsaShortcuts();
    home.applyOrder();

    // TEMPORARY: boot straight into /main.osa. No fallback — if it's missing
    // we paint a hard error and stop. Restore the normal lockscreen flow when
    // done testing.
    if (osaApp.loadScript("/main.osa")) {
        osaApp.show();
        activeApp = &osaApp;
    } else {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED); tft.setTextDatum(MC_DATUM); tft.setTextFont(2);
        tft.drawString("/main.osa missing", 120, 150);
        tft.drawString("Copy it to the SD root", 120, 175);
        activeApp = nullptr;
    }
}

void loop() {
    // Background WiFi -> NTP one-shot once the radio comes up.
    static bool autoNtpDone = false;
    if (!autoNtpDone && sysNtpSynced == false && WiFi.status() == WL_CONNECTED) {
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
        struct tm t;
        if (getLocalTime(&t, 3000)) { sysNtpSynced = true; time(&sysLastNtpSync); }
        autoNtpDone = true;
    }

    if (currentState == STATE_LOCKSCREEN) {
        osaApp.update();
        if (osaApp.wantsExit) {
            osaApp.wantsExit = false;
            currentState = STATE_HOMESCREEN;
            loadHomeScript();
        }
        return;
    }

    if (currentState == STATE_CONTROLCENTER) {
        if (!osaOverlayApp) {
            currentState = STATE_HOMESCREEN;
            loadHomeScript();
            return;
        }
        osaOverlayApp->update();
        if (osaOverlayApp->wantsExit) {
            osaOverlayApp->wantsExit = false;
            osaOverlayApp->clearPendingLaunch();   // ignore app.launch from CC
            osaOverlayApp->clearWantsOverlay();    // CC can't open another CC
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
        // Home is now a regular OSA script. It runs in osaApp; pendingLaunch()
        // signals a tile tap (open anim was already played via anim.openTile),
        // wantsOverlay() signals a swipe-down to Control Center.
        osaApp.update();
        if (osaApp.wantsExit) {
            osaApp.wantsExit = false;
            if (osaApp.wantsOverlay()) {
                osaApp.clearWantsOverlay();
                openControlCenter(STATE_HOMESCREEN);
                return;
            }
            String next = osaApp.pendingLaunch();
            osaApp.clearPendingLaunch();
            osaApp.recycle();
            if (next.length() > 0) {
                if (osaApp.loadScript(next)) {
                    activeApp    = &osaApp;
                    currentState = STATE_IN_APP;
                    osaApp.show();
                } else {
                    loadHomeScript();
                }
            } else {
                // Home script exited without a launch — reload it.
                loadHomeScript();
            }
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
                    loadHomeScript();
                    return;
                }
            }
        } else {
            isGlobalSwiping = false;
        }

        if (!blockApp && activeApp != nullptr) {
            activeApp->update();

            // OSA script ended (exit(), error, swipe-up, swipe-down, or app.launch):
            //   - swipe-down (wantsOverlay) → open Control Center over this app
            //   - app.launch                → chain into the requested script
            //   - everything else           → back to home
            if (activeApp == &osaApp && osaApp.wantsExit) {
                osaApp.wantsExit = false;
                isGlobalSwiping = false;

                if (osaApp.wantsOverlay()) {
                    osaApp.clearWantsOverlay();
                    openControlCenter(STATE_IN_APP);
                    return;
                }

                String next = osaApp.pendingLaunch();
                osaApp.clearPendingLaunch();
                if (next.length() > 0 && osaApp.loadScript(next)) {
                    activeApp = &osaApp;
                    currentState = STATE_IN_APP;
                    delay(50);
                    activeApp->show();
                    return;
                }
                activeApp = nullptr;
                currentState = STATE_HOMESCREEN;
                loadHomeScript();
                return;
            }
        }
    }
}