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
#include "Applications/Lockscreen.h"

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
#include "Applications/OsaShortcut.h"
#include "Runtime/OSARuntime.h"
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

NotificationService notifyService(&tft);
OSAApp osaApp(&tft, &ts);
// Second runtime dedicated to overlays (Control Center) so loading the CC
// script doesn't trash the state of whatever app is currently underneath.
OSAApp osaOverlayApp(&tft, &ts);
static App* g_underlyingApp = nullptr;  // activeApp captured before CC opened


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

static void bootstrapExampleScript() {
    if (!isSdReady) return;

    // Retire earlier demos so they stop showing up in Files/Home.
    if (SD.exists("/example.osa"))      SD.remove("/example.osa");
    if (SD.exists("/chat_example.osa")) SD.remove("/chat_example.osa");

    // ── /tap_game.osa — tap-the-dot mini-game ────────────────────────────────
    // Always rewritten during development so iteration on the script doesn't
    // require manually deleting it from the SD card.
    do {
        const char* path = "/tap_game.osa";
        SD.remove(path);
        File f = SD.open(path, FILE_WRITE);
        if (!f) break;

        static const char SCRIPT[] PROGMEM =
        "#app \"Tap!\"\n"
        "#isApp true\n"
        "#appColor \"#FF3B30\"\n"
        "\n"
        "var score = 0\n"
        "var startMs = 0\n"
        "var tx = 120\n"
        "var ty = 180\n"
        "var tr = 30\n"
        "var lastSec = -1\n"
        "var phase = 0\n"
        "var lastTapMs = 0\n"
        "\n"
        "def newTarget()\n"
        "  tx = random(tr + 12, 240 - tr - 12)\n"
        "  ty = random(tr + 60, 300 - tr)\n"
        "end\n"
        "\n"
        "def drawScoreBar(sec)\n"
        "  setcolor(28, 28, 38)\n"
        "  rect(0, 0, 240, 40)\n"
        "  setcolor(60, 60, 80)\n"
        "  rect(0, 40, 240, 1)\n"
        "  textcolor(255, 255, 255)\n"
        "  fontsize(2)\n"
        "  text(10, 12, \"Score: \" + str(score))\n"
        "  textcolor(255, 200, 80)\n"
        "  text(150, 12, \"Time: \" + str(sec))\n"
        "end\n"
        "\n"
        "def drawTarget()\n"
        "  setcolor(255, 59, 48)\n"
        "  circle(tx, ty, tr)\n"
        "  setcolor(255, 255, 255)\n"
        "  ring(tx, ty, tr)\n"
        "  setcolor(255, 255, 255)\n"
        "  circle(tx, ty, 5)\n"
        "end\n"
        "\n"
        "def eraseTarget()\n"
        "  setcolor(15, 15, 25)\n"
        "  circle(tx, ty, tr + 4)\n"
        "end\n"
        "\n"
        "def flashHit()\n"
        "  setcolor(255, 255, 255)\n"
        "  circle(tx, ty, tr)\n"
        "  wait(45)\n"
        "  setcolor(120, 255, 120)\n"
        "  circle(tx, ty, tr)\n"
        "  wait(35)\n"
        "end\n"
        "\n"
        "def showIntro()\n"
        "  cls()\n"
        "  bg(15, 15, 25)\n"
        "  textcolor(255, 80, 80)\n"
        "  fontsize(7)\n"
        "  textc(120, 70, \"Tap!\")\n"
        "  textcolor(255, 255, 255)\n"
        "  fontsize(2)\n"
        "  textc(120, 135, \"How many red dots\")\n"
        "  textc(120, 155, \"can you tap in 30s?\")\n"
        "  setcolor(255, 59, 48)\n"
        "  circle(120, 200, 22)\n"
        "  setcolor(255, 255, 255)\n"
        "  ring(120, 200, 22)\n"
        "  circle(120, 200, 4)\n"
        "  textcolor(120, 255, 120)\n"
        "  fontsize(2)\n"
        "  textc(120, 250, \"Tap anywhere to start\")\n"
        "  textcolor(160, 160, 180)\n"
        "  fontsize(1)\n"
        "  textc(120, 300, \"Hold bottom-left to exit\")\n"
        "end\n"
        "\n"
        "def showGameOver()\n"
        "  cls()\n"
        "  bg(15, 15, 25)\n"
        "  textcolor(255, 200, 80)\n"
        "  fontsize(4)\n"
        "  textc(120, 60, \"Time's up!\")\n"
        "  textcolor(255, 255, 255)\n"
        "  fontsize(2)\n"
        "  textc(120, 120, \"Final score\")\n"
        "  textcolor(120, 255, 120)\n"
        "  fontsize(7)\n"
        "  textc(120, 175, str(score))\n"
        "  textcolor(180, 180, 200)\n"
        "  fontsize(2)\n"
        "  textc(120, 240, \"Tap to play again\")\n"
        "  textcolor(160, 160, 180)\n"
        "  fontsize(1)\n"
        "  textc(120, 295, \"Hold bottom-left to exit\")\n"
        "end\n"
        "\n"
        "def waitTapRelease()\n"
        "  while touch.down() == 1 do wait(20) end\n"
        "end\n"
        "\n"
        "def waitTapPress()\n"
        "  while touch.down() == 0 do wait(30) end\n"
        "end\n"
        "\n"
        "def checkExit()\n"
        "  var px = touch.x()\n"
        "  var py = touch.y()\n"
        "  if py > 280 and px < 60 and px >= 0 then exit() end\n"
        "end\n"
        "\n"
        "def startGame()\n"
        "  cls()\n"
        "  bg(15, 15, 25)\n"
        "  score = 0\n"
        "  startMs = millis()\n"
        "  lastSec = 30\n"
        "  drawScoreBar(30)\n"
        "  newTarget()\n"
        "  drawTarget()\n"
        "end\n"
        "\n"
        "# Wait for the launching tap (from the home screen) to be released so it\n"
        "# doesn't immediately count as the \"tap to start\" press on the intro.\n"
        "while touch.down() == 1 do wait(20) end\n"
        "wait(150)\n"
        "\n"
        "showIntro()\n"
        "\n"
        "loop\n"
        "  if phase == 0 then\n"
        "    if touch.down() == 1 then\n"
        "      checkExit()\n"
        "      waitTapRelease()\n"
        "      startGame()\n"
        "      phase = 1\n"
        "    end\n"
        "    wait(30)\n"
        "    continue\n"
        "  end\n"
        "\n"
        "  if phase == 2 then\n"
        "    if touch.down() == 1 then\n"
        "      checkExit()\n"
        "      waitTapRelease()\n"
        "      showIntro()\n"
        "      phase = 0\n"
        "    end\n"
        "    wait(30)\n"
        "    continue\n"
        "  end\n"
        "\n"
        "  # phase 1 — active gameplay\n"
        "  var elapsed = (millis() - startMs) / 1000\n"
        "  var remain  = 30 - elapsed\n"
        "  if remain < 0 then remain = 0 end\n"
        "  if remain != lastSec then\n"
        "    drawScoreBar(remain)\n"
        "    drawTarget()\n"
        "    lastSec = remain\n"
        "  end\n"
        "\n"
        "  if remain == 0 then\n"
        "    showGameOver()\n"
        "    phase = 2\n"
        "    wait(400)\n"
        "    continue\n"
        "  end\n"
        "\n"
        "  if touch.down() == 1 then\n"
        "    var px = touch.x()\n"
        "    var py = touch.y()\n"
        "    if px < 0 or py < 0 then\n"
        "      wait(15)\n"
        "      continue\n"
        "    end\n"
        "    # Diagnostic marker — yellow dot at the reported touch position.\n"
        "    # Remove later once we know touch coords are correct.\n"
        "    setcolor(255, 255, 0)\n"
        "    circle(px, py, 3)\n"
        "    if py > 290 and px < 60 then exit() end\n"
        "    var dx = px - tx\n"
        "    var dy = py - ty\n"
        "    # Very generous hit radius — touchscreen is not pixel-perfect and\n"
        "    # mapping can drift a bit at the edges.\n"
        "    var hitR = tr + 22\n"
        "    if dx * dx + dy * dy < hitR * hitR then\n"
        "      flashHit()\n"
        "      eraseTarget()\n"
        "      score = score + 1\n"
        "      newTarget()\n"
        "      drawScoreBar(remain)\n"
        "      drawTarget()\n"
        "      waitTapRelease()\n"
        "    end\n"
        "  end\n"
        "\n"
        "  wait(20)\n"
        "end\n";

        f.print((const __FlashStringHelper*)SCRIPT);
        f.close();
    } while (0);
}

