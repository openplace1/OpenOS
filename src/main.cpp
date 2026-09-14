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
#include <new>

#include "Config.h"
// Lockscreen is now an OSA script (/system/apps/lockscreen.osa) that runs in
// osaApp on boot — no native class. See LOCKSCREEN_SCRIPT below.

// Override the weak symbol defined in arduino-esp32's main.cpp.
// The default loop task gets 8 KB, which is not enough for the OSA runtime's
// recursive expression evaluator plus nested user-function calls. It ran with
// 32 KB for a long time, but that stack comes out of the same heap as
// everything else, and on this no-PSRAM board 12 KB of it is the difference
// between a TLS transfer inside OpenStore stalling and completing. Measured
// high-water mark across boot, the lock screen, Home, OpenStore (with a TLS
// handshake, which runs on this stack) and Settings: under 7.5 KB used. 20 KB
// leaves more than double that. OPENOS:PING reports the current mark.
size_t getArduinoLoopTaskStackSize() {
    return 20480;
}

// Keep a freshly installed OTA image in PENDING_VERIFY until OpenOS completes
// setup and remains alive in loop() for the probation window. This overrides
// the Arduino core's weak default, which otherwise accepts it before setup().
extern "C" bool verifyRollbackLater() {
    return true;
}

#include "Applications/Home.h"
#include "Applications/Crypto.h"
#include "Applications/Wallpaper.h"
#include "Applications/Theme.h"
#include "Applications/Toast.h"
#include "Applications/OSAApp.h"
#include "Runtime/FirmwareUpdate.h"
#include "Runtime/HeapReserve.h"
#include "Runtime/PackageManager.h"
#include "Runtime/SecureHttp.h"
#include "OpenOSVersion.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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
// Where the overlay runtime lives: inside the heap reserve when it fits
// there (an application-time heap never offers 20-30 KB in one piece), else
// wherever new found room.
static bool osaOverlayInReserve = false;

static OSAApp* createOverlayApp() {
    void* room = HeapReserve::allocate(sizeof(OSAApp), "Control Center runtime");
    if (room) {
        osaOverlayInReserve = true;
        return new (room) OSAApp(&tft, &ts);
    }
    osaOverlayInReserve = false;
    return new (std::nothrow) OSAApp(&tft, &ts);
}

static void destroyOverlayApp() {
    if (!osaOverlayApp) return;
    if (osaOverlayInReserve) {
        osaOverlayApp->~OSAApp();
        HeapReserve::deallocate(osaOverlayApp);
    } else {
        delete osaOverlayApp;
    }
    osaOverlayApp = nullptr;
    osaOverlayInReserve = false;
}
static OSAApp* g_underlyingApp = nullptr;  // activeApp captured before CC opened


bool sysWiFiEnabled = false;
int sysBrightness = 255;

// OpenOS 1.6 dropped Classic Bluetooth SPP: the stack cost ~180 KB of flash
// and 80 KB of heap while enabled, on a board with neither to spare. The
// controller's memory is handed back to the heap once at boot.
static void releaseBluetoothMemory() {
    esp_err_t result = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    Serial.printf("[BT] controller memory released: %d free=%u maxBlock=%u\n",
                  (int)result, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
uint16_t sysAccent = 0; // 0 = default blue, else RGB565 from Settings
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
                               OSARuntime::readIconColorFromFile(full, tft.color565(255, 149, 0)),
                               OSARuntime::readAppIconFromFile(full));
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
                                       entry, tft.color565(255, 149, 0)),
                                   OSARuntime::readAppIconFromFile(entry));
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

// ─── Staged firmware update ──────────────────────────────────────────────────
// ota.install() records the verified release and restarts; the download runs
// here, before any script is loaded, because that is the only moment the heap
// still offers one block large enough for a TLS session plus the flash
// writer. A failure just carries on booting — the previous firmware is intact
// and the record has already been cleared, so the device cannot loop.

