#include "OSARuntime.h"
#include "../Config.h"
#include "../Applications/OSKeyboard.h"
#include "../Applications/Theme.h"
#include "../Applications/Crypto.h"
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <BluetoothSerial.h>
#include <math.h>

extern bool isSdReady;
extern int  sysTheme;
extern int  sysBrightness;
extern bool sysNtpSynced;
extern bool sysWiFiEnabled;
extern bool sysBTEnabled;
extern BluetoothSerial SerialBT;

// Forward — defined below near the block-navigation helpers.
static bool isBlockOpen(const String& t);

// Forward-declare notification push
namespace _osa_notify { void push(const char* msg); }

// ─── Permission table ────────────────────────────────────────────────────────
const OSAPermDesc OSA_PERM_TABLE[] = {
    { OSA_PERM_NOTIFY,  "Notifications",   "notify() \xE2\x80\x94 system banners" },
    { OSA_PERM_NETWORK, "Network",         "http.get(), http.post() \xE2\x80\x94 outbound" },
    { OSA_PERM_SYSTEM,  "System Settings", "setbright(), setwallpaper()" },
};
const int OSA_PERM_TABLE_COUNT = sizeof(OSA_PERM_TABLE) / sizeof(OSA_PERM_TABLE[0]);

// Map keyword in #perm header to a bit. Returns 0 if unknown.
static uint8_t permBitFromKeyword(const String& kw) {
    String k = kw; k.toLowerCase(); k.trim();
    if (k == "notify"  || k == "notifications") return OSA_PERM_NOTIFY;
    if (k == "system"  || k == "settings")      return OSA_PERM_SYSTEM;
    if (k == "network" || k == "http" || k == "internet") return OSA_PERM_NETWORK;
    return 0;
}

// ─── HTTP state (per-runtime) ────────────────────────────────────────────────
static int    s_httpStatus = 0;
static String s_httpBearer;

static String urlEncode(const String& s) {
    static const char hex[] = "0123456789ABCDEF";
    String out; out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (safe) { out += (char)c; }
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// ─── Minimal JSON path resolver ───────────────────────────────────────────────
// Not a full RFC-8259 parser; designed for navigating typical REST responses
// with json.get(body, "choices.0.message.content"). Handles strings (with the
// common \" \\ \n \t \r escapes), numbers, booleans, null, nested objects and
// arrays. Numeric path segments index into arrays.

static int jsonSkipWs(const String& s, int p) {
    int len = s.length();
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    return p;
}

// Advance past one complete JSON value starting at pos. Returns end index.
static int jsonSkipValue(const String& s, int pos) {
    int len = s.length();
    pos = jsonSkipWs(s, pos);
    if (pos >= len) return pos;
    char c = s[pos];

    if (c == '"') {
        pos++;
        while (pos < len) {
            if (s[pos] == '\\' && pos + 1 < len) { pos += 2; continue; }
            if (s[pos] == '"') return pos + 1;
            pos++;
        }
        return pos;
    }
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        int depth = 1; pos++;
        bool inStr = false;
        while (pos < len && depth > 0) {
            char ch = s[pos];
            if (inStr) {
                if (ch == '\\' && pos + 1 < len) { pos += 2; continue; }
                if (ch == '"') inStr = false;
                pos++;
            } else {
                if (ch == '"') { inStr = true; pos++; }
                else if (ch == '{' || ch == '[') { depth++; pos++; }
                else if (ch == '}' || ch == ']') { depth--; pos++; }
                else pos++;
            }
        }
        return pos;
    }
    // primitive — read until separator
    while (pos < len) {
        char ch = s[pos];
        if (ch == ',' || ch == '}' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
        pos++;
    }
    return pos;
}

// Get raw value text for "key" in object at pos (which must point to '{').
// Returns empty string if not found.
static String jsonObjectField(const String& s, int pos, const String& name) {
    int len = s.length();
    pos = jsonSkipWs(s, pos);
    if (pos >= len || s[pos] != '{') return "";
    pos++;
    while (pos < len) {
        pos = jsonSkipWs(s, pos);
        if (pos >= len || s[pos] == '}') return "";
        if (s[pos] != '"') return "";
        int keyStart = ++pos;
        while (pos < len && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < len) pos++;
            pos++;
        }
        String key = s.substring(keyStart, pos);
        if (pos < len) pos++; // closing "
        pos = jsonSkipWs(s, pos);
        if (pos >= len || s[pos] != ':') return "";
        pos++;
        pos = jsonSkipWs(s, pos);
        int valStart = pos;
        int valEnd = jsonSkipValue(s, pos);
        if (key == name) return s.substring(valStart, valEnd);
        pos = jsonSkipWs(s, valEnd);
        if (pos < len && s[pos] == ',') { pos++; continue; }
        return "";
    }
    return "";
}

// Get Nth element of array starting at pos (which must point to '[').
static String jsonArrayElement(const String& s, int pos, int idx) {
    int len = s.length();
    pos = jsonSkipWs(s, pos);
    if (pos >= len || s[pos] != '[') return "";
    pos++;
    int i = 0;
    while (pos < len) {
        pos = jsonSkipWs(s, pos);
        if (pos >= len || s[pos] == ']') return "";
        int valStart = pos;
        int valEnd = jsonSkipValue(s, pos);
        if (i == idx) return s.substring(valStart, valEnd);
        i++;
        pos = jsonSkipWs(s, valEnd);
        if (pos < len && s[pos] == ',') { pos++; continue; }
        return "";
    }
    return "";
}

// Count elements of object or array at pos.
static int jsonContainerSize(const String& s, int pos) {
    int len = s.length();
    pos = jsonSkipWs(s, pos);
    if (pos >= len) return 0;
    bool isObj = (s[pos] == '{');
    bool isArr = (s[pos] == '[');
    if (!isObj && !isArr) return 0;
    pos++;
    int n = 0;
    while (pos < len) {
        pos = jsonSkipWs(s, pos);
        if (pos >= len) return n;
        if (s[pos] == '}' || s[pos] == ']') return n;
        if (isObj) {
            if (s[pos] != '"') return n;
            pos++;
            while (pos < len && s[pos] != '"') {
                if (s[pos] == '\\' && pos + 1 < len) pos++;
                pos++;
            }
            if (pos < len) pos++;
            pos = jsonSkipWs(s, pos);
            if (pos >= len || s[pos] != ':') return n;
            pos++;
        }
        pos = jsonSkipWs(s, pos);
        pos = jsonSkipValue(s, pos);
        n++;
        pos = jsonSkipWs(s, pos);
        if (pos < len && s[pos] == ',') { pos++; continue; }
        return n;
    }
    return n;
}

// Strip outer quotes and unescape if the value is a JSON string; pass through
// numbers, booleans, null, objects and arrays as-is.
static String jsonUnquote(const String& v) {
    int len = v.length();
    if (len < 2 || v[0] != '"' || v[len - 1] != '"') return v;
    String out; out.reserve(len);
    for (int i = 1; i < len - 1; i++) {
        if (v[i] == '\\' && i + 1 < len - 1) {
            char e = v[i + 1];
            if      (e == 'n')  out += '\n';
            else if (e == 't')  out += '\t';
            else if (e == 'r')  out += '\r';
            else if (e == '"')  out += '"';
            else if (e == '\\') out += '\\';
            else if (e == '/')  out += '/';
            else if (e == 'u' && i + 5 < len - 1) {
                long cp = strtol(v.substring(i + 2, i + 6).c_str(), nullptr, 16);
                if (cp > 0 && cp < 0x80)        out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                i += 4;
            }
            else out += e;
            i++;
        } else {
            out += v[i];
        }
    }
    return out;
}

// Walk a dotted path through a JSON document. Numeric segments index arrays.
// Returns raw value text (still in JSON form — caller unquotes if desired).
static String jsonWalkPath(const String& src, const String& path) {
    String cur = src;
    if (path.length() == 0) return cur;
    int start = 0;
    for (int i = 0; i <= (int)path.length(); i++) {
        if (i == (int)path.length() || path[i] == '.') {
            String seg = path.substring(start, i);
            start = i + 1;
            if (seg.length() == 0) continue;
            // Numeric → array index. Otherwise object field.
            bool numeric = true;
            for (int k = 0; k < (int)seg.length(); k++)
                if (!isdigit(seg[k])) { numeric = false; break; }
            cur = numeric
                ? jsonArrayElement(cur, jsonSkipWs(cur, 0), seg.toInt())
                : jsonObjectField (cur, jsonSkipWs(cur, 0), seg);
            if (cur.length() == 0) return "";
        }
    }
    return cur;
}

// ═══════════════════════════════════════════════════════════════════════════════
// OSAVal helpers
// ═══════════════════════════════════════════════════════════════════════════════

static String numToStr(double v) {
    if (v == (long long)v && fabs(v) < 1e15) {
        return String((long long)v);
    }
    char buf[32]; dtostrf(v, 0, 6, buf);
    String s(buf); s.trim();
    if (s.indexOf('.') >= 0) {
        while (s.endsWith("0")) s.remove(s.length() - 1);
        if (s.endsWith(".")) s.remove(s.length() - 1);
    }
    return s;
}

bool   OSAVal::truthy()   const { return isNum ? (num != 0.0) : (str.length() > 0); }
String OSAVal::toString() const { return isNum ? numToStr(num) : str; }
double OSAVal::toNum()    const { return isNum ? num : str.toDouble(); }

// ═══════════════════════════════════════════════════════════════════════════════
// Constructor / reset
// ═══════════════════════════════════════════════════════════════════════════════

OSARuntime::OSARuntime(TFT_eSPI* t, XPT2046_Touchscreen* ts_)
    : tft(t), ts(ts_) {}

void OSARuntime::setError(int line, const String& msg) {
    if (errLine >= 0) return; // keep first error
    errLine = line;
    errMsg  = msg;
    exitFlag = true;
}

// Waits until the touch panel has reported "not touched" for N consecutive
// reads. XPT2046 occasionally flickers to !touched() while a finger is still
// down, so a naive `while (ts->touched()) yield();` exits too early and lets
// the next widget grab the same physical tap (the "Back in Wi-Fi → opens
// Wallpapers because both are row 4" bug).
static void waitFullRelease(XPT2046_Touchscreen* ts) {
    int clean = 0;
    while (clean < 10) {
        yield();
        if (ts->touched()) clean = 0;
        else               clean++;
        delay(8);
    }
}

// Global cool-down between widget taps. Solves the ghost-tap that remained
// even after waitFullRelease: rapid taps can sneak through if the parent
// widget finishes drawing fast. The cool-down restarts on every touch event,
// so the next widget only accepts a tap that began AFTER the user has been
// completely off the screen for the full gap.
static unsigned long s_lastTapMs = 0;
static const unsigned long MIN_TAP_GAP_MS = 500;

static void enforceTapGap(XPT2046_Touchscreen* ts) {
    unsigned long target = s_lastTapMs + MIN_TAP_GAP_MS;
    while (millis() < target) {
        // If the user is still tapping during the cool-down, reset the target
        // so we measure from when they actually let go.
        if (ts->touched()) target = millis() + MIN_TAP_GAP_MS;
        yield();
        delay(8);
    }
}

static void markTapAccepted() {
    s_lastTapMs = millis();
}

bool OSARuntime::checkExitGesture() {
    if (!ts->touched()) { swipeHomeStartY = -1; return false; }
    TS_Point p = ts->getPoint();
    int ty = map(p.y, 300, 3800, 0, 320);
    if (ty > 290) {
        if (swipeHomeStartY < 0) swipeHomeStartY = ty;
        return false;
    }
    // Need a clear upward drag — guards against accidental taps near edges.
    if (swipeHomeStartY > 0 && swipeHomeStartY - ty > 60) {
        while (ts->touched()) yield();
        swipeHomeStartY = -1;
        exitFlag = true;
        return true;
    }
    return false;
}