// ─── Built-in exception (privileged) apps ────────────────────────────────────
// These ship with the firmware as .osa scripts that get force-installed on the
// SD card at boot. They carry `#exception true` so they can use the privileged
// SDK (sys.*, cfg.*, fs.*, ntp.sync, sys.reboot). Add an entry to install
// another one — keep the script under OSA_MAX_LINES (256).
struct BuiltinException {
    const char* path;       // SD path, e.g. "/system/exceptions/sys_demo.osa"
    const char* script;     // raw script body (PROGMEM-friendly C string)
};

const char SETTINGS_SCRIPT[] PROGMEM =
    "#app \"Settings\"\n"
    "#isApp true\n"
    "#appColor \"#8E8E93\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "def waitTap()\n"
    "  while touch.down() == 0 do wait(40) end\n"
    "  while touch.down() == 1 do wait(20) end\n"
    "end\n"
    "\n"
    "def displayScreen()\n"
    "  while 1 == 1 do\n"
    "    var t = theme()\n"
    "    var modeLbl = \"Light\"\n"
    "    if t == 1 then modeLbl = \"Dark\" end\n"
    "    ui.menuStart(\"Display\", 1)\n"
    "    ui.menuRow(\"Theme\",      \"T\", 142, 142, 147, modeLbl + \" >\")\n"
    "    ui.menuRow(\"Brightness\", \"B\", 255, 149,   0, str(getbright()) + \" >\")\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick == 0 then\n"
    "      var nt = ui.segmented(\"Theme\", \"Light|Dark\", t)\n"
    "      if nt >= 0 then sys.theme(nt) end\n"
    "    end\n"
    "    if pick == 1 then\n"
    "      var b = ui.slider(\"Brightness\", 10, 255, getbright())\n"
    "      if b >= 0 then sys.brightness(b) end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def wifiConnect()\n"
    "  ui.alert(\"Wi-Fi\", \"Scanning networks...\")\n"
    "  var n = wifi.scan()\n"
    "  if n == 0 then ui.alert(\"Wi-Fi\", \"No networks found\") return end\n"
    "  var list = \"\"\n"
    "  var i = 0\n"
    "  while i < n do\n"
    "    if i > 0 then list = list + \"|\" end\n"
    "    var lock = \" \"\n"
    "    if wifi.scanSecure(i) == 1 then lock = \"*\" end\n"
    "    list = list + lock + wifi.scanSsid(i) + \"  \" + str(wifi.scanRssi(i))\n"
    "    i = i + 1\n"
    "  end\n"
    "  var pick = ui.menu(list, \"Select Wi-Fi\")\n"
    "  if pick < 0 then return end\n"
    "  var ssid = wifi.scanSsid(pick)\n"
    "  var pwd = \"\"\n"
    "  if wifi.scanSecure(pick) == 1 then pwd = input(\"Password for \" + ssid, \"\") end\n"
    "  ui.alert(\"Wi-Fi\", \"Connecting to \" + ssid)\n"
    "  var ok = wifi.connect(ssid, pwd)\n"
    "  if ok == 1 then\n"
    "    wifi.save(ssid, pwd)\n"
    "    ui.alert(\"Wi-Fi\", \"Connected: \" + wifi.ip())\n"
    "  end\n"
    "  if ok == 0 then ui.alert(\"Wi-Fi\", \"Connection failed\") end\n"
    "end\n"
    "\n"
    "def wifiScreen()\n"
    "  while 1 == 1 do\n"
    "    var st = \"OFF\"\n"
    "    if wifi.isEnabled() == 1 then st = \"ON\" end\n"
    "    if wifi.connected() == 1 then st = wifi.ssid() end\n"
    "    ui.menuStart(\"Wi-Fi\", 1)\n"
    "    ui.menuRow(\"Status\",         \"i\", 142, 142, 147, st)\n"
    "    ui.menuRow(\"Toggle Wi-Fi\",   \"T\",   0, 122, 255, \" \")\n"
    "    ui.menuRow(\"Scan & Connect\", \"S\",   0, 122, 255, \">\")\n"
    "    ui.menuRow(\"Disconnect\",     \"D\", 255,  59,  48, \" \")\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick == 1 then\n"
    "      var t = ui.toggle(\"Wi-Fi enabled\", wifi.isEnabled())\n"
    "      if t == 1 then wifi.enable() end\n"
    "      if t == 0 then wifi.disable() end\n"
    "    end\n"
    "    if pick == 2 then\n"
    "      if wifi.isEnabled() == 0 then wifi.enable() end\n"
    "      wifiConnect()\n"
    "    end\n"
    "    if pick == 3 then wifi.disconnect() ui.alert(\"Wi-Fi\", \"Disconnected\") end\n"
    "  end\n"
    "end\n"
    "\n"
    "def btScreen()\n"
    "  var t = ui.toggle(\"Bluetooth (OpenOS)\", bt.enabled())\n"
    "  if t == 1 then bt.enable() sys.notify(\"BT enabled\") end\n"
    "  if t == 0 then bt.disable() sys.notify(\"BT disabled\") end\n"
    "end\n"
    "\n"
    "def passcodeScreen()\n"
    "  while 1 == 1 do\n"
    "    var cur = crypto.decrypt(cfg.get(\"passcode\", \"\"))\n"
    "    var st = \"OFF\"\n"
    "    if len(cur) > 0 then st = \"ON\" end\n"
    "    ui.menuStart(\"Passcode\", 1)\n"
    "    ui.menuRow(\"Status\",  \"i\", 142, 142, 147, st)\n"
    "    ui.menuRow(\"Set new\", \"S\",   0, 122, 255, \">\")\n"
    "    ui.menuRow(\"Disable\", \"D\", 255,  59,  48, \" \")\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick == 1 then\n"
    "      var p1 = ui.numpad(\"New passcode\", 6)\n"
    "      if len(p1) > 0 then\n"
    "        var p2 = ui.numpad(\"Confirm passcode\", 6)\n"
    "        if p1 == p2 then\n"
    "          cfg.set(\"passcode\", crypto.encrypt(p1))\n"
    "          ui.alert(\"Passcode\", \"Saved\")\n"
    "        end\n"
    "        if p1 != p2 then ui.alert(\"Passcode\", \"Mismatch\") end\n"
    "      end\n"
    "    end\n"
    "    if pick == 2 then\n"
    "      if confirm(\"Disable passcode?\", \"Lockscreen will skip prompt\") == 1 then\n"
    "        cfg.set(\"passcode\", \"\")\n"
    "        ui.alert(\"Passcode\", \"Disabled\")\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def wallpapersScreen()\n"
    "  var page = 0\n"
    "  while 1 == 1 do\n"
    "    cls()\n"
    "    ui.backHeader(\"Wallpapers\")\n"
    "    var list = fs.list(\"/system/assets/wallpapers\")\n"
    "    var n = 0\n"
    "    if len(list) > 0 then\n"
    "      n = 1\n"
    "      var i = 0\n"
    "      while i < len(list) do\n"
    "        if substr(list, i, 1) == \"|\" then n = n + 1 end\n"
    "        i = i + 1\n"
    "      end\n"
    "    end\n"
    "    var enabled = cfg.get(\"wallpaper\", \"1\")\n"
    "    # Toggle row\n"
    "    setcolor(40, 40, 50)\n"
    "    rect(0, 55, 240, 45)\n"
    "    textcolor(255, 255, 255)\n"
    "    fontsize(2)\n"
    "    text(15, 77, \"Wallpaper\")\n"
    "    var stLbl = \"OFF\"\n"
    "    if enabled == \"1\" then stLbl = \"ON\" end\n"
    "    textcolor(255, 200, 80)\n"
    "    text(180, 77, stLbl)\n"
    "    if enabled != \"1\" then\n"
    "      textcolor(180, 180, 200)\n"
    "      textc(120, 200, \"Wallpaper disabled\")\n"
    "      textc(120, 220, \"Tap toggle to enable\")\n"
    "    end\n"
    "    if enabled == \"1\" then\n"
    "      if n == 0 then\n"
    "        textcolor(180, 180, 200)\n"
    "        textc(120, 175, \"No .bmp files in\")\n"
    "        textc(120, 195, \"/system/assets/wallpapers/\")\n"
    "      end\n"
    "      if n > 0 then\n"
    "        var startIdx = page * 4\n"
    "        var endIdx = startIdx + 4\n"
    "        if endIdx > n then endIdx = n end\n"
    "        var idx = startIdx\n"
    "        while idx < endIdx do\n"
    "          var localIdx = idx - startIdx\n"
    "          var col = localIdx % 2\n"
    "          var row = localIdx / 2\n"
    "          var tx = 26 + col * 107\n"
    "          var ty = 108 + row * 105\n"
    "          var name = split(list, \"|\", idx)\n"
    "          bmp.thumb(\"/system/assets/wallpapers/\" + name, tx, ty, 80, 90)\n"
    "          idx = idx + 1\n"
    "        end\n"
    "        # Pagination\n"
    "        textcolor(120, 200, 255)\n"
    "        fontsize(2)\n"
    "        if page > 0 then text(20, 305, \"< Prev\") end\n"
    "        textcolor(180, 180, 200)\n"
    "        textc(120, 305, \"Page \" + str(page + 1))\n"
    "        textcolor(120, 200, 255)\n"
    "        if (page + 1) * 4 < n then text(180, 305, \"Next >\") end\n"
    "      end\n"
    "    end\n"
    "    # Wait tap\n"
    "    while touch.down() == 0 do wait(40) end\n"
    "    var px = touch.x()\n"
    "    var py = touch.y()\n"
    "    while touch.down() == 1 do wait(20) end\n"
    "    if px < 0 or py < 0 then continue end\n"
    "    if py < 50 and px < 80 then return end\n"
    "    if py >= 55 and py < 100 then\n"
    "      if enabled == \"1\" then cfg.set(\"wallpaper\", \"0\") end\n"
    "      if enabled != \"1\" then cfg.set(\"wallpaper\", \"1\") end\n"
    "      continue\n"
    "    end\n"
    "    if enabled != \"1\" then continue end\n"
    "    if n == 0 then continue end\n"
    "    if py >= 295 then\n"
    "      if px < 100 and page > 0 then page = page - 1 end\n"
    "      if px > 140 and (page + 1) * 4 < n then page = page + 1 end\n"
    "      continue\n"
    "    end\n"
    "    if py >= 108 and py < 295 then\n"
    "      var col = 0\n"
    "      if px > 130 then col = 1 end\n"
    "      var row = 0\n"
    "      if py >= 213 then row = 1 end\n"
    "      var localIdx = row * 2 + col\n"
    "      var idx = page * 4 + localIdx\n"
    "      if idx < n then\n"
    "        var name = split(list, \"|\", idx)\n"
    "        sys.wallpaper(\"/system/assets/wallpapers/\" + name)\n"
    "        ui.alert(\"Wallpaper\", \"Set to \" + name)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def timeScreen()\n"
    "  while 1 == 1 do\n"
    "    var clock = padleft(str(time.hour()), 2, \"0\") + \":\" + padleft(str(time.min()), 2, \"0\")\n"
    "    var date  = str(time.day()) + \"/\" + str(time.month()) + \"/\" + str(time.year())\n"
    "    var ntpSt = \"never\"\n"
    "    if time.synced() == 1 then ntpSt = \"OK\" end\n"
    "    ui.menuStart(\"Time & Date\", 1)\n"
    "    ui.menuRow(\"Time\",       \"C\", 255, 149,   0, clock)\n"
    "    ui.menuRow(\"Date\",       \"D\", 142, 142, 147, date)\n"
    "    ui.menuRow(\"Set time\",   \"S\",   0, 122, 255, \">\")\n"
    "    ui.menuRow(\"Set date\",   \"T\",   0, 122, 255, \">\")\n"
    "    ui.menuRow(\"NTP status\", \"N\", 142, 142, 147, ntpSt)\n"
    "    ui.menuRow(\"Sync NTP\",   \"Y\",   0, 122, 255, \">\")\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick == 2 then\n"
    "      var h = ui.slider(\"Hour\", 0, 23, time.hour())\n"
    "      if h >= 0 then\n"
    "        var m = ui.slider(\"Minute\", 0, 59, time.min())\n"
    "        if m >= 0 then\n"
    "          sys.setTime(h, m, 0, time.day(), time.month(), time.year())\n"
    "          ui.alert(\"Time\", \"Set \" + padleft(str(h),2,\"0\") + \":\" + padleft(str(m),2,\"0\"))\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "    if pick == 3 then\n"
    "      var dd = ui.slider(\"Day\", 1, 31, time.day())\n"
    "      if dd >= 0 then\n"
    "        var mo = ui.slider(\"Month\", 1, 12, time.month())\n"
    "        if mo >= 0 then\n"
    "          var yy = ui.slider(\"Year\", 2024, 2099, time.year())\n"
    "          if yy >= 0 then\n"
    "            sys.setTime(time.hour(), time.min(), time.sec(), dd, mo, yy)\n"
    "            ui.alert(\"Date\", str(dd) + \"/\" + str(mo) + \"/\" + str(yy))\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "    if pick == 5 then\n"
    "      if wifi.connected() == 0 then ui.alert(\"NTP\", \"Wi-Fi not connected\") end\n"
    "      if wifi.connected() == 1 then\n"
    "        var ok = ntp.sync()\n"
    "        if ok == 1 then ui.alert(\"NTP\", \"Time synchronised\") end\n"
    "        if ok == 0 then ui.alert(\"NTP\", \"Sync failed\") end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def sdcardScreen()\n"
    "  while 1 == 1 do\n"
    "    ui.menuStart(\"SD Card\", 1)\n"
    "    var rdy = \"no\"\n"
    "    if sdready() == 1 then rdy = \"yes\" end\n"
    "    ui.menuRow(\"Status\",  \"i\", 142, 142, 147, rdy)\n"
    "    ui.menuRow(\"Free RAM\",\"R\", 142, 142, 147, str(int(freeram()/1024)) + \" KB\")\n"
    "    ui.menuRow(\"Uptime\",  \"U\", 142, 142, 147, str(int(uptime()/60)) + \" min\")\n"
    "    ui.menuRow(\"Wipe SD card\", \"!\", 255, 59, 48, \">\")\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick == 3 then\n"
    "      if confirm(\"Wipe SD card?\", \"Erases user files\") == 1 then\n"
    "        fs.wipe(\"/\")\n"
    "        ui.alert(\"SD Card\", \"Wiped. Reboot recommended.\")\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def appDetail(idx)\n"
    "  while 1 == 1 do\n"
    "    var name = apps.name(idx)\n"
    "    var anyReq = 0\n"
    "    if apps.needsPerm(idx, 1) == 1 then anyReq = 1 end\n"
    "    if apps.needsPerm(idx, 2) == 1 then anyReq = 1 end\n"
    "    if apps.needsPerm(idx, 4) == 1 then anyReq = 1 end\n"
    "    ui.menuStart(name, 1)\n"
    "    var rowMap = \"\"\n"
    "    if anyReq == 0 or apps.needsPerm(idx, 1) == 1 then\n"
    "      var st = \"OFF\"\n"
    "      if apps.hasPerm(idx, 1) == 1 then st = \"ON\" end\n"
    "      ui.menuRow(\"Notifications\", \"N\", 0, 122, 255, st)\n"
    "      rowMap = rowMap + \"1|\"\n"
    "    end\n"
    "    if anyReq == 0 or apps.needsPerm(idx, 2) == 1 then\n"
    "      var st = \"OFF\"\n"
    "      if apps.hasPerm(idx, 2) == 1 then st = \"ON\" end\n"
    "      ui.menuRow(\"Network\", \"W\", 52, 199, 89, st)\n"
    "      rowMap = rowMap + \"2|\"\n"
    "    end\n"
    "    if anyReq == 0 or apps.needsPerm(idx, 4) == 1 then\n"
    "      var st = \"OFF\"\n"
    "      if apps.hasPerm(idx, 4) == 1 then st = \"ON\" end\n"
    "      ui.menuRow(\"System Settings\", \"S\", 255, 149, 0, st)\n"
    "      rowMap = rowMap + \"4|\"\n"
    "    end\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    var pickBit = split(rowMap, \"|\", pick)\n"
    "    if pickBit == \"\" then return end\n"
    "    apps.togglePerm(idx, num(pickBit))\n"
    "  end\n"
    "end\n"
    "\n"
    "def applicationsScreen()\n"
    "  var n = apps.scan()\n"
    "  if n == 0 then\n"
    "    ui.alert(\"Applications\", \"No .osa apps on SD\")\n"
    "    return\n"
    "  end\n"
    "  while 1 == 1 do\n"
    "    n = apps.scan()\n"
    "    ui.menuStart(\"Applications\", 1)\n"
    "    var i = 0\n"
    "    while i < n do\n"
    "      var grantCount = 0\n"
    "      if apps.hasPerm(i, 1) == 1 then grantCount = grantCount + 1 end\n"
    "      if apps.hasPerm(i, 2) == 1 then grantCount = grantCount + 1 end\n"
    "      if apps.hasPerm(i, 4) == 1 then grantCount = grantCount + 1 end\n"
    "      ui.menuRow(apps.name(i), \"A\", 255, 149, 0, str(grantCount) + \" perm >\")\n"
    "      i = i + 1\n"
    "    end\n"
    "    var pick = ui.menuShow()\n"
    "    if pick == -1 then return end\n"
    "    if pick < n then appDetail(pick) end\n"
    "  end\n"
    "end\n"
    "\n"
    "def aboutScreen()\n"
    "  cls()\n"
    "  ui.backHeader(\"About Device\")\n"
    "  # Info card\n"
    "  setcolor(40, 40, 50)\n"
    "  rrect(10, 70, 220, 80, 10)\n"
    "  setcolor(0, 122, 255)\n"
    "  rrect(20, 90, 40, 40, 8)\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(4)\n"
    "  textc(40, 110, \"OS\")\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(2)\n"
    "  text(75, 100, \"OpenOS 1.0a\")\n"
    "  textcolor(180, 180, 200)\n"
    "  text(75, 120, \"Privileged (OSA)\")\n"
    "  # Stats card\n"
    "  setcolor(40, 40, 50)\n"
    "  rrect(10, 165, 220, 110, 10)\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(2)\n"
    "  text(20, 185, \"CPU:\")\n"
    "  text(20, 215, \"Free RAM:\")\n"
    "  text(20, 245, \"Uptime:\")\n"
    "  textcolor(180, 180, 200)\n"
    "  textcolor(180, 180, 200)\n"
    "  textcolor(180, 180, 200)\n"
    "  text(220 - len(\"240 MHz\") * 6, 185, \"240 MHz\")\n"
    "  var ramTxt = str(int(freeram() / 1024)) + \" KB\"\n"
    "  text(220 - len(ramTxt) * 6, 215, ramTxt)\n"
    "  var upTxt = str(int(uptime() / 60)) + \" min\"\n"
    "  text(220 - len(upTxt) * 6, 245, upTxt)\n"
    "  while 1 == 1 do\n"
    "    if touch.down() == 1 and ui.backTapped() == 1 then\n"
    "      while touch.down() == 1 do wait(20) end\n"
    "      return\n"
    "    end\n"
    "    wait(40)\n"
    "  end\n"
    "end\n"
    "\n"
    "loop\n"
    "  ui.menuStart(\"Settings\")\n"
    "  var wifiVal = \"OFF >\"\n"
    "  if wifi.isEnabled() == 1 then wifiVal = \"ON >\" end\n"
    "  var btVal = \"OFF >\"\n"
    "  if bt.enabled() == 1 then btVal = \"ON >\" end\n"
    "  ui.menuRow(\"Wi-Fi\",                \"W\",   0, 122, 255, wifiVal)\n"
    "  ui.menuRow(\"Bluetooth\",            \"B\",   0, 122, 255, btVal)\n"
    "  ui.menuRow(\"Display & Brightness\", \"D\", 142, 142, 147, \">\")\n"
    "  ui.menuRow(\"Passcode\",             \"P\", 255,  45,  85, \"OFF >\")\n"
    "  ui.menuRow(\"Wallpapers\",           \"W\", 175,  82, 222, \">\")\n"
    "  ui.menuRow(\"Time & Date\",          \"T\", 255, 149,   0, \">\")\n"
    "  ui.menuRow(\"SD Card\",              \"SD\", 52, 199,  89, \">\")\n"
    "  ui.menuRow(\"Applications\",         \"A\", 255, 149,   0, \">\")\n"
    "  ui.menuRow(\"About\",                \"i\", 142, 142, 147, \">\")\n"
    "  var pick = ui.menuShow()\n"
    "  if pick == -1 then exit() end\n"
    "  if pick == 0 then wifiScreen() end\n"
    "  if pick == 1 then btScreen() end\n"
    "  if pick == 2 then displayScreen() end\n"
    "  if pick == 3 then passcodeScreen() end\n"
    "  if pick == 4 then wallpapersScreen() end\n"
    "  if pick == 5 then timeScreen() end\n"
    "  if pick == 6 then sdcardScreen() end\n"
    "  if pick == 7 then applicationsScreen() end\n"
    "  if pick == 8 then aboutScreen() end\n"
    "end\n";