static void drawStagedProgress(const char* phase, size_t completed,
                               size_t total, void* context) {
    (void)context;
    static int lastPercent = -1;
    static String lastPhase;
    int percent = total > 0 ? (int)((completed * 100U) / total) : 0;
    if (percent > 100) percent = 100;
    String nextPhase = phase ? phase : "Updating";
    bool phaseChanged = nextPhase != lastPhase;
    if (!phaseChanged && (percent == lastPercent || (percent % 2) != 0)) return;
    if (phaseChanged) {
        lastPhase = nextPhase;
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(TFT_WHITE);
        tft.drawString("Updating OpenOS", 120, 96);
        tft.setTextFont(2);
        tft.setTextColor(tft.color565(150, 150, 160));
        tft.drawString(nextPhase, 120, 128);
        tft.drawString("Keep the device powered", 120, 232);
    }
    lastPercent = percent;
    tft.fillRoundRect(20, 160, 200, 18, 7, tft.color565(45, 45, 52));
    int fill = (196 * percent) / 100;
    if (fill > 0)
        tft.fillRoundRect(22, 162, fill, 14, 6, tft.color565(52, 199, 89));
    tft.fillRect(80, 184, 80, 20, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(200, 200, 210));
    tft.drawString(String(percent) + "%", 120, 194);
}

static void runStagedFirmwareUpdate() {
    if (!FirmwareUpdate::staged()) return;
    Serial.printf("[OTA] staged update pending: %s\n",
                  FirmwareUpdate::stagedName().c_str());

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Updating OpenOS", 120, 96);
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(150, 150, 160));
    tft.drawString("Connecting to Wi-Fi", 120, 128);

    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 20000U) {
        delay(200);
        yield();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] staged update: no Wi-Fi, continuing boot");
        tft.setTextColor(tft.color565(255, 149, 0));
        tft.drawString("No Wi-Fi - update postponed", 120, 160);
        delay(2000);
        return;
    }

    Serial.printf("[OTA] staged install starting free=%u maxBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (FirmwareUpdate::installStaged(drawStagedProgress, nullptr)) {
        Serial.println("[OTA] staged install complete, restarting");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(tft.color565(52, 199, 89));
        tft.drawString("Update installed", 120, 150);
        delay(1200);
        ESP.restart();
    }
    Serial.printf("[OTA] staged install failed: %s\n",
                  FirmwareUpdate::lastError().c_str());
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(255, 69, 58));
    tft.drawString("Update failed", 120, 130);
    tft.setTextColor(tft.color565(180, 180, 190));
    tft.drawString("Starting the current version", 120, 158);
    delay(2500);
}

// ─── USB serial diagnostics ──────────────────────────────────────────────────
// Line-oriented commands on the USB serial port for a developer with the
// board on a cable; nothing here changes state on the device.
//   OPENOS:PING            firmware version and heap
//   OPENOS:FETCH [url]     run the real SecureHttp::get path from the main
//                          loop (default: the OpenStore catalog URL)
void osaPollSerialCommands() {
    static char line[80];
    static uint8_t length = 0;
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[length] = 0;
            if (strcmp(line, "OPENOS:PING") == 0) {
                Serial.printf("OPENOS:PONG %s/%d free=%u maxBlock=%u stackFree=%u\n",
                              OpenOSBuild::VERSION_NAME, OpenOSBuild::VERSION_CODE,
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            } else if (strncmp(line, "OPENOS:FETCH", 12) == 0) {
                String url = String(line + 12);
                url.trim();
                if (url.length() == 0) url = PackageManager::catalogSourceUrl();
                Serial.printf("[HTTPS] fetch %s stackFree=%u\n", url.c_str(),
                              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                HTTPClient http;
                WiFiClientSecure client;
                SecureHttp::Request request;
                request.attempts = 1;
                String why;
                int status = SecureHttp::get(http, client, url, request, why);
                Serial.printf("OPENOS:FETCH-DONE status=%d size=%d why=%s\n", status,
                              status > 0 ? http.getSize() : 0, why.c_str());
                http.end();
                client.stop();
            }
            length = 0;
        } else if (length < sizeof(line) - 1) {
            line[length++] = c;
        } else {
            length = 0;
        }
    }
}