bool OSARuntime::checkOverlayGesture() {
    // Swipe-down from the top edge → request Control Center. Same shape as
    // checkExitGesture but inverted. main.cpp inspects `wantsOverlay` after
    // the script unwinds and opens the overlay runtime instead of going home.
    if (!ts->touched()) { swipeOverlayStartY = -1; return false; }
    TS_Point p = ts->getPoint();
    int ty = map(p.y, 300, 3800, 0, 320);
    if (ty < 22) {
        if (swipeOverlayStartY < 0) swipeOverlayStartY = ty;
        return false;
    }
    if (swipeOverlayStartY >= 0 && ty - swipeOverlayStartY > 45) {
        while (ts->touched()) yield();
        swipeOverlayStartY = -1;
        exitFlag = true;
        wantsOverlay = true;
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Script loading
// ═══════════════════════════════════════════════════════════════════════════════

bool OSARuntime::loadScript(const String& path) {
    Serial.printf("[RT] loadScript path='%s'\n", path.c_str());
    // Reset state
    lineCount = varCount = funcCount = stackDepth = 0;
    loopStart = loopEnd = -1;
    exitFlag = returnFlag = false;
    breakFlag = continueFlag = false;
    errLine = -1; errMsg = "";
    execDepth = 0;
    appName = "";
    requiredPerms = 0;
    isException = false;
    pendingLaunch = "";
    swipeHomeStartY = -1;
    swipeOverlayStartY = -1;
    wantsOverlay = false;
    drawColor = txtColor = TFT_WHITE;
    textFont = 2;
    // Per-script HTTP state — never leak bearer tokens or status between apps.
    s_httpBearer = "";
    s_httpStatus = 0;

    if (!isSdReady) { setError(0, "No SD card"); return false; }
    File f = SD.open(path);
    if (!f) { setError(0, "Not found: " + path); return false; }

    while (f.available() && lineCount < OSA_MAX_LINES) {
        String raw = f.readStringUntil('\n');
        raw.trim();
        lines[lineCount++] = raw;
    }
    f.close();
    Serial.printf("[RT] read %d lines\n", lineCount);

    // Headers — extracted via shared helpers so Settings and runtime stay in sync.
    appName       = readAppNameFromFile(path);
    requiredPerms = readRequiredPermsFromFile(path);
    isException   = readIsExceptionFromFile(path);
    Serial.printf("[RT] appName='%s' perms=0x%02X exception=%d\n",
                  appName.c_str(), requiredPerms, isException ? 1 : 0);

    // Find the *top-level* loop section. A bare `loop` keyword inside a
    // user-defined function would otherwise be picked up as the main update
    // body, leaving the real game/app loop unreachable.
    {
        int depth = 0;
        for (int i = 0; i < lineCount; i++) {
            const String& t = lines[i];
            if (depth == 0 && t == "loop") { loopStart = i; break; }
            if (isBlockOpen(t)) depth++;
            else if (t == "end" && depth > 0) depth--;
        }
    }
    if (loopStart >= 0) loopEnd = findMatchingEnd(loopStart);

    // Pre-register all user functions
    registerFuncs();

    // Create per-app sandbox directory
    if (isSdReady) {
        SD.mkdir("/apps");
        SD.mkdir(sandboxDir().c_str());
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// run entry points
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::runShow() {
    exitFlag = returnFlag = breakFlag = continueFlag = false;
    execDepth = 0;
    int to = (loopStart >= 0) ? loopStart : lineCount;
    Serial.printf("[RT] runShow 0..%d (loopStart=%d)\n", to, loopStart);
    execRange(0, to);
    Serial.printf("[RT] runShow done err=%d exit=%d\n", errLine, exitFlag ? 1 : 0);
}

bool OSARuntime::runUpdate() {
    if (exitFlag || errLine >= 0) return false;
    if (loopStart < 0) return false;
    returnFlag = breakFlag = continueFlag = false;
    execDepth = 0;
    execRange(loopStart + 1, loopEnd);
    return !(exitFlag || errLine >= 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Block navigation helpers
// ═══════════════════════════════════════════════════════════════════════════════

static bool isBlockOpen(const String& t) {
    // Inline one-liners ("if X then Y end" / "while X do Y end") are *not*
    // block opens — they're complete on one line and execLine handles them
    // directly. Treating them as block opens would skew depth counting in
    // findMatchingEnd, breaking enclosing def/while/if scopes.
    if (t.startsWith("if ") && t.endsWith(" end") &&
        t.indexOf(" then ") > 0) return false;
    if (t.startsWith("while ") && t.endsWith(" end") &&
        t.indexOf(" do ") > 0) return false;
    return t.startsWith("if ") || t.startsWith("while ") ||
           t.startsWith("for ") || t.startsWith("def ") || t == "loop";
}

int OSARuntime::findMatchingEnd(int lineNo) {
    int depth = 1;
    for (int i = lineNo + 1; i < lineCount; i++) {
        const String& t = lines[i];
        if (isBlockOpen(t)) depth++;
        else if (t == "end") { if (--depth == 0) return i; }
    }
    return lineCount;
}

int OSARuntime::findNextBranch(int from) {
    int depth = 0;
    for (int i = from; i < lineCount; i++) {
        const String& t = lines[i];
        if (isBlockOpen(t)) {
            depth++;
        } else if (t == "end") {
            if (depth == 0) return i;
            depth--;
        } else if (depth == 0 && (t.startsWith("elif ") || t == "else")) {
            return i;
        }
    }
    return lineCount;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Function registration
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::registerFuncs() {
    for (int i = 0; i < lineCount; i++) {
        String t = lines[i]; t.trim();
        if (!t.startsWith("def ")) continue;
        if (funcCount >= OSA_MAX_FUNCS) break;

        int parenO = t.indexOf('(');
        int parenC = t.lastIndexOf(')');
        if (parenO < 0 || parenC < 0) continue;

        Func& fn = funcs[funcCount++];
        fn.name = t.substring(4, parenO); fn.name.trim();
        fn.bodyStart = i + 1;
        fn.bodyEnd   = findMatchingEnd(i);
        fn.paramCount = 0;

        String pStr = t.substring(parenO + 1, parenC); pStr.trim();
        if (pStr.length() > 0) {
            int s = 0;
            for (int k = 0; k <= (int)pStr.length(); k++) {
                if (k == (int)pStr.length() || pStr[k] == ',') {
                    String p = pStr.substring(s, k); p.trim();
                    if (p.length() > 0 && fn.paramCount < 8)
                        fn.params[fn.paramCount++] = p;
                    s = k + 1;
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Execution engine
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::execRange(int from, int to) {
    if (execDepth > 24) { setError(from, "Nesting too deep"); return; }
    execDepth++;
    int pc = from;
    while (pc < to && !exitFlag && !returnFlag && !breakFlag && !continueFlag && errLine < 0) {
        yield(); // feed WDT
        execLine(pc, pc);
    }
    execDepth--;
}

void OSARuntime::execLine(int lineNo, int& pc) {
    const String& raw = lines[lineNo];

    // Empty / comment / def (already registered, skip body)
    if (raw.length() == 0 || raw[0] == '#') { pc++; return; }
    if (raw.startsWith("def ")) { pc = findMatchingEnd(lineNo) + 1; return; }
    if (raw == "end" || raw == "else" || raw.startsWith("elif ")) { pc++; return; }
    if (raw == "loop") { pc = findMatchingEnd(lineNo) + 1; return; }

    // ── Inline control flow (one-liners) ─────────────────────────────────────
    // "if <cond> then <body> end" / "while <cond> do <body> end"
    // Run the body by temporarily swapping it into lines[lineNo] and recursing
    // into execLine — the recursion stays shallow (one level per inline form).
    if (raw.startsWith("if ") && raw.endsWith(" end")) {
        int thenPos = raw.indexOf(" then ");
        if (thenPos > 0 && thenPos < (int)raw.length() - 4) {
            String cond = raw.substring(3, thenPos); cond.trim();
            String body = raw.substring(thenPos + 6, raw.length() - 4); body.trim();
            if (eval(cond).truthy() && body.length() > 0) {
                String saved = lines[lineNo];
                lines[lineNo] = body;
                int inner = lineNo;
                execLine(lineNo, inner);
                lines[lineNo] = saved;
            }
            pc++;
            return;
        }
    }
    if (raw.startsWith("while ") && raw.endsWith(" end")) {
        int doPos = raw.indexOf(" do ");
        if (doPos > 0 && doPos < (int)raw.length() - 4) {
            String cond = raw.substring(6, doPos); cond.trim();
            String body = raw.substring(doPos + 4, raw.length() - 4); body.trim();
            while (eval(cond).truthy() && !exitFlag && !returnFlag && errLine < 0) {
                yield();
                if (body.length() > 0) {
                    String saved = lines[lineNo];
                    lines[lineNo] = body;
                    int inner = lineNo;
                    execLine(lineNo, inner);
                    lines[lineNo] = saved;
                }
                if (continueFlag) { continueFlag = false; continue; }
                if (breakFlag)    { breakFlag = false; break; }
            }
            pc++;
            return;
        }
    }

    // ── Control flow (multi-line) ────────────────────────────────────────────

    if (raw.startsWith("if ")) { processIf(lineNo, pc); return; }
    if (raw.startsWith("while ")) { processWhile(lineNo, pc); return; }
    if (raw.startsWith("for ")) { processFor(lineNo, pc); return; }

    // return
    if (raw == "return") {
        returnValue = OSAVal(); returnFlag = true; pc++; return;
    }
    if (raw.startsWith("return ")) {
        String expr = raw.substring(7); expr.trim();
        returnValue = eval(expr); returnFlag = true; pc++; return;
    }

    // break / continue — handled by enclosing while/for in processWhile/processFor
    if (raw == "break")    { breakFlag    = true; pc++; return; }
    if (raw == "continue") { continueFlag = true; pc++; return; }

    // ── Variable declaration: var NAME = EXPR ────────────────────────────────
    if (raw.startsWith("var ")) {
        String rest = raw.substring(4); rest.trim();
        int eq = rest.indexOf('=');
        if (eq > 0) {
            String nm = rest.substring(0, eq); nm.trim();
            String ex = rest.substring(eq + 1); ex.trim();
            declareVar(nm, eval(ex));
        }
        pc++; return;
    }

    // ── Assignment: NAME = EXPR (careful not to match ==) ────────────────────
    {
        int eq = -1;
        for (int i = 0; i < (int)raw.length(); i++) {
            if (raw[i] == '=') {
                bool prevOk = (i == 0 || (raw[i-1] != '!' && raw[i-1] != '<' &&
                                          raw[i-1] != '>' && raw[i-1] != '='));
                bool nextOk = (i+1 >= (int)raw.length() || raw[i+1] != '=');
                if (prevOk && nextOk) { eq = i; break; }
            }
        }
        if (eq > 0) {
            String nm = raw.substring(0, eq); nm.trim();
            // Validate: must be a plain identifier
            bool valid = (nm.length() > 0);
            for (int i = 0; i < (int)nm.length() && valid; i++)
                if (!isalnum(nm[i]) && nm[i] != '_') valid = false;
            if (valid) {
                String ex = raw.substring(eq + 1); ex.trim();
                setVar(nm, eval(ex));
                pc++; return;
            }
        }
    }

    // ── Anything else: evaluate as expression (function call, etc.) ───────────
    eval(raw);
    pc++;
}

// ── if / elif / else ─────────────────────────────────────────────────────────

void OSARuntime::processIf(int lineNo, int& pc) {
    int curLine = lineNo;
    // Iterative elif chain — no recursion
    while (!exitFlag && !returnFlag && errLine < 0) {
        const String& raw = lines[curLine];

        // Extract condition
        String cond = raw.startsWith("elif ") ? raw.substring(5) : raw.substring(3);
        if (cond.endsWith(" then")) cond = cond.substring(0, cond.length() - 5);
        cond.trim();

        int nextBranch = findNextBranch(curLine + 1);
        int finalEnd   = findMatchingEnd(curLine);

        if (eval(cond).truthy()) {
            execRange(curLine + 1, nextBranch);
            pc = finalEnd + 1;
            return;
        }

        if (nextBranch >= lineCount) { pc = lineCount; return; }
        const String& bt = lines[nextBranch];

        if (bt.startsWith("elif ")) {
            curLine = nextBranch; // iterate rather than recurse
        } else if (bt == "else") {
            int elseEnd = findNextBranch(nextBranch + 1);
            execRange(nextBranch + 1, elseEnd);
            pc = elseEnd + 1;
            return;
        } else {
            pc = nextBranch + 1;
            return;
        }
    }
}

// ── while ────────────────────────────────────────────────────────────────────

void OSARuntime::processWhile(int lineNo, int& pc) {
    const String& raw = lines[lineNo];
    String cond = raw.substring(6); // remove "while "
    if (cond.endsWith(" do")) cond = cond.substring(0, cond.length() - 3);
    cond.trim();

    int matchEnd = findMatchingEnd(lineNo);
    while (eval(cond).truthy() && !exitFlag && !returnFlag && errLine < 0) {
        execRange(lineNo + 1, matchEnd);
        if (continueFlag) { continueFlag = false; continue; }
        if (breakFlag)    { breakFlag = false; break; }
    }
    pc = matchEnd + 1;
}

// ── for i in start..end do ───────────────────────────────────────────────────

void OSARuntime::processFor(int lineNo, int& pc) {
    const String& raw = lines[lineNo];
    // "for VAR in START..END do"
    String rest = raw.substring(4); rest.trim();
    int inPos = rest.indexOf(" in ");
    if (inPos < 0) { pc++; return; }
    String varNm  = rest.substring(0, inPos); varNm.trim();
    String rangeS = rest.substring(inPos + 4);
    if (rangeS.endsWith(" do")) rangeS = rangeS.substring(0, rangeS.length() - 3);
    rangeS.trim();

    int dotdot = rangeS.indexOf("..");
    if (dotdot < 0) { pc++; return; }
    int startV = (int)eval(rangeS.substring(0, dotdot)).toNum();
    int endV   = (int)eval(rangeS.substring(dotdot + 2)).toNum();

    int matchEnd = findMatchingEnd(lineNo);
    for (int i = startV; i <= endV && !exitFlag && !returnFlag && errLine < 0; i++) {
        setVar(varNm, OSAVal((double)i));
        execRange(lineNo + 1, matchEnd);
        if (continueFlag) { continueFlag = false; continue; }
        if (breakFlag)    { breakFlag = false; break; }
    }
    pc = matchEnd + 1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Variable store
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::getVar(const String& name) const {
    for (int i = varCount - 1; i >= 0; i--)
        if (vars[i].name == name) return vars[i].val;
    return OSAVal(); // default 0
}

void OSARuntime::setVar(const String& name, const OSAVal& val) {
    for (int i = varCount - 1; i >= 0; i--) {
        if (vars[i].name == name) { vars[i].val = val; return; }
    }
    if (varCount < OSA_MAX_VARS) {
        vars[varCount].name = name;
        vars[varCount].val  = val;
        varCount++;
    }
}

void OSARuntime::declareVar(const String& name, const OSAVal& val) {
    // Limit the lookup to the current scope so `var pick = ...` inside a
    // user function doesn't reach into the caller and clobber its `pick`.
    // (Found this when SysDemo's wifiScreen() was overwriting the main loop's
    // `pick` variable and re-entering wallpapersScreen on Back.)
    int scopeStart = (stackDepth > 0) ? callStack[stackDepth - 1].retVarCount : 0;
    for (int i = varCount - 1; i >= scopeStart; i--) {
        if (vars[i].name == name) { vars[i].val = val; return; }
    }
    if (varCount < OSA_MAX_VARS) {
        vars[varCount].name = name;
        vars[varCount].val  = val;
        varCount++;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// User function call
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::callUser(const String& name, const String& argsStr) {
    Serial.printf("[RT] callUser %s(%s) depth=%d\n",
                  name.c_str(), argsStr.c_str(), stackDepth);
    // Find function
    int fi = -1;
    for (int i = 0; i < funcCount; i++)
        if (funcs[i].name == name) { fi = i; break; }
    if (fi < 0) { setError(-1, "Undefined: " + name); return OSAVal(); }

    // Parse + evaluate arguments
    String argTokens[6]; int argc = 0;
    splitArgs(argsStr, argTokens, argc);

    if (stackDepth >= OSA_STACK_MAX) { setError(-1, "Stack overflow"); return OSAVal(); }

    // Save scope
    int savedVarCount = varCount;
    callStack[stackDepth++] = { savedVarCount };

    // Bind parameters
    for (int i = 0; i < funcs[fi].paramCount && i < argc; i++)
        setVar(funcs[fi].params[i], eval(argTokens[i]));

    // Execute body
    returnFlag = false;
    returnValue = OSAVal();
    bool savedBreak    = breakFlag;
    bool savedContinue = continueFlag;
    breakFlag = continueFlag = false;
    execRange(funcs[fi].bodyStart, funcs[fi].bodyEnd);

    // Restore scope
    stackDepth--;
    varCount = savedVarCount;
    returnFlag = false;
    // Loop-control flags can't leak across function boundaries.
    breakFlag    = savedBreak;
    continueFlag = savedContinue;
    return returnValue;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Argument splitter (respects strings and nested parens)
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::splitArgs(const String& s, String* out, int& count) {
    count = 0;
    int depth = 0; bool inStr = false; int start = 0;
    for (int i = 0; i <= (int)s.length(); i++) {
        char c = (i < (int)s.length()) ? s[i] : ',';
        if (inStr) {
            if (c == '\\') i++;
            else if (c == '"') inStr = false;
        } else if (c == '"') {
            inStr = true;
        } else if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            depth--;
        } else if (c == ',' && depth == 0) {
            if (count < 6) {
                out[count] = s.substring(start, i);
                out[count].trim();
                count++;
            }
            start = i + 1;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Expression evaluator — recursive descent
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::skipWS(const String& s, int& p) {
    while (p < (int)s.length() && s[p] == ' ') p++;
}

bool OSARuntime::matchKw(const String& s, int& p, const char* kw) {
    int klen = strlen(kw);
    if (p + klen > (int)s.length()) return false;
    if (strncmp(s.c_str() + p, kw, klen) != 0) return false;
    // Keyword must not be followed by alphanumeric
    if (p + klen < (int)s.length() && (isalnum(s[p + klen]) || s[p + klen] == '_'))
        return false;
    p += klen;
    return true;
}

OSAVal OSARuntime::eval(const String& expr) {
    String e = expr; e.trim();
    int p = 0;
    return evalOr(e, p);
}

OSAVal OSARuntime::evalOr(const String& s, int& p) {
    OSAVal left = evalAnd(s, p);
    while (true) {
        skipWS(s, p);
        if (matchKw(s, p, "or")) {
            OSAVal right = evalAnd(s, p);
            left = OSAVal(left.truthy() || right.truthy());
        } else break;
    }
    return left;
}

OSAVal OSARuntime::evalAnd(const String& s, int& p) {
    OSAVal left = evalCompare(s, p);
    while (true) {
        skipWS(s, p);
        if (matchKw(s, p, "and")) {
            OSAVal right = evalCompare(s, p);
            left = OSAVal(left.truthy() && right.truthy());
        } else break;
    }
    return left;
}

OSAVal OSARuntime::evalCompare(const String& s, int& p) {
    OSAVal left = evalAddSub(s, p);
    skipWS(s, p);

    String op;
    if (p + 2 <= (int)s.length()) {
        String two = s.substring(p, p + 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">=")
            { op = two; p += 2; }
    }
    if (op.length() == 0 && p < (int)s.length()) {
        char c = s[p];
        if (c == '<' || c == '>') { op = String(c); p++; }
    }
    if (op.length() == 0) return left;

    skipWS(s, p);
    OSAVal right = evalAddSub(s, p);

    bool result;
    if (!left.isNum || !right.isNum) {
        int cmp = left.toString().compareTo(right.toString());
        if      (op == "==") result = cmp == 0;
        else if (op == "!=") result = cmp != 0;
        else if (op == "<")  result = cmp <  0;
        else if (op == ">")  result = cmp >  0;
        else if (op == "<=") result = cmp <= 0;
        else                 result = cmp >= 0;
    } else {
        double l = left.num, r = right.num;
        if      (op == "==") result = l == r;
        else if (op == "!=") result = l != r;
        else if (op == "<")  result = l <  r;
        else if (op == ">")  result = l >  r;
        else if (op == "<=") result = l <= r;
        else                 result = l >= r;
    }
    return OSAVal(result);
}

OSAVal OSARuntime::evalAddSub(const String& s, int& p) {
    OSAVal left = evalMulDiv(s, p);
    while (true) {
        skipWS(s, p);
        if (p >= (int)s.length()) break;
        char op = s[p];
        if (op != '+' && op != '-') break;
        p++;
        skipWS(s, p);
        OSAVal right = evalMulDiv(s, p);
        if (op == '+') {
            if (!left.isNum || !right.isNum)
                left = OSAVal(left.toString() + right.toString());
            else
                left = OSAVal(left.num + right.num);
        } else {
            left = OSAVal(left.toNum() - right.toNum());
        }
    }
    return left;
}

OSAVal OSARuntime::evalMulDiv(const String& s, int& p) {
    OSAVal left = evalUnary(s, p);
    while (true) {
        skipWS(s, p);
        if (p >= (int)s.length()) break;
        char op = s[p];
        if (op != '*' && op != '/' && op != '%') break;
        p++;
        skipWS(s, p);
        OSAVal right = evalUnary(s, p);
        if (op == '*') left = OSAVal(left.toNum() * right.toNum());
        else if (op == '/') {
            double d = right.toNum();
            left = OSAVal(d != 0 ? left.toNum() / d : 0.0);
        } else {
            left = OSAVal(fmod(left.toNum(), right.toNum()));
        }
    }
    return left;
}

OSAVal OSARuntime::evalUnary(const String& s, int& p) {
    skipWS(s, p);
    if (p < (int)s.length() && s[p] == '-') {
        p++; return OSAVal(-evalUnary(s, p).toNum());
    }
    if (matchKw(s, p, "not")) {
        return OSAVal(!evalUnary(s, p).truthy());
    }
    return evalPrimary(s, p);
}

OSAVal OSARuntime::evalPrimary(const String& s, int& p) {
    skipWS(s, p);
    if (p >= (int)s.length()) return OSAVal();

    // Grouped
    if (s[p] == '(') {
        p++;
        OSAVal v = evalOr(s, p);
        skipWS(s, p);
        if (p < (int)s.length() && s[p] == ')') p++;
        return v;
    }

    // String literal
    if (s[p] == '"') {
        p++;
        String str;
        while (p < (int)s.length() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < (int)s.length()) {
                p++;
                char e = s[p];
                if (e == 'n') str += '\n';
                else if (e == 't') str += '\t';
                else str += e;
            } else {
                str += s[p];
            }
            p++;
        }
        if (p < (int)s.length()) p++; // closing "
        return OSAVal(str);
    }

    // Number literal
    if (isdigit(s[p]) || (s[p] == '.' && p + 1 < (int)s.length() && isdigit(s[p+1]))) {
        double val = 0; bool dot = false; double frac = 0.1;
        while (p < (int)s.length() && (isdigit(s[p]) || (s[p] == '.' && !dot))) {
            if (s[p] == '.') { dot = true; }
            else if (!dot)   { val = val * 10 + (s[p] - '0'); }
            else             { val += (s[p] - '0') * frac; frac *= 0.1; }
            p++;
        }
        return OSAVal(val);
    }

    // Boolean literals
    if (matchKw(s, p, "true"))  return OSAVal(1.0);
    if (matchKw(s, p, "false")) return OSAVal(0.0);

    // Identifier, function call, or method call
    if (isalpha(s[p]) || s[p] == '_') {
        String name;
        while (p < (int)s.length() && (isalnum(s[p]) || s[p] == '_' || s[p] == '.'))
            name += s[p++];

        skipWS(s, p);
        if (p < (int)s.length() && s[p] == '(') {
            // Function / method call — collect arguments respecting nested parens and strings
            p++; // skip (
            int depth = 1, start = p;
            bool inStr = false;
            while (p < (int)s.length() && depth > 0) {
                char c = s[p];
                if (inStr) {
                    if (c == '\\') p++;
                    else if (c == '"') inStr = false;
                } else if (c == '"') { inStr = true; }
                else if (c == '(') depth++;
                else if (c == ')') { if (--depth == 0) break; }
                p++;
            }
            String argsStr = s.substring(start, p);
            if (p < (int)s.length()) p++; // skip )
            return callBuiltin(name, argsStr);
        }

        // Variable lookup
        return getVar(name);
    }

    p++; // consume unknown char
    return OSAVal();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Permissions
// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════
// Sandbox
// ═══════════════════════════════════════════════════════════════════════════════

String OSARuntime::sandboxDir() const {
    String dir = "/apps/";
    for (int i = 0; i < (int)appName.length() && dir.length() < 28; i++) {
        char c = appName[i];
        dir += isalnum(c) ? (char)tolower(c) : '_';
    }
    return dir;
}

String OSARuntime::sandboxPath(const String& rel) const {
    // Strip any path-traversal attempts, then join with sandbox dir
    String safe = rel;
    safe.replace("../", "");
    safe.replace("./",  "");
    safe.replace("..",  "");
    while (safe.startsWith("/")) safe.remove(0, 1);
    return sandboxDir() + "/" + safe;
}

// ─────────────────────────────────────────────────────────────────────────────

String OSARuntime::permKey() const {
    return permKeyForName(appName);
}

// ─── Shared helpers (single source of truth) ────────────────────────────────

String OSARuntime::permKeyForName(const String& appName) {
    String k = "perm_";
    for (int i = 0; i < (int)appName.length() && k.length() < 22; i++) {
        char c = appName[i];
        k += isalnum(c) ? (char)tolower(c) : '_';
    }
    return k;
}

String OSARuntime::readAppNameFromFile(const String& path) {
    String fallback;
    int slash = path.lastIndexOf('/');
    fallback = (slash >= 0) ? path.substring(slash + 1) : path;

    if (!isSdReady) return fallback;
    File f = SD.open(path);
    if (!f) return fallback;

    // Scan the whole file — #app may not be the first line (comments, blanks).
    while (f.available()) {
        String raw = f.readStringUntil('\n');
        raw.trim();
        if (raw.startsWith("#app ")) {
            String nm = raw.substring(5); nm.trim();
            if (nm.startsWith("\"")) nm = nm.substring(1);
            if (nm.endsWith("\""))   nm = nm.substring(0, nm.length() - 1);
            f.close();
            return nm.length() > 0 ? nm : fallback;
        }
    }
    f.close();
    return fallback;
}

uint8_t OSARuntime::readRequiredPermsFromFile(const String& path) {
    if (!isSdReady) return 0;
    File f = SD.open(path);
    if (!f) return 0;

    uint8_t mask = 0;
    while (f.available()) {
        String raw = f.readStringUntil('\n');
        raw.trim();
        if (!raw.startsWith("#perm ") && raw != "#perm") continue;
        if (raw.length() <= 6) continue;
        String list = raw.substring(6);
        int start = 0;
        for (int i = 0; i <= (int)list.length(); i++) {
            if (i == (int)list.length() || list[i] == ',') {
                String tok = list.substring(start, i);
                mask |= permBitFromKeyword(tok);
                start = i + 1;
            }
        }
    }
    f.close();
    return mask;
}

bool OSARuntime::readIsAppFromFile(const String& path) {
    if (!isSdReady) return false;
    File f = SD.open(path);
    if (!f) return false;
    while (f.available()) {
        String raw = f.readStringUntil('\n'); raw.trim();
        if (!raw.startsWith("#isApp")) continue;
        String v = raw.substring(6); v.trim(); v.toLowerCase();
        f.close();
        return v == "true" || v == "1" || v == "yes";
    }
    f.close();
    return false;
}

bool OSARuntime::readIsExceptionFromFile(const String& path) {
    // The privileged SDK is reserved for system apps in /system/apps/. The
    // directory is wiped at boot of anything not in the firmware manifest
    // (see installBuiltinExceptions in main.cpp), so a third-party script
    // cannot escalate to privileged just by being copied onto the SD card.
    //
    // A `#exception true` header in a user-supplied script is intentionally
    // ignored — that would let anyone with an SD reader grant themselves root.
    return path.startsWith("/system/apps/");
}

uint16_t OSARuntime::readIconColorFromFile(const String& path, uint16_t fallback) {
    if (!isSdReady) return fallback;
    File f = SD.open(path);
    if (!f) return fallback;
    while (f.available()) {
        String raw = f.readStringUntil('\n'); raw.trim();
        if (!raw.startsWith("#appColor")) continue;
        String v = raw.substring(9); v.trim();
        // Accept "#RRGGBB", #RRGGBB, RRGGBB, with optional quotes.
        if (v.startsWith("\"")) v = v.substring(1);
        if (v.endsWith("\""))   v = v.substring(0, v.length() - 1);
        v.trim();
        if (v.startsWith("#"))  v = v.substring(1);
        if (v.length() != 6) { f.close(); return fallback; }
        for (int i = 0; i < 6; i++) {
            if (!isxdigit(v[i])) { f.close(); return fallback; }
        }
        long hex = strtol(v.c_str(), nullptr, 16);
        uint8_t r = (hex >> 16) & 0xFF;
        uint8_t g = (hex >> 8)  & 0xFF;
        uint8_t b =  hex        & 0xFF;
        f.close();
        // TFT_eSPI::color565 packs as ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3).
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    f.close();
    return fallback;
}

bool OSARuntime::checkPerm(uint8_t bit, const String& label, const String& detail) {
    String key    = permKey();
    int    stored = Config::getInt(key, 0);
    uint8_t granted = stored & 0x0F;
    uint8_t denied  = (stored >> 4) & 0x0F;

    if (granted & bit) return true;
    if (denied  & bit) return false;

    bool allow = showPermPopup(label, detail);

    if (allow) stored |=  (int)bit;
    else       stored |= ((int)bit << 4);
    Config::setInt(key, stored);
    Config::save();
    return allow;
}

bool OSARuntime::showSystemPopup(const String& title, const String& body1, const String& body2,
                                  const String& leftBtn, const String& rightBtn, bool rightDanger) {
    const int cx = 20, cy = 88, cw = 200, cr = 14;
    const int bodyLines = (body2.length() > 0) ? 2 : 1;
    const int ch = 56 + bodyLines * 18 + 52; // title+padding + body + buttons

    // Snapshot the popup region so we can restore the underlying script UI on dismiss.
    // (~60 KB for a 200x150 area — well under the heap budget.) Falls back to redraw
    // hint if the allocation fails on a tight system.
    uint16_t* snapshot = (uint16_t*)malloc((size_t)cw * ch * sizeof(uint16_t));
    if (snapshot) tft->readRect(cx, cy, cw, ch, snapshot);

    tft->fillRoundRect(cx, cy, cw, ch, cr, TFT_WHITE);
    tft->drawRoundRect(cx, cy, cw, ch, cr, tft->color565(220, 220, 222));

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(tft->color565(20, 20, 22));
    tft->drawString(title, 120, cy + 24);

    tft->setTextFont(1);
    tft->setTextColor(tft->color565(110, 110, 118));
    tft->drawString(body1, 120, cy + 46);
    if (body2.length() > 0) tft->drawString(body2, 120, cy + 62);

    const int divY = cy + ch - 44;
    tft->drawFastHLine(cx, divY, cw, tft->color565(218, 218, 222));
    tft->drawFastVLine(cx + cw / 2, divY, 44, tft->color565(218, 218, 222));

    tft->setTextFont(2);
    tft->setTextColor(tft->color565(0, 122, 255));
    tft->drawString(leftBtn,  cx + cw / 4,     cy + ch - 22);
    uint16_t rCol = rightDanger ? tft->color565(255, 59, 48) : tft->color565(52, 199, 89);
    tft->setTextColor(rCol);
    tft->drawString(rightBtn, cx + cw * 3 / 4, cy + ch - 22);

    delay(180);
    bool right = false;
    while (true) {
        yield();
        if (checkExitGesture() || checkOverlayGesture()) { right = false; break; }
        if (!ts->touched()) continue;
        TS_Point p = ts->getPoint();
        int tx = map(p.x, 300, 3800, 0, 240);
        int ty = map(p.y, 300, 3800, 0, 320);
        if (ty < divY || ty > cy + ch || tx < cx || tx > cx + cw) continue;
        right = (tx > cx + cw / 2);
        while (ts->touched()) yield();
        delay(60);
        break;
    }

    // Restore screen so the popup truly "goes away" instead of sitting on top
    // of the script's UI until the next full repaint.
    if (snapshot) {
        tft->pushImage(cx, cy, cw, ch, snapshot);
        free(snapshot);
    }
    return right;
}

bool OSARuntime::showPermPopup(const String& label, const String& detail) {
    String title = "\"" + appName + "\" wants to";
    return showSystemPopup(title, label, detail, "Deny", "Allow", false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Built-in functions
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::callBuiltin(const String& name, const String& argsStr) {
    // Check user functions first — avoids allocating arg arrays on the stack
    for (int i = 0; i < funcCount; i++) {
        if (funcs[i].name == name) return callUser(name, argsStr);
    }

    String argT[6]; int argc = 0;
    splitArgs(argsStr, argT, argc);

    // Evaluate arguments eagerly
    OSAVal a[6];
    for (int i = 0; i < argc; i++) a[i] = eval(argT[i]);

    auto N = [&](int i, double def = 0) -> double {
        return (i < argc) ? a[i].toNum() : def;
    };
    auto S = [&](int i, const String& def = "") -> String {
        return (i < argc) ? a[i].toString() : def;
    };
    auto iN = [&](int i, int def = 0) -> int { return (int)N(i, def); };

    // ── Screen drawing ────────────────────────────────────────────────────────

    if (name == "clear" || name == "cls") {
        uint16_t bg = (sysTheme == 1) ? tft->color565(28, 28, 30) : TFT_WHITE;
        tft->fillScreen(bg);
        return OSAVal();
    }
    if (name == "bg") {
        tft->fillScreen(tft->color565(iN(0), iN(1), iN(2)));
        return OSAVal();
    }
    if (name == "text") {
        tft->setTextFont(textFont); tft->setTextSize(1);
        tft->setTextColor(txtColor); tft->setTextDatum(TL_DATUM);
        tft->drawString(S(2), iN(0), iN(1));
        return OSAVal();
    }
    if (name == "textc") {
        // text centered: textc(cx, y, "msg")
        tft->setTextFont(textFont); tft->setTextSize(1);
        tft->setTextColor(txtColor); tft->setTextDatum(MC_DATUM);
        tft->drawString(S(2), iN(0), iN(1));
        return OSAVal();
    }
    if (name == "rect") {
        tft->fillRect(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "rrect") {
        // rounded rect: rrect(x,y,w,h,r)
        tft->fillRoundRect(iN(0), iN(1), iN(2), iN(3), iN(4), drawColor);
        return OSAVal();
    }
    if (name == "frame") {
        tft->drawRect(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "circle") {
        tft->fillCircle(iN(0), iN(1), iN(2), drawColor);
        return OSAVal();
    }
    if (name == "ring") {
        tft->drawCircle(iN(0), iN(1), iN(2), drawColor);
        return OSAVal();
    }
    if (name == "line") {
        tft->drawLine(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "pixel") {
        tft->drawPixel(iN(0), iN(1), drawColor);
        return OSAVal();
    }
    if (name == "setcolor") {
        drawColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "textcolor") {
        txtColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "fontsize") {
        int fs = iN(0); textFont = (fs >= 4) ? 4 : (fs >= 2) ? 2 : 1;
        return OSAVal();
    }
    if (name == "screenw") return OSAVal(240.0);
    if (name == "screenh") return OSAVal(320.0);

    // ── Touch ─────────────────────────────────────────────────────────────────

    if (name == "touch.down") return OSAVal(ts->touched() ? 1.0 : 0.0);
    if (name == "touch.x") {
        if (!ts->touched()) return OSAVal(-1.0);
        TS_Point p = ts->getPoint();
        return OSAVal((double)map(p.x, 300, 3800, 0, 240));
    }
    if (name == "touch.y") {
        if (!ts->touched()) return OSAVal(-1.0);
        TS_Point p = ts->getPoint();
        return OSAVal((double)map(p.y, 300, 3800, 0, 320));
    }

    // ── System ────────────────────────────────────────────────────────────────

    if (name == "wait") {
        // Poll the global exit gesture during long waits so scripts with custom
        // touch loops (no ui.menu*) can still be swiped up to home.
        int ms = iN(0);
        if (ms <= 0) return OSAVal();
        unsigned long start = millis();
        while ((long)(millis() - start) < ms) {
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal();
            yield();
            delay(5);
        }
        return OSAVal();
    }
    if (name == "exit")   { exitFlag = true; return OSAVal(); }
    if (name == "confirm") {
        // confirm(title, body) or confirm(body) → 1 if OK, 0 if Cancel
        String t = (argc >= 2) ? S(0) : appName;
        String b = (argc >= 2) ? S(1) : S(0);
        return OSAVal(showSystemPopup(t, b, "", "Cancel", "OK", false) ? 1.0 : 0.0);
    }
    if (name == "millis") return OSAVal((double)millis());
    if (name == "theme")  return OSAVal((double)sysTheme);
    if (name == "uptime") return OSAVal((double)(millis() / 1000));
    if (name == "freeram") return OSAVal((double)ESP.getFreeHeap());
    if (name == "sdready") return OSAVal(isSdReady ? 1.0 : 0.0);
    if (name == "getbright") return OSAVal((double)sysBrightness);
    if (name == "getwallpaper") return OSAVal(Config::get("wallpaper_path", ""));
    if (name == "print") { Serial.println(S(0)); return OSAVal(); }

    // ── Time (NTP-backed) ─────────────────────────────────────────────────────
    {
        bool needsTime =
            name == "time.hour"  || name == "time.min"  || name == "time.sec" ||
            name == "time.day"   || name == "time.month"|| name == "time.year"||
            name == "time.weekday" || name == "time.now";
        if (needsTime) {
            struct tm ti;
            if (!getLocalTime(&ti, 5)) return OSAVal(-1.0);
            if (name == "time.hour")    return OSAVal((double)ti.tm_hour);
            if (name == "time.min")     return OSAVal((double)ti.tm_min);
            if (name == "time.sec")     return OSAVal((double)ti.tm_sec);
            if (name == "time.day")     return OSAVal((double)ti.tm_mday);
            if (name == "time.month")   return OSAVal((double)(ti.tm_mon + 1));
            if (name == "time.year")    return OSAVal((double)(ti.tm_year + 1900));
            if (name == "time.weekday") return OSAVal((double)ti.tm_wday);
            if (name == "time.now") {
                time_t t; time(&t); return OSAVal((double)t);
            }
        }
    }
    if (name == "time.synced") return OSAVal(sysNtpSynced ? 1.0 : 0.0);

    if (name == "notify") {
        if (!checkPerm(OSA_PERM_NOTIFY, "Send Notifications",
                       "Show system notification banners"))
            return OSAVal();
        extern void osa_notify(const char*);
        osa_notify(S(0).c_str());
        return OSAVal();
    }
    if (name == "setbright") {
        if (!checkPerm(OSA_PERM_SYSTEM, "System Settings",
                       "Change screen brightness"))
            return OSAVal();
        sysBrightness = constrain(iN(0), 10, 255);
        analogWrite(21, sysBrightness); // TFT_BL
        return OSAVal();
    }
    if (name == "setwallpaper") {
        if (!checkPerm(OSA_PERM_SYSTEM, "System Settings",
                       "Change system wallpaper"))
            return OSAVal();
        Config::set("wallpaper_path", S(0));
        Config::save();
        return OSAVal();
    }
    if (name == "wifi.connected") return OSAVal(WiFi.status() == WL_CONNECTED ? 1.0 : 0.0);
    if (name == "wifi.ssid") return OSAVal(WiFi.SSID());
    if (name == "wifi.ip")   return OSAVal(WiFi.localIP().toString());

    // ── JSON path navigation ─────────────────────────────────────────────────
    // Use dotted paths; numeric segments index arrays.
    //   json.get(body, "choices.0.message.content") → "Hello"
    //   json.has(body, "error")                     → 1 / 0
    //   json.size(body, "choices")                  → element count
    if (name == "json.get") {
        String raw = jsonWalkPath(S(0), S(1));
        return OSAVal(jsonUnquote(raw));
    }
    if (name == "json.raw") {
        // Returns the raw JSON fragment without unquoting — useful for nested
        // objects you want to feed back into json.get later.
        return OSAVal(jsonWalkPath(S(0), S(1)));
    }
    if (name == "json.has") {
        return OSAVal(jsonWalkPath(S(0), S(1)).length() > 0 ? 1.0 : 0.0);
    }
    if (name == "json.size") {
        String frag = (S(1).length() > 0) ? jsonWalkPath(S(0), S(1)) : S(0);
        return OSAVal((double)jsonContainerSize(frag, 0));
    }

    // ── HTTP (network permission) ────────────────────────────────────────────
    if (name == "url_encode") return OSAVal(urlEncode(S(0)));
    if (name == "http.status") return OSAVal((double)s_httpStatus);
    if (name == "http.bearer") {
        if (!checkPerm(OSA_PERM_NETWORK, "Network",
                       "Send authenticated requests"))
            return OSAVal();
        s_httpBearer = S(0);
        return OSAVal();
    }
    if (name == "http.get" || name == "http.post") {
        if (!checkPerm(OSA_PERM_NETWORK, "Network",
                       "Make HTTP requests over Wi-Fi"))
            return OSAVal("");
        if (WiFi.status() != WL_CONNECTED) {
            s_httpStatus = -1;
            return OSAVal("");
        }
        String url = S(0);
        // HTTPS requires a secure client; HTTP uses the default client.
        bool secure = url.startsWith("https://");
        HTTPClient http;
        WiFiClientSecure secureClient;
        bool ok = false;
        if (secure) {
            secureClient.setInsecure(); // no CA bundle on-device — trust on first use
            ok = http.begin(secureClient, url);
        } else {
            ok = http.begin(url);
        }
        if (!ok) { s_httpStatus = -2; return OSAVal(""); }

        if (s_httpBearer.length() > 0)
            http.addHeader("Authorization", "Bearer " + s_httpBearer);

        int code;
        if (name == "http.get") {
            code = http.GET();
        } else {
            String body = S(1);
            String ctype = (argc >= 3) ? S(2) : "application/json";
            http.addHeader("Content-Type", ctype);
            code = http.POST(body);
        }
        s_httpStatus = code;
        String resp;
        if (code > 0) resp = http.getString();
        http.end();
        return OSAVal(resp);
    }

    // ── Math ─────────────────────────────────────────────────────────────────

    if (name == "abs")    return OSAVal(fabs(N(0)));
    if (name == "min")    return OSAVal(min(N(0), N(1)));
    if (name == "max")    return OSAVal(max(N(0), N(1)));
    if (name == "sqrt")   return OSAVal(sqrt(N(0)));
    if (name == "sin")    return OSAVal(sin(N(0)));
    if (name == "cos")    return OSAVal(cos(N(0)));
    if (name == "floor")  return OSAVal(floor(N(0)));
    if (name == "ceil")   return OSAVal(ceil(N(0)));
    if (name == "int")    return OSAVal((double)(int)N(0));
    if (name == "pow")    return OSAVal(pow(N(0), N(1)));
    if (name == "round")  return OSAVal((double)(long long)floor(N(0) + 0.5));
    if (name == "log")    return OSAVal(log(N(0)));
    if (name == "exp")    return OSAVal(exp(N(0)));
    if (name == "tan")    return OSAVal(tan(N(0)));
    if (name == "atan2")  return OSAVal(atan2(N(0), N(1)));
    if (name == "random") {
        if (argc < 2) return OSAVal((double)random((long)N(0)));
        return OSAVal((double)random((long)N(0), (long)N(1)));
    }
    if (name == "pi") return OSAVal(3.14159265358979);

    // ── String ───────────────────────────────────────────────────────────────

    if (name == "str")       return OSAVal(a[0].toString());
    if (name == "num")       return OSAVal(S(0).toDouble());
    if (name == "len")       return OSAVal((double)S(0).length());
    if (name == "upper")     { String s = S(0); s.toUpperCase(); return OSAVal(s); }
    if (name == "lower")     { String s = S(0); s.toLowerCase(); return OSAVal(s); }
    if (name == "trim")      { String s = S(0); s.trim(); return OSAVal(s); }
    if (name == "substr")    return OSAVal(S(0).substring(iN(1), iN(1) + iN(2)));
    if (name == "replace")   { String s = S(0); s.replace(S(1), S(2)); return OSAVal(s); }
    if (name == "contains")  return OSAVal(S(0).indexOf(S(1)) >= 0 ? 1.0 : 0.0);
    if (name == "startswith")return OSAVal(S(0).startsWith(S(1)) ? 1.0 : 0.0);
    if (name == "endswith")  return OSAVal(S(0).endsWith(S(1)) ? 1.0 : 0.0);
    if (name == "indexof")   return OSAVal((double)S(0).indexOf(S(1)));
    if (name == "char")      { char c = (char)iN(0); return OSAVal(String(c)); }
    if (name == "code")      return OSAVal((double)(unsigned char)S(0)[0]);
    if (name == "split") {
        // split(str, delim) -> returns the Nth part; split(s, ",", n)
        String src = S(0), delim = S(1); int idx = iN(2);
        int count = 0, start = 0;
        for (int i = 0; i <= (int)src.length(); i++) {
            bool atDelim = (i == (int)src.length()) ||
                           src.substring(i, i + delim.length()) == delim;
            if (atDelim) {
                if (count == idx) return OSAVal(src.substring(start, i));
                count++; start = i + delim.length();
            }
        }
        return OSAVal("");
    }

    // ── File I/O (sandboxed to /apps/<appname>/) ──────────────────────────────

    if (name == "fread") {
        if (!isSdReady) return OSAVal("");
        File f = SD.open(sandboxPath(S(0)));
        if (!f) return OSAVal("");
        String c = f.readString(); f.close(); return OSAVal(c);
    }
    if (name == "freadline") {
        if (!isSdReady) return OSAVal("");
        File f = SD.open(sandboxPath(S(0))); if (!f) return OSAVal("");
        int target = iN(1);
        for (int i = 0; i <= target; i++) {
            String line = f.readStringUntil('\n');
            if (i == target) { f.close(); line.trim(); return OSAVal(line); }
        }
        f.close(); return OSAVal("");
    }
    if (name == "fwrite") {
        if (!isSdReady) return OSAVal(0.0);
        String p = sandboxPath(S(0));
        SD.remove(p.c_str());
        File f = SD.open(p, FILE_WRITE); if (!f) return OSAVal(0.0);
        f.print(S(1)); f.close(); return OSAVal(1.0);
    }
    if (name == "fappend") {
        if (!isSdReady) return OSAVal(0.0);
        File f = SD.open(sandboxPath(S(0)), FILE_APPEND); if (!f) return OSAVal(0.0);
        f.println(S(1)); f.close(); return OSAVal(1.0);
    }
    if (name == "fexists") {
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.exists(sandboxPath(S(0)).c_str()) ? 1.0 : 0.0);
    }
    if (name == "fremove") {
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.remove(sandboxPath(S(0)).c_str()) ? 1.0 : 0.0);
    }

    // ── String formatting ────────────────────────────────────────────────────

    if (name == "repeat") {
        String src = S(0); int n = iN(1);
        if (n <= 0) return OSAVal("");
        String out; out.reserve(src.length() * n);
        for (int i = 0; i < n; i++) out += src;
        return OSAVal(out);
    }
    if (name == "padleft" || name == "padright") {
        String src = S(0); int n = iN(1);
        String chS = (argc >= 3) ? S(2) : " ";
        char ch = chS.length() > 0 ? chS[0] : ' ';
        if ((int)src.length() >= n) return OSAVal(src);
        String pad; pad.reserve(n - src.length());
        for (int i = 0; i < n - (int)src.length(); i++) pad += ch;
        return OSAVal(name == "padleft" ? pad + src : src + pad);
    }

    // ── Per-app key-value storage (sandboxed) ────────────────────────────────
    // Stored as INI lines in /apps/<name>/_kv.ini
    if (name == "kv.get" || name == "kv.set" || name == "kv.del") {
        if (!isSdReady) {
            return (name == "kv.get") ? OSAVal(S(1, "")) : OSAVal(0.0);
        }
        String kvPath = sandboxPath("_kv.ini");
        String key = S(0);

        // Load existing key-values
        String keys[24], vals[24]; int kvCount = 0;
        File rf = SD.open(kvPath);
        if (rf) {
            while (rf.available() && kvCount < 24) {
                String ln = rf.readStringUntil('\n'); ln.trim();
                int eq = ln.indexOf('=');
                if (eq < 1) continue;
                keys[kvCount] = ln.substring(0, eq);
                vals[kvCount] = ln.substring(eq + 1);
                kvCount++;
            }
            rf.close();
        }

        // get → return matching value or default
        if (name == "kv.get") {
            for (int i = 0; i < kvCount; i++)
                if (keys[i] == key) return OSAVal(vals[i]);
            return OSAVal(S(1, ""));
        }

        // mutate
        if (name == "kv.set") {
            String value = a[1].toString();
            bool found = false;
            for (int i = 0; i < kvCount; i++) {
                if (keys[i] == key) { vals[i] = value; found = true; break; }
            }
            if (!found && kvCount < 24) {
                keys[kvCount] = key; vals[kvCount] = value; kvCount++;
            }
        } else { // kv.del
            for (int i = 0; i < kvCount; i++) {
                if (keys[i] == key) {
                    for (int j = i; j < kvCount - 1; j++) {
                        keys[j] = keys[j+1]; vals[j] = vals[j+1];
                    }
                    kvCount--; break;
                }
            }
        }

        // Persist
        SD.remove(kvPath.c_str());
        File wf = SD.open(kvPath, FILE_WRITE);
        if (!wf) return OSAVal(0.0);
        for (int i = 0; i < kvCount; i++) {
            wf.print(keys[i]); wf.print('='); wf.println(vals[i]);
        }
        wf.close();
        return OSAVal(1.0);
    }

    // ── input(prompt, default, [multiLine]) → modal text via OSKeyboard ──────
    // When multiLine != 0, Enter appends '\n' and a "Done" button in the
    // header confirms. Otherwise Enter confirms (single-line).
    if (name == "input") {
        String prompt = S(0);
        String buf    = S(1, "");
        bool   multi  = (argc >= 3) ? (iN(2) != 0) : false;
        OSKeyboard kbd(tft, ts);
        const int doneX = 178, doneY = 5, doneW = 56, doneH = 28;

        tft->fillRect(0, 0, 240, 160, Theme::bg());

        tft->fillRect(0, 0, 240, 38, Theme::header());
        tft->drawFastHLine(0, 38, 240, Theme::divider());
        tft->setTextFont(2); tft->setTextSize(1);
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString(multi ? "Edit" : "Input", multi ? 90 : 120, 19);
        if (multi) {
            tft->fillRoundRect(doneX, doneY, doneW, doneH, 6, tft->color565(52, 199, 89));
            tft->setTextColor(TFT_WHITE);
            tft->drawString("Done", doneX + doneW / 2, doneY + doneH / 2);
        }

        tft->setTextColor(Theme::subtext()); tft->setTextDatum(TL_DATUM);
        tft->drawString(prompt.length() > 0 ? prompt
                        : (multi ? "Multi-line:" : "Enter text:"),
                        10, multi ? 42 : 48);

        auto drawField = [&]() {
            if (multi) {
                tft->fillRoundRect(8, 58, 224, 98, 6, Theme::surface());
                tft->drawRoundRect(8, 58, 224, 98, 6, Theme::divider2());
                tft->setTextFont(2); tft->setTextColor(Theme::text());
                tft->setTextDatum(TL_DATUM);
                const int lineH = 16;
                const int maxLines = 5;
                int totalLines = 1;
                for (int i = 0; i < (int)buf.length(); i++)
                    if (buf[i] == '\n') totalLines++;
                int firstLine = (totalLines > maxLines) ? (totalLines - maxLines) : 0;
                int idx = 0;
                int start = 0;
                int y = 64;
                for (int i = 0; i <= (int)buf.length(); i++) {
                    if (i == (int)buf.length() || buf[i] == '\n') {
                        if (idx >= firstLine) {
                            String line = buf.substring(start, i);
                            if (line.length() > 28)
                                line = String("...") + line.substring(line.length() - 25);
                            if (i == (int)buf.length()) line += "_";
                            tft->drawString(line, 14, y);
                            y += lineH;
                        }
                        idx++;
                        start = i + 1;
                    }
                }
            } else {
                tft->fillRoundRect(8, 78, 224, 36, 6, Theme::surface());
                tft->drawRoundRect(8, 78, 224, 36, 6, Theme::divider2());
                tft->setTextFont(2); tft->setTextColor(Theme::text());
                tft->setTextDatum(ML_DATUM);
                String shown = buf;
                if (shown.length() > 28) shown = shown.substring(shown.length() - 28);
                tft->drawString(shown + "_", 14, 96);
            }
        };
        drawField();
        kbd.draw();
        delay(120);

        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(buf);

            // Done button (multi-line only)
            if (multi && ts->touched()) {
                TS_Point p = ts->getPoint();
                int tx = map(p.x, 300, 3800, 0, 240);
                int ty = map(p.y, 300, 3800, 0, 320);
                if (ty >= doneY && ty <= doneY + doneH &&
                    tx >= doneX && tx <= doneX + doneW) {
                    waitFullRelease(ts);
                    return OSAVal(buf);
                }
            }

            char c = kbd.update();
            if (c == 0) continue;
            if (c == '\n') {
                if (multi) {
                    if ((int)buf.length() < 800) buf += '\n';
                } else {
                    break;
                }
            } else if (c == '\b') {
                if (buf.length() > 0) buf.remove(buf.length() - 1);
            } else if ((int)buf.length() < (multi ? 800 : 80)) {
                buf += c;
            }
            drawField();
        }
        return OSAVal(buf);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // UI helpers — available to every script
    // ═════════════════════════════════════════════════════════════════════════

    // ui.header(title) — paints a standard top bar so apps look consistent.
    if (name == "ui.header") {
        tft->fillRect(0, 0, 240, 40, Theme::header());
        tft->drawFastHLine(0, 40, 240, Theme::divider());
        tft->setTextFont(2); tft->setTextSize(1);
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString(S(0), 120, 20);
        return OSAVal();
    }

    // ui.alert(title, body) — OK popup, blocks until acknowledged.
    if (name == "ui.alert") {
        showSystemPopup(S(0), S(1), "", "", "OK", false);
        return OSAVal();
    }

    // ui.menu(itemsPiped, title) — vertical list, returns selected index or -1
    // when user swipes up from the bottom edge to cancel.
    //   ui.menu("Display|Wi-Fi|About", "Settings")
    if (name == "ui.menu") {
        String items_str = S(0);
        String title     = (argc >= 2) ? S(1) : String("Menu");
        // Optional third arg: showBack (1 = render "< Back" in header, tap
        // returns -1).
        bool   showBack  = (argc >= 3) ? (iN(2) != 0) : false;
        const int MAX_ITEMS = 12;
        String items[MAX_ITEMS];
        int    itemCount = 0;
        int start = 0;
        for (int i = 0; i <= (int)items_str.length() && itemCount < MAX_ITEMS; i++) {
            if (i == (int)items_str.length() || items_str[i] == '|') {
                items[itemCount++] = items_str.substring(start, i);
                start = i + 1;
            }
        }

        const int rowH    = 44;
        const int headerH = showBack ? 50 : 40;
        const int rowY0   = headerH + 10;
        tft->fillScreen(Theme::bg());
        tft->fillRect(0, 0, 240, headerH, Theme::header());
        tft->drawFastHLine(0, headerH, 240, Theme::divider());
        tft->setTextFont(2);
        if (showBack) {
            tft->setTextColor(tft->color565(0, 122, 255));
            tft->setTextDatum(ML_DATUM);
            tft->drawString("< Back", 10, headerH / 2);
        }
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString(title, 120, headerH / 2);
        for (int i = 0; i < itemCount; i++) {
            int y = rowY0 + i * rowH;
            tft->fillRect(0, y, 240, rowH - 1, Theme::surface());
            tft->drawFastHLine(0, y + rowH - 1, 240, Theme::divider2());
            tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM);
            tft->drawString(items[i], 16, y + rowH / 2);
            tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
            tft->drawString(">", 222, y + rowH / 2);
        }

        enforceTapGap(ts);
        waitFullRelease(ts);
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
            if (!ts->touched()) continue;
            TS_Point p = ts->getPoint();
            int tx = map(p.x, 300, 3800, 0, 240);
            int ty = map(p.y, 300, 3800, 0, 320);
            // Don't grab taps in the bottom strip — that's the exit gesture zone.
            if (ty > 290) continue;
            // "< Back" hot zone (top-left corner of header).
            if (showBack && ty < headerH && tx < 80) {
                waitFullRelease(ts);
                markTapAccepted();
                return OSAVal(-1.0);
            }
            if (ty >= rowY0 && ty < rowY0 + itemCount * rowH && tx >= 0 && tx <= 240) {
                int idx = (ty - rowY0) / rowH;
                waitFullRelease(ts);
                markTapAccepted();
                return OSAVal((double)idx);
            }
        }
    }

    // ui.slider(label, min, max, val) → new value, or -1 on Cancel
    if (name == "ui.slider") {
        String label = S(0);
        int minV = iN(1), maxV = iN(2);
        int val  = constrain(iN(3), minV, maxV);
        const int barX = 20, barW = 200, barY = 180;
        auto draw = [&]() {
            tft->fillScreen(Theme::bg());
            tft->fillRect(0, 0, 240, 40, Theme::header());
            tft->drawFastHLine(0, 40, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextColor(Theme::text());
            tft->setTextDatum(MC_DATUM);
            tft->drawString(label, 120, 20);
            tft->fillRect(0, 80, 240, 80, Theme::bg());
            tft->setTextFont(7);
            tft->drawString(String(val), 120, 115);
            tft->fillRoundRect(barX, barY, barW, 12, 6, Theme::divider2());
            int range = (maxV - minV);
            int fillW = (range > 0) ? (barW * (val - minV)) / range : 0;
            tft->fillRoundRect(barX, barY, fillW, 12, 6, tft->color565(0, 122, 255));
            int thumbCX = barX + fillW;
            if (thumbCX < barX) thumbCX = barX;
            if (thumbCX > barX + barW) thumbCX = barX + barW;
            tft->fillCircle(thumbCX, barY + 6, 14, TFT_WHITE);
            tft->drawCircle(thumbCX, barY + 6, 14, Theme::divider());
            tft->fillRoundRect(20, 250, 80, 36, 8, tft->color565(255, 59, 48));
            tft->setTextFont(2); tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
            tft->drawString("Cancel", 60, 268);
            tft->fillRoundRect(140, 250, 80, 36, 8, tft->color565(52, 199, 89));
            tft->drawString("OK", 180, 268);
        };
        draw();
        delay(150);
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
            if (!ts->touched()) continue;
            TS_Point p = ts->getPoint();
            int tx = map(p.x, 300, 3800, 0, 240);
            int ty = map(p.y, 300, 3800, 0, 320);
            if (ty >= 250 && ty <= 286 && tx >= 20 && tx <= 100) {
                while (ts->touched()) yield();
                return OSAVal(-1.0);
            }
            if (ty >= 250 && ty <= 286 && tx >= 140 && tx <= 220) {
                while (ts->touched()) yield();
                return OSAVal((double)val);
            }
            if (ty >= barY - 25 && ty <= barY + 35) {
                int range = (maxV - minV);
                int newVal = (range > 0)
                    ? minV + (long)(tx - barX) * range / barW
                    : minV;
                newVal = constrain(newVal, minV, maxV);
                if (newVal != val) { val = newVal; draw(); }
            }
        }
    }

    // ui.toggle(label, state) → new state (0/1), or -1 on Cancel
    if (name == "ui.toggle") {
        String label = S(0);
        int state = iN(1) ? 1 : 0;
        const int sw = 120, sh = 60, sx = (240 - sw) / 2, sy = 110;
        auto draw = [&]() {
            tft->fillScreen(Theme::bg());
            tft->fillRect(0, 0, 240, 40, Theme::header());
            tft->drawFastHLine(0, 40, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextColor(Theme::text());
            tft->setTextDatum(MC_DATUM);
            tft->drawString(label, 120, 20);
            tft->fillRect(0, 80, 240, 130, Theme::bg());
            uint16_t onCol  = tft->color565(52, 199, 89);
            uint16_t offCol = Theme::toggleOff();
            tft->fillRoundRect(sx, sy, sw, sh, sh / 2, state ? onCol : offCol);
            int knobR = sh / 2 - 6;
            int knobX = state ? (sx + sw - sh / 2) : (sx + sh / 2);
            tft->fillCircle(knobX, sy + sh / 2, knobR, TFT_WHITE);
            tft->setTextColor(state ? onCol : Theme::hint());
            tft->drawString(state ? "ON" : "OFF", 120, 200);
            tft->fillRoundRect(20, 250, 80, 36, 8, tft->color565(255, 59, 48));
            tft->setTextColor(TFT_WHITE);
            tft->drawString("Cancel", 60, 268);
            tft->fillRoundRect(140, 250, 80, 36, 8, tft->color565(52, 199, 89));
            tft->drawString("OK", 180, 268);
        };
        draw();
        delay(150);
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
            if (!ts->touched()) continue;
            TS_Point p = ts->getPoint();
            int tx = map(p.x, 300, 3800, 0, 240);
            int ty = map(p.y, 300, 3800, 0, 320);
            if (ty >= 250 && ty <= 286 && tx >= 20 && tx <= 100) {
                while (ts->touched()) yield();
                return OSAVal(-1.0);
            }
            if (ty >= 250 && ty <= 286 && tx >= 140 && tx <= 220) {
                while (ts->touched()) yield();
                return OSAVal((double)state);
            }
            if (ty >= sy - 10 && ty <= sy + sh + 10 && tx >= sx - 10 && tx <= sx + sw + 10) {
                state = !state;
                draw();
                while (ts->touched()) yield();
                delay(80);
            }
        }
    }

    // ── Settings-style rich menu (built incrementally) ───────────────────────
    // Mirrors SettingsApp::drawMenu pixel-for-pixel: 31 px rows, 24x24 rounded
    // icon with a letter, title left-aligned, value/arrow right-aligned.
    //
    //   ui.menuStart("Settings")
    //   ui.menuRow("Wi-Fi",     "W",  0, 122, 255, "ON >")
    //   ui.menuRow("Bluetooth", "B",  0, 122, 255, "OFF >")
    //   var pick = ui.menuShow()
    {
        static const int RICH_MAX_ROWS = 16;
        static String   s_rmTitle;
        static String   s_rmTitles[RICH_MAX_ROWS];
        static String   s_rmLetters[RICH_MAX_ROWS];
        static uint16_t s_rmColors[RICH_MAX_ROWS];
        static String   s_rmValues[RICH_MAX_ROWS];
        static int      s_rmCount = 0;
        static bool     s_rmShowBack = false;

        if (name == "ui.menuStart") {
            // Optional second arg: showBack (1 = draw "< Back" in header).
            // Defaults to 0 so the root menu doesn't get an unwanted button.
            s_rmTitle    = S(0);
            s_rmShowBack = (argc >= 2) ? (iN(1) != 0) : false;
            s_rmCount    = 0;
            return OSAVal();
        }
        if (name == "ui.menuRow") {
            // (title, letter, r, g, b, value)
            if (s_rmCount >= RICH_MAX_ROWS) return OSAVal();
            s_rmTitles [s_rmCount] = S(0);
            s_rmLetters[s_rmCount] = S(1);
            s_rmColors [s_rmCount] = tft->color565(iN(2), iN(3), iN(4));
            s_rmValues [s_rmCount] = (argc >= 6) ? S(5) : String(">");
            s_rmCount++;
            return OSAVal();
        }
        if (name == "ui.menuShow") {
            const int headerH = s_rmShowBack ? 50 : 40;
            tft->fillScreen(Theme::bg());
            tft->fillRect(0, 0, 240, headerH, Theme::header());
            tft->drawFastHLine(0, headerH, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextSize(1);
            if (s_rmShowBack) {
                tft->setTextColor(tft->color565(0, 122, 255));
                tft->setTextDatum(ML_DATUM);
                tft->drawString("< Back", 10, headerH / 2);
            }
            tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
            tft->drawString(s_rmTitle, 120, headerH / 2);

            const int rowH = 31;
            for (int i = 0; i < s_rmCount; i++) {
                int y = headerH + i * rowH;
                tft->fillRect(0, y, 240, rowH, Theme::surface());
                if (i < s_rmCount - 1)
                    tft->drawFastHLine(0, y + rowH, 240, Theme::divider2());

                tft->fillRoundRect(14, y + 3, 24, 24, 6, s_rmColors[i]);
                tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
                tft->drawString(s_rmLetters[i], 26, y + 15);

                tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM);
                tft->drawString(s_rmTitles[i], 46, y + 15);

                tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
                tft->drawString(s_rmValues[i], 225, y + 15);
            }

            // enforceTapGap() guarantees a cool-down since the last widget tap,
            // and waitFullRelease() makes sure the finger really is up.
            enforceTapGap(ts);
            waitFullRelease(ts);
            while (true) {
                yield();
                if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
                if (!ts->touched()) continue;
                TS_Point p = ts->getPoint();
                int tx = map(p.x, 300, 3800, 0, 240);
                int ty = map(p.y, 300, 3800, 0, 320);
                // Reserve the bottom strip for the exit gesture.
                if (ty > 290) continue;
                // "< Back" hot zone in the header (top-left corner).
                if (s_rmShowBack && ty < headerH && tx < 80) {
                    waitFullRelease(ts);
                    markTapAccepted();
                    return OSAVal(-1.0);
                }
                if (ty >= headerH && ty < headerH + s_rmCount * rowH && tx >= 0 && tx <= 240) {
                    int idx = (ty - headerH) / rowH;
                    waitFullRelease(ts);
                    markTapAccepted();
                    return OSAVal((double)idx);
                }
            }
        }
    }

    // ui.backHeader(title) — paints the standard "< Back  TITLE" 50-px bar
    // used by every Settings sub-screen.
    if (name == "ui.backHeader") {
        tft->fillRect(0, 0, 240, 50, Theme::header());
        tft->drawFastHLine(0, 50, 240, Theme::divider());
        tft->setTextFont(2); tft->setTextSize(1);
        tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM);
        tft->drawString("< Back", 10, 25);
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString(S(0), 120, 25);
        return OSAVal();
    }

    // ui.backTapped() — non-blocking check whether the user just tapped the
    // top-left "< Back" hot zone (x<80, y<50). Use in scripts that paint a
    // custom sub-screen and run their own touch loop.
    if (name == "ui.backTapped") {
        if (!ts->touched()) return OSAVal(0.0);
        TS_Point p = ts->getPoint();
        int tx = map(p.x, 300, 3800, 0, 240);
        int ty = map(p.y, 300, 3800, 0, 320);
        return OSAVal((tx < 80 && ty < 50) ? 1.0 : 0.0);
    }

    // ui.segmented(label, "Opt1|Opt2|...", current) — iOS-style segmented
    // control on a full screen, returns selected index or -1 on Cancel.
    if (name == "ui.segmented") {
        String label = S(0);
        String opts  = S(1);
        int    cur   = iN(2);

        String items[6]; int itemCount = 0;
        int start = 0;
        for (int i = 0; i <= (int)opts.length() && itemCount < 6; i++) {
            if (i == (int)opts.length() || opts[i] == '|') {
                items[itemCount++] = opts.substring(start, i);
                start = i + 1;
            }
        }
        if (itemCount == 0) return OSAVal(-1.0);
        if (cur < 0 || cur >= itemCount) cur = 0;

        const int barX = 20, barY = 120, barW = 200, barH = 40;
        int segW = barW / itemCount;
        auto draw = [&]() {
            tft->fillScreen(Theme::bg());
            tft->fillRect(0, 0, 240, 40, Theme::header());
            tft->drawFastHLine(0, 40, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextColor(Theme::text());
            tft->setTextDatum(MC_DATUM); tft->drawString(label, 120, 20);
            // segmented background
            tft->fillRoundRect(barX, barY, barW, barH, 10, tft->color565(228, 228, 228));
            for (int i = 0; i < itemCount; i++) {
                int sx = barX + i * segW;
                if (i == cur) {
                    tft->fillRoundRect(sx + 2, barY + 2, segW - 4, barH - 4, 9, TFT_WHITE);
                    tft->setTextColor(TFT_BLACK);
                } else {
                    tft->setTextColor(tft->color565(140, 140, 140));
                }
                tft->setTextDatum(MC_DATUM);
                tft->drawString(items[i], sx + segW / 2, barY + barH / 2);
            }
            // Cancel / OK
            tft->fillRoundRect(20, 250, 80, 36, 8, tft->color565(255, 59, 48));
            tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
            tft->drawString("Cancel", 60, 268);
            tft->fillRoundRect(140, 250, 80, 36, 8, tft->color565(52, 199, 89));
            tft->drawString("OK", 180, 268);
        };
        draw();
        delay(150);
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
            if (!ts->touched()) continue;
            TS_Point p = ts->getPoint();
            int tx = map(p.x, 300, 3800, 0, 240);
            int ty = map(p.y, 300, 3800, 0, 320);
            if (ty >= 250 && ty <= 286 && tx >= 20 && tx <= 100) {
                while (ts->touched()) yield();
                return OSAVal(-1.0);
            }
            if (ty >= 250 && ty <= 286 && tx >= 140 && tx <= 220) {
                while (ts->touched()) yield();
                return OSAVal((double)cur);
            }
            if (ty >= barY && ty <= barY + barH && tx >= barX && tx <= barX + barW) {
                int idx = (tx - barX) / segW;
                if (idx >= 0 && idx < itemCount && idx != cur) {
                    cur = idx; draw();
                    while (ts->touched()) yield();
                    delay(60);
                }
            }
        }
    }

    // ui.numpad(prompt, maxDigits) → entered string (digits only) or "" on cancel
    // 3x4 grid: 1-9, "<" (backspace), 0, ">" (confirm).
    if (name == "ui.numpad") {
        String prompt    = S(0);
        int    maxDigits = (argc >= 2) ? iN(1) : 8;
        if (maxDigits < 1) maxDigits = 1;
        if (maxDigits > 12) maxDigits = 12;
        String buf;
        const int btn = 55, gap = 5;
        const int gridW = 3 * btn + 2 * gap;
        const int gridX = (240 - gridW) / 2;
        const int gridY = 95;
        const char* keys = "123456789<0>";

        auto drawHeader = [&]() {
            tft->fillRect(0, 0, 240, 50, Theme::header());
            tft->drawFastHLine(0, 50, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextColor(tft->color565(0, 122, 255));
            tft->setTextDatum(ML_DATUM);
            tft->drawString("< Back", 10, 25);
            tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
            tft->drawString(prompt, 120, 25);
        };
        auto drawDots = [&]() {
            tft->fillRect(0, 60, 240, 28, Theme::bg());
            int dotW = 20 * maxDigits; if (dotW > 220) dotW = 220;
            int dotX = (240 - dotW) / 2;
            int step = dotW / maxDigits;
            for (int i = 0; i < maxDigits; i++) {
                int cx = dotX + i * step + step / 2;
                if (i < (int)buf.length())
                    tft->fillCircle(cx, 74, 7, Theme::text());
                else
                    tft->drawCircle(cx, 74, 7, Theme::divider2());
            }
        };
        auto drawKeys = [&]() {
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 3; c++) {
                    int x = gridX + c * (btn + gap);
                    int y = gridY + r * (btn + gap);
                    char k = keys[r * 3 + c];
                    uint16_t fill = Theme::surface();
                    if (k == '<') fill = tft->color565(120, 50, 50);
                    if (k == '>') fill = tft->color565(50, 130, 80);
                    tft->fillRoundRect(x, y, btn, btn, 10, fill);
                    tft->drawRoundRect(x, y, btn, btn, 10, Theme::divider2());
                    tft->setTextDatum(MC_DATUM);
                    tft->setTextFont(4);
                    tft->setTextColor(k == '<' || k == '>' ? TFT_WHITE : Theme::text());
                    char buf2[2] = { k, 0 };
                    tft->drawString(buf2, x + btn / 2, y + btn / 2);
                }
            }
        };

        tft->fillScreen(Theme::bg());
        drawHeader();
        drawDots();
        drawKeys();

        enforceTapGap(ts);
        waitFullRelease(ts);
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal("");
            if (!ts->touched()) continue;
            TS_Point p = ts->getPoint();
            int tx = map(p.x, 300, 3800, 0, 240);
            int ty = map(p.y, 300, 3800, 0, 320);
            if (ty > 290) continue;
            // "< Back" header — cancel
            if (ty < 50 && tx < 80) {
                waitFullRelease(ts);
                markTapAccepted();
                return OSAVal("");
            }
            // Keypad area
            if (ty >= gridY && ty < gridY + 4 * (btn + gap) &&
                tx >= gridX && tx < gridX + gridW) {
                int col = (tx - gridX) / (btn + gap);
                int row = (ty - gridY) / (btn + gap);
                if (col < 0 || col > 2 || row < 0 || row > 3) continue;
                char k = keys[row * 3 + col];
                if (k == '<') {
                    if (buf.length() > 0) buf.remove(buf.length() - 1);
                    drawDots();
                } else if (k == '>') {
                    waitFullRelease(ts);
                    markTapAccepted();
                    return OSAVal(buf);
                } else {
                    if ((int)buf.length() < maxDigits) buf += k;
                    drawDots();
                }
                waitFullRelease(ts);
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Privileged SDK — requires #exception true in script header
    // ═════════════════════════════════════════════════════════════════════════
    auto needException = [&](const char* fn) -> bool {
        if (isException) return true;
        setError(-1, String(fn) + " needs #exception");
        return false;
    };

    // sys.* — system mutation
    if (name == "sys.brightness") {
        if (!needException("sys.brightness")) return OSAVal();
        sysBrightness = constrain(iN(0), 10, 255);
        analogWrite(21, sysBrightness);
        Config::setInt("brightness", sysBrightness);
        Config::save();
        return OSAVal();
    }
    if (name == "sys.theme") {
        if (!needException("sys.theme")) return OSAVal();
        sysTheme = iN(0) ? 1 : 0;
        Config::setInt("theme", sysTheme);
        Config::save();
        return OSAVal();
    }
    if (name == "sys.wallpaper") {
        if (!needException("sys.wallpaper")) return OSAVal();
        Config::set("wallpaper_path", S(0));
        Config::save();
        return OSAVal();
    }
    if (name == "sys.reboot") {
        if (!needException("sys.reboot")) return OSAVal();
        delay(120);
        ESP.restart();
        return OSAVal();
    }
    if (name == "sys.notify") {
        if (!needException("sys.notify")) return OSAVal();
        extern void osa_notify(const char*);
        osa_notify(S(0).c_str());
        return OSAVal();
    }
    // app.launch(absPath) — unwinds this script and asks the host to load
    // another .osa instead of returning to home. Used by the Files app.
    if (name == "app.launch") {
        if (!needException("app.launch")) return OSAVal();
        pendingLaunch = S(0);
        exitFlag = true;
        return OSAVal();
    }

    // cfg.* — global config (poza per-app kv.*)
    if (name == "cfg.get") {
        if (!needException("cfg.get")) return OSAVal("");
        return OSAVal(Config::get(S(0), S(1, "")));
    }
    if (name == "cfg.set") {
        if (!needException("cfg.set")) return OSAVal();
        Config::set(S(0), a[1].toString());
        Config::save();
        return OSAVal();
    }
    if (name == "cfg.del") {
        if (!needException("cfg.del")) return OSAVal();
        // Config has no public delete — overwrite with empty as the practical
        // equivalent. Storage still keeps the key but consumers see "".
        Config::set(S(0), "");
        Config::save();
        return OSAVal();
    }

    // fs.* — file system access *outside* the per-app sandbox
    if (name == "fs.read") {
        if (!needException("fs.read")) return OSAVal("");
        if (!isSdReady) return OSAVal("");
        File f = SD.open(S(0));
        if (!f) return OSAVal("");
        String c = f.readString(); f.close();
        return OSAVal(c);
    }
    if (name == "fs.write") {
        if (!needException("fs.write")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        String p = S(0);
        SD.remove(p.c_str());
        File f = SD.open(p, FILE_WRITE);
        if (!f) return OSAVal(0.0);
        f.print(S(1)); f.close();
        return OSAVal(1.0);
    }
    if (name == "fs.append") {
        if (!needException("fs.append")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        File f = SD.open(S(0), FILE_APPEND); if (!f) return OSAVal(0.0);
        f.println(S(1)); f.close(); return OSAVal(1.0);
    }
    if (name == "fs.exists") {
        if (!needException("fs.exists")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.exists(S(0).c_str()) ? 1.0 : 0.0);
    }
    if (name == "fs.delete") {
        if (!needException("fs.delete")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.remove(S(0).c_str()) ? 1.0 : 0.0);
    }
    if (name == "fs.mkdir") {
        if (!needException("fs.mkdir")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.mkdir(S(0).c_str()) ? 1.0 : 0.0);
    }
    if (name == "fs.rmdir") {
        if (!needException("fs.rmdir")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.rmdir(S(0).c_str()) ? 1.0 : 0.0);
    }
    // fs.wipe(path) — recursive nuke. Mirrors SettingsApp::wipeSDCard.
    if (name == "fs.wipe") {
        if (!needException("fs.wipe")) return OSAVal(0.0);
        if (!isSdReady) return OSAVal(0.0);
        struct Rec {
            static void walk(const String& dirPath) {
                File dir = SD.open(dirPath);
                if (!dir || !dir.isDirectory()) return;
                File f = dir.openNextFile();
                while (f) {
                    String n = f.name();
                    int slash = n.lastIndexOf('/');
                    if (slash >= 0) n = n.substring(slash + 1);
                    String full = dirPath;
                    if (!full.endsWith("/")) full += "/";
                    full += n;
                    bool isDir = f.isDirectory();
                    f.close();
                    if (isDir) {
                        walk(full);
                        SD.rmdir(full.c_str());
                    } else {
                        SD.remove(full.c_str());
                    }
                    f = dir.openNextFile();
                }
                dir.close();
            }
        };
        Rec::walk(S(0));
        return OSAVal(1.0);
    }

    // ntp.sync() — kick off an NTP fetch right now (requires Wi-Fi connected)
    if (name == "ntp.sync") {
        if (!needException("ntp.sync")) return OSAVal(0.0);
        if (WiFi.status() != WL_CONNECTED) return OSAVal(0.0);
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
        struct tm t;
        bool ok = getLocalTime(&t, 3000);
        if (ok) {
            extern time_t sysLastNtpSync;
            sysNtpSynced = true; time(&sysLastNtpSync);
        }
        return OSAVal(ok ? 1.0 : 0.0);
    }

    // ── Wi-Fi control + scan (privileged) ────────────────────────────────────
    if (name == "wifi.enable") {
        if (!needException("wifi.enable")) return OSAVal();
        sysWiFiEnabled = true;
        WiFi.mode(WIFI_STA);
        Config::setInt("wifi", 1); Config::save();
        return OSAVal();
    }
    if (name == "wifi.disable") {
        if (!needException("wifi.disable")) return OSAVal();
        sysWiFiEnabled = false;
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        Config::setInt("wifi", 0); Config::save();
        return OSAVal();
    }
    if (name == "wifi.isEnabled") return OSAVal(sysWiFiEnabled ? 1.0 : 0.0);
    if (name == "wifi.scan") {
        if (!needException("wifi.scan")) return OSAVal(0.0);
        if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
        int n = WiFi.scanNetworks();
        return OSAVal((double)(n < 0 ? 0 : n));
    }
    if (name == "wifi.scanSsid") {
        if (!needException("wifi.scanSsid")) return OSAVal("");
        return OSAVal(WiFi.SSID(iN(0)));
    }
    if (name == "wifi.scanRssi") {
        if (!needException("wifi.scanRssi")) return OSAVal(0.0);
        return OSAVal((double)WiFi.RSSI(iN(0)));
    }
    if (name == "wifi.scanSecure") {
        if (!needException("wifi.scanSecure")) return OSAVal(0.0);
        return OSAVal(WiFi.encryptionType(iN(0)) != WIFI_AUTH_OPEN ? 1.0 : 0.0);
    }
    if (name == "wifi.connect") {
        if (!needException("wifi.connect")) return OSAVal(0.0);
        if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
        WiFi.begin(S(0).c_str(), S(1, "").c_str());
        unsigned long start = millis();
        while (millis() - start < 10000) {
            yield(); delay(100);
            if (WiFi.status() == WL_CONNECTED) return OSAVal(1.0);
        }
        return OSAVal(0.0);
    }
    if (name == "wifi.disconnect") {
        if (!needException("wifi.disconnect")) return OSAVal();
        WiFi.disconnect();
        return OSAVal();
    }
    if (name == "wifi.save") {
        if (!needException("wifi.save")) return OSAVal();
        // Stored encrypted using the same format Settings/main.cpp expect for
        // auto-reconnect: "ssid|password" XOR-obfuscated via Crypto.
        Config::set("net_0", Crypto::encrypt(S(0) + "|" + S(1, "")));
        Config::save();
        return OSAVal();
    }

    // ── Bluetooth control (privileged) ───────────────────────────────────────
    if (name == "bt.enable") {
        if (!needException("bt.enable")) return OSAVal();
        sysBTEnabled = true;
        SerialBT.begin("OpenOS");
        Config::setInt("bluetooth", 1); Config::save();
        return OSAVal();
    }
    if (name == "bt.disable") {
        if (!needException("bt.disable")) return OSAVal();
        sysBTEnabled = false;
        SerialBT.end();
        Config::setInt("bluetooth", 0); Config::save();
        return OSAVal();
    }
    if (name == "bt.enabled") return OSAVal(sysBTEnabled ? 1.0 : 0.0);

    // ── sys.setTime(h, m, s, day, mon, year) — sets RTC ─────────────────────
    if (name == "sys.setTime") {
        if (!needException("sys.setTime")) return OSAVal();
        struct tm t = {};
        t.tm_hour = iN(0);
        t.tm_min  = iN(1);
        t.tm_sec  = iN(2);
        t.tm_mday = iN(3);
        t.tm_mon  = iN(4) - 1;
        t.tm_year = iN(5) - 1900;
        t.tm_isdst = -1;
        struct timeval tv;
        tv.tv_sec  = mktime(&t);
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        return OSAVal();
    }

    // ── crypto.* — same XOR scheme used by native Settings/passcode ──────────
    if (name == "crypto.encrypt") {
        if (!needException("crypto.encrypt")) return OSAVal("");
        return OSAVal(Crypto::encrypt(S(0)));
    }
    if (name == "crypto.decrypt") {
        if (!needException("crypto.decrypt")) return OSAVal("");
        return OSAVal(Crypto::decrypt(S(0)));
    }

    // ── apps.* — manage every .osa app's metadata + permissions ─────────────
    {
        static const int APPS_MAX_SLOTS = 16;
        static String s_appsPaths[APPS_MAX_SLOTS];
        static String s_appsNames[APPS_MAX_SLOTS];
        static int    s_appsCount = 0;

        // Recursive helper — same shape as Settings::scanOsaDir.
        struct ScanCtx {
            static void walk(const String& dirPath, int depth) {
                if (s_appsCount >= APPS_MAX_SLOTS || depth > 1) return;
                File dir = SD.open(dirPath);
                if (!dir || !dir.isDirectory()) return;
                File f = dir.openNextFile();
                while (f && s_appsCount < APPS_MAX_SLOTS) {
                    String full = f.name();
                    if (!full.startsWith("/")) full = "/" + full;
                    if (full.startsWith("/apps/")) { f = dir.openNextFile(); continue; }
                    if (f.isDirectory()) {
                        if (depth == 0) walk(full, 1);
                    } else {
                        String lower = full; lower.toLowerCase();
                        if (lower.endsWith(".osa")) {
                            s_appsPaths[s_appsCount] = full;
                            s_appsNames[s_appsCount] = OSARuntime::readAppNameFromFile(full);
                            s_appsCount++;
                        }
                    }
                    f = dir.openNextFile();
                }
                dir.close();
            }
        };

        if (name == "apps.scan") {
            if (!needException("apps.scan")) return OSAVal(0.0);
            s_appsCount = 0;
            if (isSdReady) ScanCtx::walk("/", 0);
            return OSAVal((double)s_appsCount);
        }
        if (name == "apps.name") {
            int i = iN(0);
            if (i < 0 || i >= s_appsCount) return OSAVal("");
            return OSAVal(s_appsNames[i]);
        }
        if (name == "apps.path") {
            int i = iN(0);
            if (i < 0 || i >= s_appsCount) return OSAVal("");
            return OSAVal(s_appsPaths[i]);
        }
        if (name == "apps.needsPerm") {
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal(0.0);
            uint8_t mask = OSARuntime::readRequiredPermsFromFile(s_appsPaths[i]);
            return OSAVal((mask & bit) ? 1.0 : 0.0);
        }
        if (name == "apps.hasPerm") {
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal(0.0);
            int stored = Config::getInt(OSARuntime::permKeyForName(s_appsNames[i]), 0);
            return OSAVal((stored & 0x0F & bit) ? 1.0 : 0.0);
        }
        if (name == "apps.togglePerm") {
            if (!needException("apps.togglePerm")) return OSAVal();
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal();
            String key   = OSARuntime::permKeyForName(s_appsNames[i]);
            int stored   = Config::getInt(key, 0);
            uint8_t gr   = stored & 0x0F;
            if (gr & bit) {
                stored &= ~(int)bit;
                stored |=  ((int)bit << 4);  // deny
            } else {
                stored |=  (int)bit;
                stored &= ~((int)bit << 4);  // re-allow
            }
            Config::setInt(key, stored);
            Config::save();
            return OSAVal();
        }
    }

    // ── bmp.thumb(absPath, x, y, w, h) — render 24-bit BMP scaled to w×h ─────
    // Same algorithm as SettingsApp::drawBmpThumbnail. Public (not privileged)
    // so any script can render images.
    if (name == "bmp.thumb") {
        if (!isSdReady) return OSAVal(0.0);
        String path = S(0);
        int x = iN(1), y = iN(2), w = iN(3), h = iN(4);
        File bmpFile = SD.open(path);
        if (!bmpFile) return OSAVal(0.0);

        auto read16 = [&](File& f) -> uint16_t {
            uint16_t r; ((uint8_t*)&r)[0] = f.read(); ((uint8_t*)&r)[1] = f.read();
            return r;
        };
        auto read32 = [&](File& f) -> uint32_t {
            uint32_t r;
            ((uint8_t*)&r)[0] = f.read(); ((uint8_t*)&r)[1] = f.read();
            ((uint8_t*)&r)[2] = f.read(); ((uint8_t*)&r)[3] = f.read();
            return r;
        };
        if (read16(bmpFile) != 0x4D42) { bmpFile.close(); return OSAVal(0.0); }
        read32(bmpFile); read32(bmpFile);
        uint32_t imageOffset = read32(bmpFile);
        read32(bmpFile);
        int32_t srcW = read32(bmpFile);
        int32_t srcH = read32(bmpFile);
        bool flip = true;
        if (srcH < 0) { srcH = -srcH; flip = false; }
        if (read16(bmpFile) != 1 || read16(bmpFile) != 24) {
            bmpFile.close(); return OSAVal(0.0);
        }
        uint32_t rowSize = (srcW * 3 + 3) & ~3;
        float stepX = (float)srcW / w;
        float stepY = (float)srcH / h;
        uint8_t* sd = (uint8_t*)malloc(rowSize);
        uint16_t* lcd = (uint16_t*)malloc(w * sizeof(uint16_t));
        if (sd && lcd) {
            for (int ty = 0; ty < h; ty++) {
                int srcY = (int)(ty * stepY); if (srcY >= srcH) srcY = srcH - 1;
                int fileRow = flip ? (srcH - 1 - srcY) : srcY;
                bmpFile.seek(imageOffset + fileRow * rowSize);
                bmpFile.read(sd, rowSize);
                for (int tx = 0; tx < w; tx++) {
                    int srcX = (int)(tx * stepX); if (srcX >= srcW) srcX = srcW - 1;
                    uint8_t b = sd[srcX * 3];
                    uint8_t g = sd[srcX * 3 + 1];
                    uint8_t r = sd[srcX * 3 + 2];
                    lcd[tx] = tft->color565(r, g, b);
                }
                tft->startWrite();
                tft->setAddrWindow(x, y + ty, w, 1);
                tft->pushColors(lcd, w, true);
                tft->endWrite();
            }
        }
        if (sd)  free(sd);
        if (lcd) free(lcd);
        bmpFile.close();
        return OSAVal(1.0);
    }

    // ── fs.list(absPath) → pipe-separated entries, directories suffixed "/" ──
    if (name == "fs.list") {
        if (!needException("fs.list")) return OSAVal("");
        if (!isSdReady) return OSAVal("");
        File dir = SD.open(S(0));
        if (!dir || !dir.isDirectory()) return OSAVal("");
        String out;
        File f = dir.openNextFile();
        int n = 0;
        while (f && n < 32) {
            if (n > 0) out += "|";
            String nm = f.name();
            int slash = nm.lastIndexOf('/');
            if (slash >= 0) nm = nm.substring(slash + 1);
            out += nm;
            if (f.isDirectory()) out += "/";
            n++;
            f = dir.openNextFile();
        }
        dir.close();
        return OSAVal(out);
    }

    // Unknown built-in (user funcs were handled at the top)
    setError(-1, "Unknown: " + name);
    return OSAVal();
}