// ─── Clock ───────────────────────────────────────────────────────────────────
const char CLOCK_SCRIPT[] PROGMEM =
    "#app \"Clock\"\n"
    "#isApp true\n"
    "#appColor \"#5856D6\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "var lastSec = -1\n"
    "\n"
    "def drawClock()\n"
    "  cls()\n"
    "  bg(15, 15, 25)\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(7)\n"
    "  var hh = padleft(str(time.hour()), 2, \"0\")\n"
    "  var mm = padleft(str(time.min()), 2, \"0\")\n"
    "  textc(120, 110, hh + \":\" + mm)\n"
    "  fontsize(4)\n"
    "  textcolor(180, 180, 200)\n"
    "  var ss = padleft(str(time.sec()), 2, \"0\")\n"
    "  textc(120, 170, ss)\n"
    "  fontsize(2)\n"
    "  textcolor(220, 220, 230)\n"
    "  textc(120, 220, str(time.day()) + \"/\" + str(time.month()) + \"/\" + str(time.year()))\n"
    "  if time.synced() == 1 then\n"
    "    textcolor(120, 255, 120)\n"
    "    textc(120, 260, \"NTP synced\")\n"
    "  end\n"
    "  if time.synced() == 0 then\n"
    "    textcolor(255, 200, 80)\n"
    "    textc(120, 260, \"NTP not synced\")\n"
    "  end\n"
    "end\n"
    "\n"
    "drawClock()\n"
    "\n"
    "loop\n"
    "  var s = time.sec()\n"
    "  if s != lastSec then\n"
    "    drawClock()\n"
    "    lastSec = s\n"
    "  end\n"
    "  wait(200)\n"
    "end\n";