// OpenOS <= 1.1 stored these values XOR-obfuscated with a key compiled into
// the firmware. Re-seal them with the per-device AES-GCM key the first time
// the new firmware boots so the SD card stops carrying readable secrets.
static void migrateLegacySecrets() {
    if (!isSdReady || !Crypto::ready()) return;
    static const char* keys[] = { "net_0", "passcode" };
    bool changed = false;
    for (const char* key : keys) {
        String stored = Config::get(key, "");
        if (stored.length() == 0 || Crypto::isCurrent(stored)) continue;
        String plain = Crypto::decrypt(stored);
        String sealed = Crypto::encrypt(plain);
        if (sealed.length() == 0) continue;
        Config::set(key, sealed);
        changed = true;
    }
    if (changed) {
        Config::save();
        Serial.println("[CRYPTO] re-sealed legacy secrets with the device key");
    }
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

    releaseBluetoothMemory();
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
// anim.openTile() in OSA records which tile launched; the zoom itself plays
// from main.cpp once the launch is known (playOpenAnimation), so a script
// cannot start it without actually launching anything.
void osaPlayOpenAnim(int idx) {
    if (idx < 0 || idx >= home.appCount) return;
    home.lastLaunchX     = 12 + (idx % 12 % 4) * 55;
    home.lastLaunchY     = 30 + (idx % 12 / 4) * 80;
    home.lastLaunchColor = home.tiles[idx].color;
    home.lastLaunchValid = true;
}

// The launched tile grows into the whole screen in its own colour, and that
// colour then stays under the loading script until it paints its first
// frame — the way a phone opens an app. Six eased frames, about 120 ms of
// SPI; a slower version of this once felt sluggish, which is why the tail
// of the curve is where the big fills happen and why it stops at six.
static void playOpenAnimation(int fromX, int fromY, int fromSize, uint16_t color,
                              const String& label) {
    const int frames = 6;
    for (int i = 1; i <= frames; ++i) {
        float t = (float)i / (float)frames;
        t = 1.0f - (1.0f - t) * (1.0f - t);
        int x = (int)(fromX + (0 - fromX) * t);
        int y = (int)(fromY + (0 - fromY) * t);
        int w = (int)(fromSize + (240 - fromSize) * t);
        int h = (int)(fromSize + (320 - fromSize) * t);
        int r = (int)(12 + (2 - 12) * t);
        tft.fillRoundRect(x, y, w, h, r, color);
        delay(8);
    }
    tft.fillScreen(color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(label, 120, 152);
}

// Finds the Home tile a launch came from — top level or inside a folder —
// and plays the zoom. Launches that did not come from a tile (OpenStore,
// app.launch from another script) get no animation.
static void animateLaunchFromHome(const String& scriptPath) {
    if (home.lastLaunchValid) {
        // home.osa said where the tile is (it knows the page and folder).
        String label;
        for (int i = 0; i < home.appCount && label.length() == 0; ++i) {
            const HomeTile& tile = home.tiles[i];
            if (!tile.isFolder && tile.scriptPath == scriptPath) label = tile.name;
            else if (tile.isFolder && tile.children)
                for (int j = 0; j < tile.childCount; ++j)
                    if (tile.children[j].scriptPath == scriptPath) { label = tile.children[j].name; break; }
        }
        playOpenAnimation(home.lastLaunchX, home.lastLaunchY, 46, home.lastLaunchColor, label);
        return;
    }
    for (int i = 0; i < home.appCount; ++i) {
        const HomeTile& tile = home.tiles[i];
        if (!tile.isFolder && tile.scriptPath == scriptPath) {
            home.lastLaunchX = 12 + (i % 12 % 4) * 55;
            home.lastLaunchY = 30 + (i % 12 / 4) * 80;
            home.lastLaunchColor = tile.color;
            home.lastLaunchValid = true;
            playOpenAnimation(home.lastLaunchX, home.lastLaunchY, 46, tile.color, tile.name);
            return;
        }
        if (tile.isFolder && tile.children) {
            for (int j = 0; j < tile.childCount; ++j) {
                if (tile.children[j].scriptPath == scriptPath) {
                    // The folder view lays its children out in a 3-wide grid.
                    playOpenAnimation(37 + (j % 3) * 60, 50 + (j / 3) * 72, 46,
                                      tile.children[j].color, tile.children[j].name);
                    return;
                }
            }
        }
    }
}

// The reverse of playOpenAnimation: the app's colour shrinks back into its
// tile over the Home ground colour, then Home paints itself on top and the
// real tile lands exactly where the colour ended. Only the strips uncovered
// by each step are painted, so nothing flickers. Ground is the flat
// wallpaper colour when that is what Home uses; with a bitmap wallpaper the
// theme background stands in until Home draws.
// The reverse of playOpenAnimation, in two halves around the Home reload
// so nothing waits on a bare screen: the app's last frame is covered by its
// tile colour at once (the "fold"), Home loads behind that colour, then the
// colour shrinks into the tile over the Home ground and Home paints itself
// on top, its real tile landing exactly where the colour ended. Only the
// strips each step uncovers are painted, so nothing flickers. Ground is the
// flat wallpaper colour when that is what Home uses; with a bitmap wallpaper
// the theme background stands in until Home draws.
static void closeAnimationFold() {
    if (!home.lastLaunchValid) return;
    tft.fillScreen(home.lastLaunchColor);
}

static void closeAnimationShrink() {
    if (!home.lastLaunchValid) return;
    home.lastLaunchValid = false;
    const uint16_t color = home.lastLaunchColor;
    const uint16_t ground = Wallpaper::lastColor ? Wallpaper::lastColor : Theme::bg();
    const int toX = home.lastLaunchX, toY = home.lastLaunchY, toSize = 46;
    int prevX = 0, prevY = 0, prevW = 240, prevH = 320;
    const int frames = 6;
    for (int i = 1; i <= frames; ++i) {
        float t = (float)i / (float)frames;
        t = t * t;
        int x = (int)(0 + (toX - 0) * t);
        int y = (int)(0 + (toY - 0) * t);
        int w = (int)(240 + (toSize - 240) * t);
        int h = (int)(320 + (toSize - 320) * t);
        int r = (int)(2 + (12 - 2) * t);
        if (y > prevY) tft.fillRect(prevX, prevY, prevW, y - prevY, ground);
        if (y + h < prevY + prevH) tft.fillRect(prevX, y + h, prevW, prevY + prevH - (y + h), ground);
        if (x > prevX) tft.fillRect(prevX, y, x - prevX, h, ground);
        if (x + w < prevX + prevW) tft.fillRect(x + w, y, prevX + prevW - (x + w), h, ground);
        tft.fillRect(x, y, r, r, ground);
        tft.fillRect(x + w - r, y, r, r, ground);
        tft.fillRect(x, y + h - r, r, r, ground);
        tft.fillRect(x + w - r, y + h - r, r, r, ground);
        tft.fillRoundRect(x, y, w, h, r, color);
        prevX = x; prevY = y; prevW = w; prevH = h;
        delay(8);
    }
}

// Home's card scan reads a dozen manifests and headers (~2.5 s), so it runs
// at boot and only again after something that can add or remove a script:
// a package install or removal, or a privileged script writing, deleting or
// compiling an .osa/.osac file. Those call osaHomeContentChanged().
static bool g_homeRescanNeeded = true;

void osaHomeContentChanged() {
    g_homeRescanNeeded = true;
}

// Loads /system/apps/home.osa into osaApp without drawing it; showHome()
// paints. Split so the close animation can run between the two.
static bool prepareHomeScript() {
    osaApp.recycle();
    HeapReserve::reclaim();
    // OpenStore keeps one bounded catalog String while it is open. Home never
    // needs that document, so release it before rediscovery.
    PackageManager::clearCatalog();
    if (g_homeRescanNeeded) {
        // Incremental scan: files copied/installed while OpenOS was running
        // appear now. Home::addScript deduplicates grid/folders.
        uint32_t started = millis();
        registerOsaShortcuts();
        g_homeRescanNeeded = false;
        Serial.printf("[HOME] rescan %u ms\n", (unsigned)(millis() - started));
    }
    String homePath = PackageManager::resolveSystemEntry(
        "openos.home", "/system/apps/home.osa");
    return osaApp.loadScript(homePath);
}

static void loadHomeScript();

// Fold, reload Home behind the colour, shrink, paint.
static void returnHomeFromApp() {
    closeAnimationFold();
    if (prepareHomeScript()) {
        closeAnimationShrink();
        activeApp = &osaApp;
        osaApp.show();
        return;
    }
    home.lastLaunchValid = false;
    loadHomeScript();
}

// Called on boot, after lockscreen unlock, and after every app exit.
static void loadHomeScript() {
    if (prepareHomeScript()) {
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
    if (!osaOverlayApp) osaOverlayApp = createOverlayApp();
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
            Serial.printf("[CC] runtime allocation failed need=%u free=%u maxBlock=%u\n",
                          (unsigned)sizeof(OSAApp), (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        destroyOverlayApp();
    }
}

static void closeControlCenter() {
    activeApp    = g_underlyingApp;
    g_underlyingApp = nullptr;
    currentState = previousState;
    // Free the overlay runtime so its 20-30 KB of inline arrays + loaded
    // script content goes back to the reserve (a TLS session or a sprite
    // needs every byte of it).
    destroyOverlayApp();
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
    // Long enough for a serial monitor to attach; a full second only delayed
    // every cold boot.
    delay(250);
    FirmwareUpdate::begin();

    
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
        sysAccent           = (uint16_t)Config::getInt("accent", 0);
        sysWiFiEnabled      = (Config::getInt("wifi", 0) != 0);
    }

    applySystemState();

    // The device secret must come from the hardware RNG, which is only a
    // true random source while an RF stack runs. Bring the Wi-Fi STA up for
    // the first-boot draw when the user keeps the radio off, then stop it.
    bool radioForEntropy = !sysWiFiEnabled && !Crypto::hasDeviceSecret();
    if (radioForEntropy) WiFi.mode(WIFI_STA);
    Crypto::begin();
    if (radioForEntropy) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    if (isSdReady) {
        migrateLegacySecrets();
        PackageManager::begin();
    }
    // Take the large-block reserve now, while nothing has fragmented the heap.
    HeapReserve::begin();

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

    // Before any script loads: the heap is still unbroken here, which is what
    // a 2 MB download over TLS needs.
    runStagedFirmwareUpdate();

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
    FirmwareUpdate::beginBootValidation();
}

void loop() {
    FirmwareUpdate::pollBootValidation();
    Toast::poll(&tft);
    osaPollSerialCommands();
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
            if (next.length() > 0) animateLaunchFromHome(next);
            // Swipe-up on Home means "show me everything": the reloaded Home
            // opens its drawer straight away.
            if (next.length() == 0 && osaApp.exitedBySwipe()) home.drawerRequested = true;
            osaApp.recycle();
            if (next.length() > 0) {
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
                    returnHomeFromApp();
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
                returnHomeFromApp();
                return;
            }
        }
    }
}
