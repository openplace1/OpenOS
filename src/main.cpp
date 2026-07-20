#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <sys/time.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include "BluetoothSerial.h"
#include <new>

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
#include "Runtime/PackageManager.h"
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

static bool s_bleMemoryReleaseAttempted = false;
static bool s_btSuspendedForMemory = false;
static const char* s_bluetoothError = "";

static void logBluetoothFailure(const char* message, esp_err_t error = ESP_OK) {
    s_bluetoothError = message;
    Serial.printf("[BT] %s err=%d free=%u maxBlock=%u\n", message, (int)error,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void releaseUnusedBleMemory() {
    if (s_bleMemoryReleaseAttempted) return;
    s_bleMemoryReleaseAttempted = true;
    esp_err_t result = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    Serial.printf("[BT] release unused BLE memory: %d free=%u maxBlock=%u\n",
                  (int)result, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// BluetoothSerial's bundled btStart() does not check the return value from
// esp_bt_controller_init(). On allocation failure it can spin forever waiting
// for a state change and eventually trip the watchdog. Initialize the Classic
// controller here with checked return values, then let BluetoothSerial start
// only the SPP host/profile layer.
bool osaSetBluetoothEnabled(bool enabled) {
    s_bluetoothError = "";
    if (!enabled) {
        SerialBT.end();
        sysBTEnabled = false;
        s_btSuspendedForMemory = false;
        return true;
    }

    if (sysBTEnabled &&
        esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
        return true;

    releaseUnusedBleMemory();
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        config.mode = ESP_BT_MODE_CLASSIC_BT;
        config.ble_max_conn = 0;
        esp_err_t result = esp_bt_controller_init(&config);
        if (result != ESP_OK) {
            logBluetoothFailure("Not enough RAM to initialize Bluetooth", result);
            sysBTEnabled = false;
            return false;
        }
        status = esp_bt_controller_get_status();
    }

    if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_err_t result = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
        if (result != ESP_OK) {
            esp_bt_controller_deinit();
            logBluetoothFailure("Bluetooth controller could not start", result);
            sysBTEnabled = false;
            return false;
        }
    }

    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        logBluetoothFailure("Bluetooth controller entered an invalid state");
        sysBTEnabled = false;
        return false;
    }

    if (!SerialBT.begin("OpenOS")) {
        SerialBT.end();
        logBluetoothFailure("Bluetooth serial profile could not start");
        sysBTEnabled = false;
        return false;
    }

    sysBTEnabled = true;
    s_btSuspendedForMemory = false;
    Serial.printf("[BT] enabled free=%u maxBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

// Classic BT takes roughly 80 KB on this no-PSRAM board. Keep the user's
// enabled setting, but temporarily stop the controller when an app launch
// would otherwise run out of heap. It is restarted after returning Home (or
// after closing Control Center). No Config key is changed by suspension.
static bool suspendBluetoothForMemory(const char* reason) {
    if (!sysBTEnabled || s_btSuspendedForMemory ||
        esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
        return false;

    Serial.printf("[BT] suspending for %s free=%u maxBlock=%u\n", reason,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    SerialBT.end();
    s_btSuspendedForMemory = true;
    delay(30);
    Serial.printf("[BT] suspended free=%u maxBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

// PackageManager lives in a separate translation unit. Give it a narrow hook
// to release Classic Bluetooth's large heap allocation before a TLS handshake
// without exposing the BluetoothSerial instance or changing the saved setting.
bool osaSuspendBluetoothForMemory(const char* reason) {
    return suspendBluetoothForMemory(reason ? reason : "HTTPS");
}

static void prepareMemoryForScript(const String& path) {
    if (!sysBTEnabled || s_btSuspendedForMemory ||
        esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
        return;

    size_t sourceBytes = 0;
    File source = SD.open(path);
    if (source && !source.isDirectory()) sourceBytes = (size_t)source.size();
    if (source) source.close();

    // Source text, compiler temporaries, variables and initial UI strings all
    // coexist briefly. Small apps stay connected; large apps get headroom.
    const size_t required = sourceBytes + 24U * 1024U;
    if (ESP.getFreeHeap() < required ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 12U * 1024U)
        suspendBluetoothForMemory(path.c_str());
}

static void resumeBluetoothAfterMemoryUse() {
    if (!s_btSuspendedForMemory) return;
    if (!sysBTEnabled) {
        s_btSuspendedForMemory = false;
        return;
    }
    Serial.printf("[BT] resuming free=%u maxBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!osaSetBluetoothEnabled(true))
        Serial.printf("[BT] resume failed: %s\n", s_bluetoothError);
}

const char* osaBluetoothLastError() {
    return s_bluetoothError;
}


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
static bool hasPackagedSystemReplacement(const String& fullPath) {
    struct LegacyMap { const char* path; const char* id; };
    static const LegacyMap replacements[] = {
        { "/system/apps/home.osa",          "openos.home" },
        { "/system/apps/lockscreen.osa",    "openos.lockscreen" },
        { "/system/apps/controlcenter.osa", "openos.controlcenter" },
        { "/system/apps/settings.osa",      "openos.settings" },
        { "/system/apps/files.osa",         "openos.files" },
        { "/system/apps/clock.osa",         "openos.clock" },
        { "/system/apps/calculator.osa",    "openos.calculator" },
        { "/system/apps/notes.osa",         "openos.notes" },
        { "/system/apps/compiler.osa",      "openos.compiler" },
        { "/system/apps/openstore.osa",     "openos.openstore" }
    };
    String lower = fullPath;
    lower.toLowerCase();
    if (lower.endsWith(".osac")) lower = lower.substring(0, lower.length() - 1);
    for (const LegacyMap& item : replacements) {
        if (lower != item.path) continue;

        // Hide the loose legacy tile only when the installed replacement is
        // itself a valid Home app.  A stale/incomplete package must not make
        // the working /system/apps copy disappear.
        String packageDir = String("/system/packages/") + item.id;
        OPKManifest manifest;
        String entry = PackageManager::installedEntry(item.id, true);
        return entry.length() > 0 &&
               PackageManager::readManifest(packageDir, manifest) &&
               manifest.id == item.id && manifest.scope == "system" &&
               manifest.isApp && OSARuntime::readIsAppFromFile(entry);
    }
    return false;
}

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
        bool directoryEntry = f.isDirectory();
        f.close();
        if (full.startsWith("/apps/")) { f = dir.openNextFile(); continue; }

        if (directoryEntry) {
            if (depth == 0) scanDirForScripts(full, 1);
        } else {
            String lower = full; lower.toLowerCase();
            // Pick up both raw .osa scripts and pre-compiled .osac binaries.
            bool isScript = lower.endsWith(".osa") || lower.endsWith(".osac");
            bool replaced = isScript && hasPackagedSystemReplacement(full);
            bool visible = isScript && !replaced && OSARuntime::readIsAppFromFile(full);
            if (visible) {
                int before = home.appCount;
                home.addScript(full,
                               OSARuntime::readAppNameFromFile(full),
                               OSARuntime::readIconColorFromFile(full, tft.color565(255, 149, 0)));
                if (home.appCount != before)
                    Serial.printf("[HOME] discovered '%s'\n", full.c_str());
            } else if (replaced) {
                Serial.printf("[HOME] packaged replacement for '%s'\n", full.c_str());
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
}

static void scanPackageRoot(const String& root, bool systemPackages) {
    if (home.appCount >= Home::MAX_APPS) return;
    if (!SD.exists(root.c_str())) return;
    File directory = SD.open(root);
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        return;
    }
    File child = directory.openNextFile();
    while (child && home.appCount < Home::MAX_APPS) {
        String id = child.name();
        int slash = id.lastIndexOf('/');
        if (slash >= 0) id = id.substring(slash + 1);
        bool directoryEntry = child.isDirectory();
        child.close();
        if (directoryEntry && !id.startsWith(".")) {
            OPKManifest manifest;
            String packageDir = root + "/" + id;
            if (PackageManager::readManifest(packageDir, manifest) &&
                manifest.id == id &&
                ((systemPackages && manifest.scope == "system" &&
                  PackageManager::isOfficialSystemId(id)) ||
                 (!systemPackages && manifest.scope == "user"))) {
                String entry = PackageManager::installedEntry(id, systemPackages);
                // Both package metadata and the entry itself must opt in. This
                // prevents a stale/mismatched manifest from exposing a hidden
                // system helper as a Home tile.
                if (entry.length() > 0 && manifest.isApp &&
                    OSARuntime::readIsAppFromFile(entry)) {
                    home.addScript(entry,
                                   OSARuntime::readAppNameFromFile(entry),
                                   OSARuntime::readIconColorFromFile(
                                       entry, tft.color565(255, 149, 0)));
                }
            }
        }
        child = directory.openNextFile();
    }
    directory.close();
}

static void registerOsaShortcuts() {
    if (!isSdReady) return;
    scanDirForScripts("/", 0);
    scanDirForScripts("/system/apps", 1);
    scanPackageRoot("/packages", false);
    scanPackageRoot("/system/packages", true);
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

    // OpenOS currently exposes Classic Bluetooth SPP only. Reclaim the BLE
    // controller block early, before the heap becomes fragmented by apps.
    releaseUnusedBleMemory();
    if (!osaSetBluetoothEnabled(sysBTEnabled) && isSdReady) {
        Config::setInt("bluetooth", 0);
        Config::save();
    }
}

// ─── Folder helpers (operate on HomeTile values) ────────────────────────────
static int g_folderCounter = 0;

static bool createFolderFromIcon(int idx) {
    if (idx < 0 || idx >= home.appCount) return false;
    HomeTile& victim = home.tiles[idx];
    if (victim.isFolder) return false;

    HomeTile leaf = victim;        // copy out the leaf data
    // Never reuse an existing default name after reboot or layout reload.
    String folderName;
    bool nameTaken;
    do {
        folderName = String("Folder ") + (++g_folderCounter);
        nameTaken = false;
        for (int i = 0; i < home.appCount; ++i) {
            if (home.tiles[i].isFolder && home.tiles[i].name == folderName) {
                nameTaken = true;
                break;
            }
        }
    } while (nameTaken);

    HomeTile& slot = home.tiles[idx];
    slot.freeChildren();
    slot.name       = folderName;
    slot.scriptPath = "";
    slot.color      = tft.color565(70, 70, 86);
    slot.isFolder   = true;
    slot.children   = nullptr;
    slot.childCount = 0;
    slot.childCap   = 0;
    if (!slot.addChild(leaf)) {
        slot = static_cast<HomeTile&&>(leaf);
        return false;
    }

    home.saveOrder();
    return true;
}

static bool addAppToFolderImpl(int folderIdx, int appIdx) {
    if (folderIdx < 0 || folderIdx >= home.appCount) return false;
    if (appIdx    < 0 || appIdx    >= home.appCount) return false;
    if (folderIdx == appIdx) return false;
    HomeTile& dst = home.tiles[folderIdx];
    HomeTile& src = home.tiles[appIdx];
    if (!dst.isFolder || src.isFolder) return false;
    if (!dst.addChild(src)) return false;

    // Remove src from grid (folderIdx shifts if appIdx < folderIdx — fine,
    // saveOrder rewrites positions anyway).
    src.freeChildren();
    for (int i = appIdx; i < home.appCount - 1; i++) {
        home.tiles[i] = static_cast<HomeTile&&>(home.tiles[i + 1]);
    }
    home.tiles[--home.appCount] = HomeTile();
    home.saveOrder();
    return true;
}

static bool deleteFolderAt(int idx) {
    if (idx < 0 || idx >= home.appCount) return false;
    HomeTile& f = home.tiles[idx];
    if (!f.isFolder) return false;

    // Deleting a folder must never silently discard children when the top
    // level grid is already nearly full.
    if (home.appCount - 1 + f.childCount > Home::MAX_APPS) return false;

    // Stash children so we can append them after we collapse the gap.
    HomeTile stash[16];
    int stashCount = f.childCount;
    for (int i = 0; i < stashCount; i++) stash[i] = f.children[i];
    f.freeChildren();

    for (int i = idx; i < home.appCount - 1; i++) {
        home.tiles[i] = static_cast<HomeTile&&>(home.tiles[i + 1]);
    }
    home.tiles[--home.appCount] = HomeTile();
    for (int i = 0; i < stashCount && home.appCount < Home::MAX_APPS; i++) {
        home.tiles[home.appCount++] = static_cast<HomeTile&&>(stash[i]);
    }
    home.saveOrder();
    return true;
}

// ─── extern wrappers — bridge to OSARuntime's home.* builtins ───────────────
// Runtime sees these via extern declarations and forwards OSA-side calls into
// our file-local static helpers.
bool osaMakeFolder(int idx)                    { return createFolderFromIcon(idx); }
bool osaDeleteFolder(int idx)                  { return deleteFolderAt(idx); }
bool osaAddToFolder(int folderIdx, int appIdx) { return addAppToFolderImpl(folderIdx, appIdx); }
// anim.openTile() in OSA used to play a zoom-in animation here; it now just
// records which tile launched so a future close anim could find its position.
// The animation calls themselves are gone — they didn't feel snappy enough.
void osaPlayOpenAnim(int idx) {
    if (idx < 0 || idx >= home.appCount) return;
    home.lastLaunchX     = 12 + (idx % 4) * 55 + 23;
    home.lastLaunchY     = 30 + (idx / 4) * 80 + 23;
    home.lastLaunchColor = home.tiles[idx].color;
}

// Loads /system/apps/home.osa into osaApp and shows it. Called on boot,
// after lockscreen unlock, and after every app exit.
static void loadHomeScript() {
    osaApp.recycle();
    // OpenStore keeps one bounded catalog String while it is open. Home never
    // needs that document, so release it before rediscovery and BT resume.
    PackageManager::clearCatalog();
    // App source/runtime memory has just been released, so this is the safest
    // point to restore a controller paused for a large app.
    resumeBluetoothAfterMemoryUse();
    // Incremental scan: files copied/installed while OpenOS is running appear
    // the next time Home is shown. Home::addScript deduplicates grid/folders.
    registerOsaShortcuts();
    String homePath = PackageManager::resolveSystemEntry(
        "openos.home", "/system/apps/home.osa");
    if (osaApp.loadScript(homePath)) {
        activeApp = &osaApp;
        osaApp.show();
    } else {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED); tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.drawString("home.osa missing", 120, 150);
        tft.drawString("Copy sd_content/ to SD root", 120, 175);
        activeApp = nullptr;
    }
}

static void openControlCenter(AppState returnState) {
    previousState = returnState;
    g_underlyingApp = activeApp;
    isSwipeDown = false;
    isGlobalSwiping = false;
    const size_t overlayNeed = sizeof(OSAApp) + 16U * 1024U;
    if (ESP.getFreeHeap() < overlayNeed)
        suspendBluetoothForMemory("Control Center");
    if (!osaOverlayApp) osaOverlayApp = new (std::nothrow) OSAApp(&tft, &ts);
    String controlCenterPath = PackageManager::resolveSystemEntry(
        "openos.controlcenter", "/system/apps/controlcenter.osa");
    if (osaOverlayApp && osaOverlayApp->loadScript(controlCenterPath)) {
        activeApp    = osaOverlayApp;
        currentState = STATE_CONTROLCENTER;
        activeApp->show();
    } else {
        if (osaOverlayApp) {
            Serial.printf("[CC] load failed: %s\n", osaOverlayApp->lastError().c_str());
        } else {
            Serial.printf("[CC] runtime allocation failed free=%u maxBlock=%u\n",
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        delete osaOverlayApp;
        osaOverlayApp = nullptr;
        resumeBluetoothAfterMemoryUse();
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
    resumeBluetoothAfterMemoryUse();
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
        sysBTEnabled        = (Config::getInt("bluetooth", 0) != 0);
        PackageManager::begin();
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

    // Boot into the OSA lockscreen — exit() routes to STATE_HOMESCREEN in the
    // loop below. Fall through to home if SD failed so we don't dead-screen.
    String lockscreenPath = PackageManager::resolveSystemEntry(
        "openos.lockscreen", "/system/apps/lockscreen.osa");
    if (osaApp.loadScript(lockscreenPath)) {
        osaApp.show();
        activeApp = &osaApp;
    } else {
        currentState = STATE_HOMESCREEN;
        loadHomeScript();
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
                prepareMemoryForScript(next);
                if (osaApp.loadScript(next)) {
                    activeApp    = &osaApp;
                    currentState = STATE_IN_APP;
                    osaApp.show();
                } else {
                    osaApp.showLoadError();
                    delay(1400);
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
                if (next.length() > 0) {
                    Serial.printf("[ROUTER] launch from app: '%s'\n", next.c_str());
                    prepareMemoryForScript(next);
                    if (osaApp.loadScript(next)) {
                        activeApp = &osaApp;
                        currentState = STATE_IN_APP;
                        delay(50);
                        activeApp->show();
                        return;
                    }
                    osaApp.showLoadError();
                    delay(1400);
                }
                activeApp = nullptr;
                currentState = STATE_HOMESCREEN;
                loadHomeScript();
                return;
            }
        }
    }
}