// ─── Calculator ──────────────────────────────────────────────────────────────
// Each drawBtn() lives on its own line — OSA only executes the first statement
// per line. Pad fits within y=110..278 so the swipe-up-to-home zone (y>290)
// doesn't eat the bottom row.
const char CALCULATOR_SCRIPT[] PROGMEM =
    "#app \"Calc\"\n"
    "#isApp true\n"
    "#appColor \"#FF9500\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "var dispBuf = \"0\"\n"
    "var prevVal = 0\n"
    "var pendOp = \"\"\n"
    "var justComputed = 0\n"
    "\n"
    "def applyOp(a, b, op)\n"
    "  if op == \"+\" then return a + b end\n"
    "  if op == \"-\" then return a - b end\n"
    "  if op == \"*\" then return a * b end\n"
    "  if op == \"/\" then\n"
    "    if b == 0 then return 0 end\n"
    "    return a / b\n"
    "  end\n"
    "  if op == \"%\" then return a - floor(a / b) * b end\n"
    "  return b\n"
    "end\n"
    "\n"
    "def doOp(newOp)\n"
    "  var v = num(dispBuf)\n"
    "  if len(pendOp) > 0 then\n"
    "    prevVal = applyOp(prevVal, v, pendOp)\n"
    "    dispBuf = str(prevVal)\n"
    "  end\n"
    "  if len(pendOp) == 0 then prevVal = v end\n"
    "  pendOp = newOp\n"
    "  justComputed = 1\n"
    "end\n"
    "\n"
    "def doDigit(d)\n"
    "  if justComputed == 1 then dispBuf = \"\" end\n"
    "  justComputed = 0\n"
    "  if dispBuf == \"0\" then dispBuf = \"\" end\n"
    "  if len(dispBuf) < 12 then dispBuf = dispBuf + d end\n"
    "end\n"
    "\n"
    "def doDot()\n"
    "  if justComputed == 1 then\n"
    "    dispBuf = \"0\"\n"
    "    justComputed = 0\n"
    "  end\n"
    "  if contains(dispBuf, \".\") == 0 then dispBuf = dispBuf + \".\" end\n"
    "end\n"
    "\n"
    "def doClear()\n"
    "  dispBuf = \"0\"\n"
    "  prevVal = 0\n"
    "  pendOp = \"\"\n"
    "  justComputed = 0\n"
    "end\n"
    "\n"
    "def doBack()\n"
    "  if len(dispBuf) > 1 then dispBuf = substr(dispBuf, 0, len(dispBuf) - 1) end\n"
    "  if len(dispBuf) == 0 then dispBuf = \"0\" end\n"
    "end\n"
    "\n"
    "def drawDisplay()\n"
    "  setcolor(15, 15, 25)\n"
    "  rect(0, 0, 240, 105)\n"
    "  textcolor(255, 200, 80)\n"
    "  fontsize(2)\n"
    "  var lhs = \"\"\n"
    "  if len(pendOp) > 0 then lhs = str(prevVal) + \" \" + pendOp end\n"
    "  text(15, 12, lhs)\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(4)\n"
    "  var shown = dispBuf\n"
    "  if len(shown) > 16 then shown = substr(shown, len(shown) - 16, 16) end\n"
    "  text(15, 55, shown)\n"
    "end\n"
    "\n"
    "def drawBtn(c, r, label, kind)\n"
    "  var x = c * 60\n"
    "  var y = 110 + r * 34\n"
    "  if kind == 0 then setcolor(55, 55, 70) end\n"
    "  if kind == 1 then setcolor(255, 149, 0) end\n"
    "  if kind == 2 then setcolor(120, 50, 50) end\n"
    "  rrect(x + 2, y + 2, 56, 30, 7)\n"
    "  textcolor(255, 255, 255)\n"
    "  fontsize(4)\n"
    "  textc(x + 30, y + 17, label)\n"
    "end\n"
    "\n"
    "def drawPad()\n"
    "  drawBtn(0, 0, \"C\", 2)\n"
    "  drawBtn(1, 0, \"<\", 2)\n"
    "  drawBtn(2, 0, \"%\", 1)\n"
    "  drawBtn(3, 0, \"/\", 1)\n"
    "  drawBtn(0, 1, \"7\", 0)\n"
    "  drawBtn(1, 1, \"8\", 0)\n"
    "  drawBtn(2, 1, \"9\", 0)\n"
    "  drawBtn(3, 1, \"*\", 1)\n"
    "  drawBtn(0, 2, \"4\", 0)\n"
    "  drawBtn(1, 2, \"5\", 0)\n"
    "  drawBtn(2, 2, \"6\", 0)\n"
    "  drawBtn(3, 2, \"-\", 1)\n"
    "  drawBtn(0, 3, \"1\", 0)\n"
    "  drawBtn(1, 3, \"2\", 0)\n"
    "  drawBtn(2, 3, \"3\", 0)\n"
    "  drawBtn(3, 3, \"+\", 1)\n"
    "  drawBtn(0, 4, \"0\", 0)\n"
    "  drawBtn(1, 4, \".\", 0)\n"
    "  drawBtn(2, 4, \"=\", 1)\n"
    "  drawBtn(3, 4, \"=\", 1)\n"
    "end\n"
    "\n"
    "def drawAll()\n"
    "  cls()\n"
    "  bg(15, 15, 25)\n"
    "  drawDisplay()\n"
    "  drawPad()\n"
    "end\n"
    "\n"
    "drawAll()\n"
    "\n"
    "loop\n"
    "  while touch.down() == 0 do wait(30) end\n"
    "  var px = touch.x()\n"
    "  var py = touch.y()\n"
    "  while touch.down() == 1 do wait(20) end\n"
    "  if px < 0 or py < 0 then continue end\n"
    "  if py < 110 or py > 285 then continue end\n"
    "  # OSA division yields a double — round down to int for index math.\n"
    "  var col = int(px / 60)\n"
    "  var row = int((py - 110) / 34)\n"
    "  if col < 0 or col > 3 then continue end\n"
    "  if row < 0 or row > 4 then continue end\n"
    "  if row == 0 then\n"
    "    if col == 0 then doClear() end\n"
    "    if col == 1 then doBack() end\n"
    "    if col == 2 then doOp(\"%\") end\n"
    "    if col == 3 then doOp(\"/\") end\n"
    "  end\n"
    "  if row == 1 then\n"
    "    if col == 0 then doDigit(\"7\") end\n"
    "    if col == 1 then doDigit(\"8\") end\n"
    "    if col == 2 then doDigit(\"9\") end\n"
    "    if col == 3 then doOp(\"*\") end\n"
    "  end\n"
    "  if row == 2 then\n"
    "    if col == 0 then doDigit(\"4\") end\n"
    "    if col == 1 then doDigit(\"5\") end\n"
    "    if col == 2 then doDigit(\"6\") end\n"
    "    if col == 3 then doOp(\"-\") end\n"
    "  end\n"
    "  if row == 3 then\n"
    "    if col == 0 then doDigit(\"1\") end\n"
    "    if col == 1 then doDigit(\"2\") end\n"
    "    if col == 2 then doDigit(\"3\") end\n"
    "    if col == 3 then doOp(\"+\") end\n"
    "  end\n"
    "  if row == 4 then\n"
    "    if col == 0 then doDigit(\"0\") end\n"
    "    if col == 1 then doDot() end\n"
    "    if col == 2 then doOp(\"\") end\n"
    "    if col == 3 then doOp(\"\") end\n"
    "  end\n"
    "  drawAll()\n"
    "end\n";

// ─── Files ───────────────────────────────────────────────────────────────────
const char FILES_SCRIPT[] PROGMEM =
    "#app \"Files\"\n"
    "#isApp true\n"
    "#appColor \"#34C759\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "var curDir = \"/\"\n"
    "\n"
    "def parentDir(path)\n"
    "  if path == \"/\" then return \"/\" end\n"
    "  var i = len(path) - 1\n"
    "  while i > 0 do\n"
    "    if substr(path, i, 1) == \"/\" then return substr(path, 0, i) end\n"
    "    i = i - 1\n"
    "  end\n"
    "  return \"/\"\n"
    "end\n"
    "\n"
    "def joinPath(dir, name)\n"
    "  if dir == \"/\" then return \"/\" + name end\n"
    "  return dir + \"/\" + name\n"
    "end\n"
    "\n"
    "loop\n"
    "  var list = fs.list(curDir)\n"
    "  var menu = \"\"\n"
    "  if curDir != \"/\" then menu = \".. (up)\" end\n"
    "  if len(list) > 0 then\n"
    "    if len(menu) > 0 then menu = menu + \"|\" end\n"
    "    menu = menu + list\n"
    "  end\n"
    "  if len(menu) == 0 then menu = \"(empty)\" end\n"
    "  var pick = ui.menu(menu, curDir, 1)\n"
    "  if pick < 0 then exit() end\n"
    "  var offset = 0\n"
    "  if curDir != \"/\" then\n"
    "    if pick == 0 then curDir = parentDir(curDir) continue end\n"
    "    offset = 1\n"
    "  end\n"
    "  if len(list) == 0 then continue end\n"
    "  var name = split(list, \"|\", pick - offset)\n"
    "  if endswith(name, \"/\") then\n"
    "    var bare = substr(name, 0, len(name) - 1)\n"
    "    curDir = joinPath(curDir, bare)\n"
    "    continue\n"
    "  end\n"
    "  var lower = lower(name)\n"
    "  if endswith(lower, \".osa\") then\n"
    "    app.launch(joinPath(curDir, name))\n"
    "  end\n"
    "  if endswith(lower, \".osa\") == 0 then\n"
    "    var content = fs.read(joinPath(curDir, name))\n"
    "    if len(content) > 256 then content = substr(content, 0, 256) + \"...\" end\n"
    "    ui.alert(name, content)\n"
    "  end\n"
    "end\n";

// ─── Notes ───────────────────────────────────────────────────────────────────
const char NOTES_SCRIPT[] PROGMEM =
    "#app \"Notes\"\n"
    "#isApp true\n"
    "#appColor \"#FFCC00\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "def ensureDir()\n"
    "  if fs.exists(\"/user\") == 0 then fs.mkdir(\"/user\") end\n"
    "  if fs.exists(\"/user/notes\") == 0 then fs.mkdir(\"/user/notes\") end\n"
    "end\n"
    "\n"
    "def viewNote(name)\n"
    "  while 1 == 1 do\n"
    "    var path = \"/user/notes/\" + name\n"
    "    var content = fs.read(path)\n"
    "    cls()\n"
    "    bg(15, 15, 25)\n"
    "    ui.backHeader(name)\n"
    "    textcolor(255, 255, 255)\n"
    "    fontsize(2)\n"
    "    var nl = char(10)\n"
    "    var line = 0\n"
    "    var ls = 0\n"
    "    var i = 0\n"
    "    while i <= len(content) and line < 13 do\n"
    "      var atNl = 0\n"
    "      if i == len(content) then atNl = 1 end\n"
    "      if atNl == 0 then\n"
    "        if substr(content, i, 1) == nl then atNl = 1 end\n"
    "      end\n"
    "      if atNl == 1 then\n"
    "        var ln = substr(content, ls, i - ls)\n"
    "        if len(ln) > 28 then ln = substr(ln, 0, 28) end\n"
    "        text(8, 60 + line * 18, ln)\n"
    "        line = line + 1\n"
    "        ls = i + 1\n"
    "      end\n"
    "      i = i + 1\n"
    "    end\n"
    "    setcolor(0, 122, 255)\n"
    "    rrect(20, 270, 90, 32, 8)\n"
    "    setcolor(255, 59, 48)\n"
    "    rrect(130, 270, 90, 32, 8)\n"
    "    textcolor(255, 255, 255)\n"
    "    fontsize(2)\n"
    "    textc(65, 286, \"Edit\")\n"
    "    textc(175, 286, \"Delete\")\n"
    "    while touch.down() == 0 do wait(40) end\n"
    "    var px = touch.x()\n"
    "    var py = touch.y()\n"
    "    while touch.down() == 1 do wait(20) end\n"
    "    if px < 0 or py < 0 then continue end\n"
    "    if py < 50 and px < 80 then return end\n"
    "    if py >= 270 and py <= 302 then\n"
    "      if px >= 20 and px <= 110 then\n"
    "        var newText = input(\"Edit note\", content, 1)\n"
    "        if len(newText) > 0 then fs.write(path, newText) end\n"
    "      end\n"
    "      if px >= 130 and px <= 220 then\n"
    "        if confirm(\"Delete \" + name + \"?\", \"This can't be undone\") == 1 then\n"
    "          fs.delete(path)\n"
    "          return\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "def newNote()\n"
    "  var text = input(\"New note\", \"\", 1)\n"
    "  if len(text) == 0 then return end\n"
    "  ensureDir()\n"
    "  var n = 1\n"
    "  while n < 100 do\n"
    "    var path = \"/user/notes/Note_\" + str(n) + \".txt\"\n"
    "    if fs.exists(path) == 0 then\n"
    "      fs.write(path, text)\n"
    "      ui.alert(\"Notes\", \"Saved as Note_\" + str(n) + \".txt\")\n"
    "      return\n"
    "    end\n"
    "    n = n + 1\n"
    "  end\n"
    "  ui.alert(\"Notes\", \"Note limit reached\")\n"
    "end\n"
    "\n"
    "loop\n"
    "  ensureDir()\n"
    "  var list = fs.list(\"/user/notes\")\n"
    "  var menu = \"[New note]\"\n"
    "  if len(list) > 0 then menu = menu + \"|\" + list end\n"
    "  var pick = ui.menu(menu, \"Notes\", 1)\n"
    "  if pick < 0 then exit() end\n"
    "  if pick == 0 then newNote() end\n"
    "  if pick > 0 then\n"
    "    var name = split(list, \"|\", pick - 1)\n"
    "    if startswith(name, \".\") == 0 then viewNote(name) end\n"
    "  end\n"
    "end\n";

// ─── Control Center ──────────────────────────────────────────────────────────
// Loaded by main.cpp into the second OSAApp instance (osaOverlayApp) when the
// user swipes down from the top. No #isApp flag — not a home tile.
const char CONTROLCENTER_SCRIPT[] PROGMEM =
    "#app \"Control Center\"\n"
    "#appColor \"#5856D6\"\n"
    "\n"
    "while touch.down() == 1 do wait(20) end\n"
    "wait(150)\n"
    "\n"
    "loop\n"
    "  var wifiSt = \"OFF\"\n"
    "  if wifi.isEnabled() == 1 then wifiSt = \"ON\" end\n"
    "  if wifi.connected() == 1 then wifiSt = wifi.ssid() end\n"
    "  var btSt = \"OFF\"\n"
    "  if bt.enabled() == 1 then btSt = \"ON\" end\n"
    "  var themeLbl = \"Light\"\n"
    "  if theme() == 1 then themeLbl = \"Dark\" end\n"
    "  var ntpSt = \"no\"\n"
    "  if time.synced() == 1 then ntpSt = \"OK\" end\n"
    "  ui.menuStart(\"Control Center\", 1)\n"
    "  ui.menuRow(\"Wi-Fi\",      \"W\",   0, 122, 255, wifiSt)\n"
    "  ui.menuRow(\"Bluetooth\",  \"B\",   0, 122, 255, btSt)\n"
    "  ui.menuRow(\"Theme\",      \"D\",  88,  86, 214, themeLbl)\n"
    "  ui.menuRow(\"Brightness\", \"*\", 255, 149,   0, str(getbright()))\n"
    "  ui.menuRow(\"NTP sync\",   \"N\",  52, 199,  89, ntpSt)\n"
    "  var pick = ui.menuShow()\n"
    "  if pick == -1 then exit() end\n"
    "  if pick == 0 then\n"
    "    var t = ui.toggle(\"Wi-Fi\", wifi.isEnabled())\n"
    "    if t == 1 then wifi.enable() end\n"
    "    if t == 0 then wifi.disable() end\n"
    "  end\n"
    "  if pick == 1 then\n"
    "    var t = ui.toggle(\"Bluetooth\", bt.enabled())\n"
    "    if t == 1 then bt.enable() end\n"
    "    if t == 0 then bt.disable() end\n"
    "  end\n"
    "  if pick == 2 then\n"
    "    var t = ui.segmented(\"Theme\", \"Light|Dark\", theme())\n"
    "    if t >= 0 then sys.theme(t) end\n"
    "  end\n"
    "  if pick == 3 then\n"
    "    var b = ui.slider(\"Brightness\", 10, 255, getbright())\n"
    "    if b >= 0 then sys.brightness(b) end\n"
    "  end\n"
    "  if pick == 4 then\n"
    "    if wifi.connected() == 0 then ui.alert(\"NTP\", \"Wi-Fi not connected\") end\n"
    "    if wifi.connected() == 1 then\n"
    "      var ok = ntp.sync()\n"
    "      if ok == 1 then ui.alert(\"NTP\", \"Time synchronised\") end\n"
    "      if ok == 0 then ui.alert(\"NTP\", \"Sync failed\") end\n"
    "    end\n"
    "  end\n"
    "end\n";

// Forward declarations of every system app's source body (defined further down).
extern const char SETTINGS_SCRIPT[]      PROGMEM;
extern const char CLOCK_SCRIPT[]         PROGMEM;
extern const char CALCULATOR_SCRIPT[]    PROGMEM;
extern const char FILES_SCRIPT[]         PROGMEM;
extern const char NOTES_SCRIPT[]         PROGMEM;
extern const char CONTROLCENTER_SCRIPT[] PROGMEM;

static const BuiltinException EXCEPTIONS[] = {
    { "/system/apps/settings.osa",      SETTINGS_SCRIPT      },
    { "/system/apps/clock.osa",         CLOCK_SCRIPT         },
    { "/system/apps/calculator.osa",    CALCULATOR_SCRIPT    },
    { "/system/apps/files.osa",         FILES_SCRIPT         },
    { "/system/apps/notes.osa",         NOTES_SCRIPT         },
    { "/system/apps/controlcenter.osa", CONTROLCENTER_SCRIPT },
};
static const int EXCEPTION_COUNT = sizeof(EXCEPTIONS) / sizeof(EXCEPTIONS[0]);

static void installBuiltinExceptions() {
    if (!isSdReady) return;
    SD.mkdir("/system");
    SD.mkdir("/system/apps");
    // Clean up old paths from previous firmware versions so users don't see
    // ghost copies of renamed system apps.
    if (SD.exists("/system/exceptions/sys_demo.osa"))
        SD.remove("/system/exceptions/sys_demo.osa");
    SD.rmdir("/system/exceptions");
    if (SD.exists("/system/apps/sys_demo.osa"))
        SD.remove("/system/apps/sys_demo.osa");

    // Force-write each manifested system app — the script in main.cpp is the
    // source of truth, so a firmware update propagates immediately.
    for (int i = 0; i < EXCEPTION_COUNT; i++) {
        const BuiltinException& e = EXCEPTIONS[i];
        SD.remove(e.path);
        File f = SD.open(e.path, FILE_WRITE);
        if (!f) continue;
        f.print((const __FlashStringHelper*)e.script);
        f.close();
    }

    // Security sweep: wipe anything in /system/apps that isn't manifested.
    // Files there get the privileged SDK implicitly, so a stray script copied
    // in via card reader would otherwise be a root escalation.
    File dir = SD.open("/system/apps");
    if (!dir || !dir.isDirectory()) return;
    File entry = dir.openNextFile();
    while (entry) {
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        String fullPath = String("/system/apps/") + name;

        if (!entry.isDirectory()) {
            bool manifested = false;
            for (int i = 0; i < EXCEPTION_COUNT; i++) {
                if (String(EXCEPTIONS[i].path) == fullPath) {
                    manifested = true;
                    break;
                }
            }
            if (!manifested) {
                Serial.printf("[Security] Removing unmanifested /system/apps file: %s\n",
                              fullPath.c_str());
                SD.remove(fullPath.c_str());
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

// Holds OsaShortcut instances built at boot so they outlive the scan and stay
// valid as long as Home references them.
static const int MAX_OSA_SHORTCUTS = 12;
static OsaShortcut* g_osaShortcuts[MAX_OSA_SHORTCUTS];
static int          g_osaShortcutCount = 0;

static void scanDirForShortcuts(const String& dirPath, int depth) {
    if (g_osaShortcutCount >= MAX_OSA_SHORTCUTS || depth > 1) return;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;

    File f = dir.openNextFile();
    while (f && g_osaShortcutCount < MAX_OSA_SHORTCUTS) {
        // f.name() can be either an absolute path or just the basename
        // depending on SD-library version — normalize against dirPath so
        // we always end up with an absolute path under the directory we walked.
        String basename = f.name();
        int slash = basename.lastIndexOf('/');
        if (slash >= 0) basename = basename.substring(slash + 1);
        String full = dirPath;
        if (!full.endsWith("/")) full += "/";
        full += basename;

        // Skip the per-app sandbox (/apps/<name>/) — it's runtime data, not scripts.
        if (full.startsWith("/apps/")) { f = dir.openNextFile(); continue; }

        if (f.isDirectory()) {
            if (depth == 0) scanDirForShortcuts(full, 1);
        } else {
            String lower = full; lower.toLowerCase();
            if (lower.endsWith(".osa") && OSARuntime::readIsAppFromFile(full)) {
                String displayName = OSARuntime::readAppNameFromFile(full);
                uint16_t color     = OSARuntime::readIconColorFromFile(
                                         full, tft.color565(255, 149, 0));
                g_osaShortcuts[g_osaShortcutCount++] =
                    new OsaShortcut(&tft, &ts, full, displayName, color);
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
}

static void registerOsaShortcuts() {
    if (!isSdReady) return;
    g_osaShortcutCount = 0;
    // User scripts on root and one level deep (skips /apps sandbox).
    scanDirForShortcuts("/", 0);
    // System apps — implicitly privileged because they live under /system/apps.
    // Treated as depth 1 so the scanner descends into the directory itself.
    scanDirForShortcuts("/system/apps", 1);

    for (int i = 0; i < g_osaShortcutCount; i++) {
        home.addApp(g_osaShortcuts[i]);
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

    if (sysBTEnabled) {
        SerialBT.begin("OpenOS");
    } else {
        SerialBT.end();
    }
}

static void openControlCenter(AppState returnState) {
    previousState = returnState;
    g_underlyingApp = activeApp;
    isSwipeDown = false;
    isGlobalSwiping = false;
    if (osaOverlayApp.loadScript("/system/apps/controlcenter.osa")) {
        activeApp    = &osaOverlayApp;
        currentState = STATE_CONTROLCENTER;
        activeApp->show();
    } else {
        notifyService.push("CC: load failed");
    }
}

static void closeControlCenter() {
    activeApp    = g_underlyingApp;
    g_underlyingApp = nullptr;
    currentState = previousState;
    if (currentState == STATE_HOMESCREEN || activeApp == nullptr) {
        currentState = STATE_HOMESCREEN;
        home.show(false);
    } else if (currentState == STATE_IN_APP) {
        activeApp->show();
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
        bootstrapExampleScript();
        installBuiltinExceptions();
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

    // All apps now ship as .osa scripts in /system/apps/ — registered below.
    registerOsaShortcuts();
    home.applyOrder();

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
            osaOverlayApp.show();
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
        osaOverlayApp.update();
        if (osaOverlayApp.wantsExit) {
            osaOverlayApp.wantsExit = false;
            osaOverlayApp.clearPendingLaunch();   // ignore app.launch from CC
            osaOverlayApp.clearWantsOverlay();    // CC can't open another CC
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
            // OSA shortcuts route through the shared runtime instead of show()ing
            // the tile directly. show()/update() on OsaShortcut are no-ops.
            if (tappedApp->scriptPath.length() > 0) {
                Serial.printf("[TAP] OSA shortcut: name='%s' path='%s'\n",
                              tappedApp->name.c_str(),
                              tappedApp->scriptPath.c_str());
                Serial.printf("[TAP] free heap before loadScript: %u\n",
                              ESP.getFreeHeap());
                bool ok = osaApp.loadScript(tappedApp->scriptPath);
                Serial.printf("[TAP] loadScript returned %s\n", ok ? "true" : "false");
                if (ok) {
                    activeApp = &osaApp;
                    currentState = STATE_IN_APP;
                    Serial.println("[TAP] calling activeApp->show()");
                    activeApp->show();
                    Serial.println("[TAP] show() returned");
                } else {
                    notifyService.push("Failed: " + tappedApp->name);
                    home.show(false);
                }
            } else {
                activeApp = tappedApp;
                currentState = STATE_IN_APP;
                activeApp->show();
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
                if (next.length() > 0) notifyService.push("Failed: " + next);
                activeApp = nullptr;
                currentState = STATE_HOMESCREEN;
                delay(120);
                home.show(false);
                return;
            }
        }
    }
}