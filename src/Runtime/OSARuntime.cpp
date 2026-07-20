#include "OSARuntime.h"
#include "PackageManager.h"
#include "../Config.h"
#include "../Applications/Theme.h"
#include "../Applications/Crypto.h"
#include "../Applications/Wallpaper.h"
#include "../Applications/Home.h"
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <BluetoothSerial.h>
#include <math.h>
#include <new>

// Arduino String intentionally keeps capacity when assigned "" so it can be
// reused. Runtime recycle needs the opposite: return large script/HTTP buffers
// to the heap. Explicit lifetime restart is safe here because these objects are
// immediately reconstructed at the same address.
static void releaseStringStorage(String& value) {
    value.~String();
    new (&value) String();
}

static void releaseValueStorage(OSAVal& value) {
    value.~OSAVal();
    new (&value) OSAVal();
}

static bool userPackageFromEntry(const String& path, String& id,
                                 String* packagePrefix = nullptr) {
    static const char* root = "/packages/";
    static const int rootLength = 10;
    if (!path.startsWith(root)) return false;
    int slash = path.indexOf('/', rootLength);
    if (slash <= rootLength) return false;
    id = path.substring(rootLength, slash);
    if (packagePrefix) *packagePrefix = root + id + "/";
    return true;
}

static bool isLooseUserScript(const String& path) {
    String lower = path;
    lower.toLowerCase();
    bool script = lower.endsWith(".osa") || lower.endsWith(".osac");
    return script && path.startsWith("/") &&
           !path.startsWith("/system/") &&
           !path.startsWith("/packages/") &&
           !path.startsWith("/apps/");
}

extern bool isSdReady;
extern int  sysTheme;
extern int  sysBrightness;
extern bool sysNtpSynced;
extern bool sysWiFiEnabled;
extern bool sysBTEnabled;
extern Home home;
extern bool osaSetBluetoothEnabled(bool enabled);
extern const char* osaBluetoothLastError();
// Folder + home animation helpers exposed by main.cpp. Used by OSA-side
// home.osa so the script can trigger the same folder/open-anim UX without
// owning the C++ Home/Folder data structures itself.
extern bool osaMakeFolder(int idx);
extern bool osaDeleteFolder(int idx);
extern bool osaAddToFolder(int folderIdx, int appIdx);
extern void osaPlayOpenAnim(int idx);

// ─── Inline keyboard (formerly OSKeyboard) ───────────────────────────────────
// Was a standalone Applications/OSKeyboard.* class — now lives here as a
// runtime-private helper so input() can use it without the rest of the
// codebase depending on a separate file. lockscreen runs as an OSA script
// and uses input() / ui.numpad, so this is the only consumer left.
namespace {

// Native SDK calls may spend most of their time transferring pixels over SPI,
// waiting for touch input, reading SD, or doing network I/O. That time must not
// be charged to the tree-walker's CPU budget. A separate loop-operation limit
// still catches scripts that spin forever by repeatedly calling tiny builtins.
class BuiltinBudgetGuard {
public:
    explicit BuiltinBudgetGuard(uint32_t& sliceStarted)
        : started(sliceStarted) {}
    ~BuiltinBudgetGuard() { started = millis(); }

private:
    uint32_t& started;
};

static const char kKeysL[3][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l',' '},
    {'z','x','c','v','b','n','m',' ',' ',' '}
};
static const char kKeysU[3][10] = {
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L',' '},
    {'Z','X','C','V','B','N','M',' ',' ',' '}
};
static const char kKeysN[3][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'-','/',':',';','(',')','$','&','@',' '},
    {'.',',','?','!','\'','"','=',' ',' ',' '}
};

class InlineKbd {
public:
    InlineKbd(TFT_eSPI* t, XPT2046_Touchscreen* s) : tft(t), ts(s), layoutState(0) {}

    void draw() {
        tft->fillRect(0, 160, 240, 160, Theme::kbdBg());
        for (int i = 0; i < 10; i++)
            drawKey(2 + i * 24, 164, 20, 34, String(getCh(0, i)), false);
        for (int i = 0; i < 9; i++)
            drawKey(14 + i * 24, 202, 20, 34, String(getCh(1, i)), false);
        drawKey(2, 240, 32, 34, layoutState == 1 ? "ABC" : "abc", true);
        for (int i = 0; i < 7; i++)
            drawKey(38 + i * 24, 240, 20, 34, String(getCh(2, i)), false);
        drawKey(206, 240, 32, 34, "<X", true);
        drawKey(2, 278, 54, 34, layoutState == 2 ? "ABC" : "123", true);
        drawKey(60, 278, 120, 34, "space", false);
        drawKey(184, 278, 54, 34, "Enter", true);
    }

    char update() {
        if (!ts->touched()) return '\0';
        TS_Point p = ts->getPoint();
        int x = map(p.x, 300, 3800, 0, 240);
        int y = map(p.y, 300, 3800, 0, 320);
        if (y < 160) return '\0';

        char result = '\0';
        bool redraw = false;

        if (y >= 160 && y < 200) {
            int col = x / 24;
            if (col >= 0 && col < 10) result = getCh(0, col);
        } else if (y >= 200 && y < 240) {
            if (x > 12 && x < 228) {
                int col = (x - 12) / 24;
                if (col >= 0 && col < 9) result = getCh(1, col);
            }
        } else if (y >= 240 && y < 280) {
            if (x < 36) {
                layoutState = (layoutState == 1) ? 0 : 1;
                redraw = true;
            } else if (x > 204) {
                result = '\b';
            } else {
                int col = (x - 36) / 24;
                if (col >= 0 && col < 7) result = getCh(2, col);
            }
        } else if (y >= 280 && y < 320) {
            if (x < 60) {
                layoutState = (layoutState == 2) ? 0 : 2;
                redraw = true;
            } else if (x > 180) {
                result = '\n';
            } else {
                result = ' ';
            }
        }

        if (redraw) {
            draw();
            delay(200);
        } else if (result != '\0') {
            if (layoutState == 1) { layoutState = 0; draw(); }
            delay(200);
        }
        return result;
    }

private:
    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;
    int layoutState;

    char getCh(int row, int col) {
        if (layoutState == 0) return kKeysL[row][col];
        if (layoutState == 1) return kKeysU[row][col];
        return kKeysN[row][col];
    }

    void drawKey(int x, int y, int w, int h, String label, bool isSpecial) {
        uint16_t bg = isSpecial ? Theme::kbdSpec() : Theme::kbdKey();
        tft->fillRoundRect(x, y + 2, w, h, 4, Theme::kbdShadow());
        tft->fillRoundRect(x, y,     w, h, 4, bg);
        tft->setTextColor(Theme::kbdText());
        tft->setTextDatum(MC_DATUM);
        tft->setTextFont(2); tft->setTextSize(1);
        tft->drawString(label, x + w / 2, y + h / 2 + 2);
    }
};

static int popupNextUtf8(const String& text, int pos) {
    int next = pos + 1;
    while (next < (int)text.length() &&
           (((uint8_t)text[next] & 0xC0) == 0x80)) ++next;
    return next;
}

static void popupAddEllipsis(TFT_eSPI* tft, String& line, int maxWidth) {
    static const char* dots = "...";
    while (line.length() > 0 && tft->textWidth(line + dots) > maxWidth) {
        int cut = (int)line.length() - 1;
        while (cut > 0 && (((uint8_t)line[cut] & 0xC0) == 0x80)) --cut;
        line.remove((unsigned int)cut);
    }
    line.trim();
    line += dots;
}

static String popupFitLine(TFT_eSPI* tft, const String& text, int maxWidth) {
    if (tft->textWidth(text) <= maxWidth) return text;
    String fitted = text;
    popupAddEllipsis(tft, fitted, maxWidth);
    return fitted;
}

static bool popupStoreLine(String lines[], int maxLines, int& lineCount,
                           String& current) {
    if (lineCount >= maxLines) return false;
    current.trim();
    lines[lineCount++] = current;
    current = "";
    return true;
}

static bool popupAppendWord(TFT_eSPI* tft, const String& word,
                            String lines[], int maxLines, int& lineCount,
                            String& current, int maxWidth) {
    if (word.length() == 0) return true;

    if (current.length() > 0) {
        String candidate = current + " " + word;
        if (tft->textWidth(candidate) <= maxWidth) {
            current = candidate;
            return true;
        }
        if (!popupStoreLine(lines, maxLines, lineCount, current)) return false;
    }

    // A URL, path or error token may contain no spaces. Split it at UTF-8
    // character boundaries so even one very long word cannot escape the box.
    int start = 0;
    while (start < (int)word.length()) {
        int end = start;
        int lastFit = start;
        while (end < (int)word.length()) {
            int next = popupNextUtf8(word, end);
            if (tft->textWidth(word.substring(start, next)) > maxWidth) break;
            lastFit = next;
            end = next;
        }

        if (lastFit == start) lastFit = popupNextUtf8(word, start);
        String part = word.substring(start, lastFit);
        start = lastFit;
        if (start < (int)word.length()) {
            if (!popupStoreLine(lines, maxLines, lineCount, part)) return false;
        } else {
            current = part;
        }
    }
    return true;
}

static int popupWrapBody(TFT_eSPI* tft, const String& body1,
                         const String& body2, String lines[], int maxLines,
                         int maxWidth) {
    String text = body1;
    if (body2.length() > 0) {
        if (text.length() > 0) text += '\n';
        text += body2;
    }

    int lineCount = 0;
    String current;
    bool truncated = false;
    int pos = 0;
    while (pos < (int)text.length()) {
        char c = text[pos];
        if (c == '\r') { ++pos; continue; }
        if (c == '\n') {
            if (!popupStoreLine(lines, maxLines, lineCount, current)) {
                truncated = true;
                break;
            }
            ++pos;
            continue;
        }
        if (c == ' ' || c == '\t') { ++pos; continue; }

        int wordStart = pos;
        while (pos < (int)text.length() && text[pos] != ' ' &&
               text[pos] != '\t' && text[pos] != '\r' && text[pos] != '\n') {
            pos = popupNextUtf8(text, pos);
        }
        if (!popupAppendWord(tft, text.substring(wordStart, pos), lines,
                             maxLines, lineCount, current, maxWidth)) {
            truncated = true;
            break;
        }
    }

    if (!truncated && (current.length() > 0 || lineCount == 0) &&
        !popupStoreLine(lines, maxLines, lineCount, current)) {
        truncated = true;
    }
    if (truncated && lineCount > 0)
        popupAddEllipsis(tft, lines[lineCount - 1], maxWidth);
    return lineCount;
}

struct WrappedDrawContext {
    TFT_eSPI* tft;
    TFT_eSprite* sprite;
    int x;
    int y;
    int lineHeight;
    int scroll;
    int clipTop;
    int clipBottom;
    int fontHeight;
};

static void drawWrappedLine(const String& line, int index, void* opaque) {
    WrappedDrawContext* context = (WrappedDrawContext*)opaque;
    int y = context->y + index * context->lineHeight - context->scroll;
    if (y < context->clipTop || y + context->fontHeight > context->clipBottom)
        return;
    if (context->sprite) context->sprite->drawString(line, context->x, y);
    else                 context->tft->drawString(line, context->x, y);
}

// Iterates wrapped lines without allocating an array proportional to the text
// length. This keeps a 10 KB store description practical on a non-PSRAM ESP32.
static int visitWrappedText(TFT_eSPI* metrics, const String& text, int maxWidth,
                            int maxLines,
                            void (*visitor)(const String&, int, void*),
                            void* context) {
    if (maxWidth < 1 || text.length() == 0) return 0;
    int lineCount = 0;
    int pos = 0;
    const int length = text.length();
    const int limit = maxLines > 0 ? min(maxLines, 10000) : 10000;

    while (pos < length && lineCount < limit) {
        while (pos < length &&
               (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r')) ++pos;
        if (pos >= length) break;

        if (text[pos] == '\n') {
            if (visitor) visitor(String(""), lineCount, context);
            ++lineCount;
            ++pos;
            continue;
        }

        int start = pos;
        int scan = pos;
        int lastFit = pos;
        int lastBreak = -1;
        int end = pos;
        bool decided = false;

        while (scan < length) {
            char c = text[scan];
            if (c == '\n') {
                end = scan;
                pos = scan + 1;
                decided = true;
                break;
            }
            int next = popupNextUtf8(text, scan);
            if (c == ' ' || c == '\t' || c == '\r') lastBreak = scan;
            if (metrics->textWidth(text.substring(start, next)) > maxWidth) {
                if (lastBreak > start) {
                    end = lastBreak;
                    pos = lastBreak;
                    while (pos < length &&
                           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r')) ++pos;
                } else {
                    end = lastFit > start ? lastFit : next;
                    pos = end;
                }
                decided = true;
                break;
            }
            lastFit = next;
            scan = next;
        }
        if (!decided) {
            end = length;
            pos = length;
        }
        while (end > start &&
               (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;

        String line = text.substring(start, end);
        bool truncated = lineCount + 1 >= limit && pos < length;
        if (truncated) popupAddEllipsis(metrics, line, maxWidth);
        if (visitor) visitor(line, lineCount, context);
        ++lineCount;
        if (truncated) break;
    }
    return lineCount;
}

} // anonymous namespace

// Forward — defined below near the block-navigation helpers.
static bool isBlockOpen(const String& t);

// Forward-declare notification push
namespace _osa_notify { void push(const char* msg); }

// ─── Permission table ────────────────────────────────────────────────────────
const OSAPermDesc OSA_PERM_TABLE[] = {
    { OSA_PERM_NOTIFY,  "Notifications",       "notify() \xE2\x80\x94 system banners" },
    { OSA_PERM_NETWORK, "Network",             "http.get(), http.post() \xE2\x80\x94 outbound" },
    { OSA_PERM_SYSTEM,  "System Settings",     "setbright(), setwallpaper()" },
    { OSA_PERM_OVERLAY, "Draw over other apps","overlay.draw() \xE2\x80\x94 banners, alerts on top of any screen" },
};
const int OSA_PERM_TABLE_COUNT = sizeof(OSA_PERM_TABLE) / sizeof(OSA_PERM_TABLE[0]);

// CV(...) routes a drawing call to the active sprite (off-screen buffer) if
// gfx.begin() opened one, else direct to the TFT. TFT_eSPI's drawing methods
// aren't virtual, so we have to dispatch at the call site explicitly.
#define CV(call) do { \
    if (activeSprite) activeSprite->call; \
    else              tft->call; \
} while (0)

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
static String s_httpError;
static String s_ioError;

// Native widget caches must be released with the runtime. Function-local
// statics used to retain their String capacities after Settings was closed,
// taking heap away from the next application and from HTTPS.
static const int RICH_MAX_ROWS = 16;
static String   s_rmTitle;
static String   s_rmTitles[RICH_MAX_ROWS];
static String   s_rmLetters[RICH_MAX_ROWS];
static uint16_t s_rmColors[RICH_MAX_ROWS];
static String   s_rmValues[RICH_MAX_ROWS];
static int      s_rmCount = 0;
static bool     s_rmShowBack = false;

static const int APPS_MAX_SLOTS = 16;
static String s_appsPaths[APPS_MAX_SLOTS];
static String s_appsNames[APPS_MAX_SLOTS];
static int    s_appsCount = 0;

static void clearRichMenuCache() {
    releaseStringStorage(s_rmTitle);
    for (int i = 0; i < RICH_MAX_ROWS; ++i) {
        releaseStringStorage(s_rmTitles[i]);
        releaseStringStorage(s_rmLetters[i]);
        releaseStringStorage(s_rmValues[i]);
        s_rmColors[i] = 0;
    }
    s_rmCount = 0;
    s_rmShowBack = false;
}

static void clearAppsScanCache() {
    for (int i = 0; i < APPS_MAX_SLOTS; ++i) {
        releaseStringStorage(s_appsPaths[i]);
        releaseStringStorage(s_appsNames[i]);
    }
    s_appsCount = 0;
}

static constexpr size_t OSA_HTTP_MAX_BODY = 24 * 1024;
static constexpr size_t OSA_HTTP_MAX_SEND = 24 * 1024;
static constexpr size_t OSA_MAX_FILE_READ = 32 * 1024;

// HTTPClient::getString() grows an unbounded String. This sink is passed to
// writeToStream(), so both fixed-length and chunked responses stop at the same
// hard cap before they can exhaust or fragment the ESP32 heap.
class BoundedStringStream : public Stream {
public:
    explicit BoundedStringStream(size_t maximum) : maxBytes(maximum) {}

    bool begin(size_t hint) {
        if (hint > maxBytes) hint = maxBytes;
        return hint == 0 || body.reserve(hint);
    }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* data, size_t length) override {
        if (overflowed || length > maxBytes - body.length()) {
            overflowed = true;
            setWriteError();
            return 0;
        }
        if (length > 0 && !body.concat((const char*)data, (unsigned int)length)) {
            allocationFailed = true;
            setWriteError();
            return 0;
        }
        return length;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    String take() { return static_cast<String&&>(body); }
    bool tooLarge() const { return overflowed; }
    bool outOfMemory() const { return allocationFailed; }

private:
    String body;
    size_t maxBytes;
    bool overflowed = false;
    bool allocationFailed = false;
};

static bool readFileBounded(const String& path, size_t offset, size_t requested,
                            bool explicitLength, String& out) {
    s_ioError = "";
    File f = SD.open(path);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s_ioError = "File not found";
        return false;
    }

    size_t total = (size_t)f.size();
    if (offset > total) {
        f.close();
        s_ioError = "Offset outside file";
        return false;
    }
    size_t available = total - offset;
    size_t amount = explicitLength ? min(requested, available) : available;
    if (requested > OSA_MAX_FILE_READ || amount > OSA_MAX_FILE_READ) {
        f.close();
        s_ioError = "Read exceeds 32768 byte limit";
        return false;
    }
    if (!f.seek(offset)) {
        f.close();
        s_ioError = "Seek failed";
        return false;
    }

    out = "";
    if (amount > 0 && !out.reserve(amount)) {
        f.close();
        s_ioError = "Not enough memory";
        return false;
    }
    uint8_t buffer[512];
    size_t left = amount;
    while (left > 0) {
        size_t want = min(left, sizeof(buffer));
        int got = f.read(buffer, want);
        if (got <= 0 || !out.concat((const char*)buffer, (unsigned int)got)) {
            f.close();
            out = "";
            s_ioError = got <= 0 ? "Unexpected end of file" : "Not enough memory";
            return false;
        }
        left -= (size_t)got;
        yield();
    }
    f.close();
    return true;
}

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
static bool jsonHex4(const String& value, int start, uint32_t& codepoint) {
    if (start < 0 || start + 4 > (int)value.length()) return false;
    codepoint = 0;
    for (int i = 0; i < 4; ++i) {
        char c = value[start + i];
        if (!isxdigit((unsigned char)c)) return false;
        codepoint = (codepoint << 4) |
                    (c >= '0' && c <= '9' ? c - '0' :
                     c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10);
    }
    return true;
}

static bool jsonAppendUtf8(String& output, uint32_t codepoint) {
    char encoded[4]; int count = 0;
    if (codepoint <= 0x7F) encoded[count++] = (char)codepoint;
    else if (codepoint <= 0x7FF) {
        encoded[count++] = (char)(0xC0 | (codepoint >> 6));
        encoded[count++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        encoded[count++] = (char)(0xE0 | (codepoint >> 12));
        encoded[count++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[count++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        encoded[count++] = (char)(0xF0 | (codepoint >> 18));
        encoded[count++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        encoded[count++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[count++] = (char)(0x80 | (codepoint & 0x3F));
    } else return false;
    return output.concat(encoded, (unsigned int)count);
}

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
            else if (e == 'b')  out += '\b';
            else if (e == 'f')  out += '\f';
            else if (e == '"')  out += '"';
            else if (e == '\\') out += '\\';
            else if (e == '/')  out += '/';
            else if (e == 'u' && i + 5 < len - 1) {
                uint32_t cp = 0;
                if (!jsonHex4(v, i + 2, cp)) return String("");
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // UTF-16 surrogate pair used by many JSON encoders for
                    // emoji and code points above U+FFFF.
                    if (i + 7 >= len - 1 || v[i + 2] != '\\' || v[i + 3] != 'u')
                        return String("");
                    uint32_t low = 0;
                    if (!jsonHex4(v, i + 4, low) || low < 0xDC00 || low > 0xDFFF)
                        return String("");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return String("");
                }
                if (!jsonAppendUtf8(out, cp)) return String("");
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
struct JsonSpan {
    int start = -1;
    int end = -1;
    JsonSpan() = default;
    JsonSpan(int spanStart, int spanEnd) : start(spanStart), end(spanEnd) {}
    bool valid() const { return start >= 0 && end >= start; }
};

static JsonSpan jsonObjectFieldSpan(const String& source, JsonSpan object,
                                    const String& name) {
    int pos = jsonSkipWs(source, object.start);
    if (!object.valid() || pos >= object.end || source[pos] != '{') return {};
    pos++;
    while (pos < object.end) {
        pos = jsonSkipWs(source, pos);
        if (pos >= object.end || source[pos] == '}' || source[pos] != '"') return {};
        int keyStart = ++pos;
        while (pos < object.end && source[pos] != '"') {
            if (source[pos] == '\\' && pos + 1 < object.end) pos++;
            pos++;
        }
        if (pos >= object.end) return {};
        String key = source.substring(keyStart, pos);
        pos++;
        pos = jsonSkipWs(source, pos);
        if (pos >= object.end || source[pos++] != ':') return {};
        pos = jsonSkipWs(source, pos);
        int valueStart = pos;
        int valueEnd = jsonSkipValue(source, pos);
        if (valueEnd > object.end) return {};
        if (key == name) return { valueStart, valueEnd };
        pos = jsonSkipWs(source, valueEnd);
        if (pos < object.end && source[pos] == ',') { pos++; continue; }
        return {};
    }
    return {};
}

static JsonSpan jsonArrayElementSpan(const String& source, JsonSpan array, int index) {
    int pos = jsonSkipWs(source, array.start);
    if (!array.valid() || index < 0 || pos >= array.end || source[pos] != '[') return {};
    pos++;
    int current = 0;
    while (pos < array.end) {
        pos = jsonSkipWs(source, pos);
        if (pos >= array.end || source[pos] == ']') return {};
        int valueStart = pos;
        int valueEnd = jsonSkipValue(source, pos);
        if (valueEnd > array.end) return {};
        if (current++ == index) return { valueStart, valueEnd };
        pos = jsonSkipWs(source, valueEnd);
        if (pos < array.end && source[pos] == ',') { pos++; continue; }
        return {};
    }
    return {};
}

static JsonSpan jsonWalkSpan(const String& source, const String& path) {
    int rootStart = jsonSkipWs(source, 0);
    JsonSpan current { rootStart, jsonSkipValue(source, rootStart) };
    if (!current.valid() || current.end <= current.start) return {};
    if (path.length() == 0) return current;
    int segmentStart = 0;
    for (int i = 0; i <= (int)path.length(); ++i) {
        if (i != (int)path.length() && path[i] != '.') continue;
        String segment = path.substring(segmentStart, i);
        segmentStart = i + 1;
        if (segment.length() == 0) continue;
        bool numeric = true;
        for (size_t k = 0; k < segment.length(); ++k) {
            if (!isdigit((unsigned char)segment[k])) { numeric = false; break; }
        }
        current = numeric ? jsonArrayElementSpan(source, current, segment.toInt())
                          : jsonObjectFieldSpan(source, current, segment);
        if (!current.valid()) return {};
    }
    return current;
}

static String jsonWalkPathLegacy(const String& src, const String& path) {
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

static String jsonWalkPath(const String& source, const String& path) {
    JsonSpan span = jsonWalkSpan(source, path);
    return span.valid() ? source.substring(span.start, span.end) : String("");
}

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
// Bytecode VM — Phase A skeleton
// ═══════════════════════════════════════════════════════════════════════════════

void OSABytecode::clear() {
    codeLen = numPoolLen = strPoolLen = namePoolLen = 0;
    setupEnd = 0;
    loopStart = loopEnd = -1;
    valid = false;
    buildError = OSA_BCERR_NONE;
    for (int i = 0; i < OSA_STR_CONST;  i++) releaseStringStorage(strPool[i]);
    for (int i = 0; i < OSA_NAME_CONST; i++) releaseStringStorage(namePool[i]);
}

int OSARuntime::bcAddNum(double v) {
    for (int i = 0; i < bc.numPoolLen; i++)
        if (bc.numPool[i] == v) return i;
    if (bc.numPoolLen >= OSA_NUM_CONST) {
        bc.buildError = OSA_BCERR_NUM_POOL_FULL;
        return -1;
    }
    bc.numPool[bc.numPoolLen] = v;
    return bc.numPoolLen++;
}

int OSARuntime::bcAddStr(const String& s) {
    for (int i = 0; i < bc.strPoolLen; i++)
        if (bc.strPool[i] == s) return i;
    if (bc.strPoolLen >= OSA_STR_CONST) {
        bc.buildError = OSA_BCERR_STR_POOL_FULL;
        return -1;
    }
    bc.strPool[bc.strPoolLen] = s;
    return bc.strPoolLen++;
}

int OSARuntime::bcAddName(const String& s) {
    for (int i = 0; i < bc.namePoolLen; i++)
        if (bc.namePool[i] == s) return i;
    if (bc.namePoolLen >= OSA_NAME_CONST) {
        bc.buildError = OSA_BCERR_NAME_POOL_FULL;
        return -1;
    }
    bc.namePool[bc.namePoolLen] = s;
    return bc.namePoolLen++;
}

bool OSARuntime::vmPush(OSAVal v) {
    // Once any VM stack operation fails, never mutate the stack again during
    // the remainder of the current opcode. exec() observes vmHalted and
    // unwinds immediately.
    if (vmHalted) return false;
    if (vmSp >= OSA_STACK_SIZE) {
        setError(-1, "VM stack overflow");
        vmHalted = true;
        return false;
    }
    vmStack[vmSp++] = static_cast<OSAVal&&>(v);
    return true;
}

OSAVal OSARuntime::vmPop() {
    if (vmHalted) return OSAVal();
    if (vmSp == 0) {
        setError(-1, "VM stack underflow");
        vmHalted = true;
        return OSAVal();
    }
    return static_cast<OSAVal&&>(vmStack[--vmSp]);
}

int OSARuntime::bcFindMatchingEnd(int n) { return findMatchingEnd(n); }
int OSARuntime::bcFindNextBranch(int n)  { return findNextBranch(n); }

// ─── .osac binary format helpers (used by serializeOsac / loadOsac) ───────
static const char    OSAC_MAGIC[4] = { 'O', 'S', 'A', 'C' };
static const uint8_t OSAC_VERSION  = 1;
static void w8(File& f, uint8_t v)  { f.write(v); }
static void w16(File& f, uint16_t v){ f.write((uint8_t)(v & 0xFF)); f.write((uint8_t)(v >> 8)); }
static void wD(File& f, double v) {
    uint8_t* p = (uint8_t*)&v;
    for (int i = 0; i < 8; i++) f.write(p[i]);
}
static void wS(File& f, const String& s) {
    uint16_t n = (uint16_t)s.length();
    w16(f, n);
    for (uint16_t i = 0; i < n; i++) f.write((uint8_t)s[i]);
}
static bool readExact(File& f, void* dst, size_t len) {
    return len == 0 || f.read((uint8_t*)dst, len) == (int)len;
}
static bool readLineLimited(File& f, String& out, size_t maximum) {
    out = "";
    while (f.available()) {
        int value = f.read();
        if (value < 0 || value == '\n') return true;
        if (value == '\r') continue;
        if (out.length() >= maximum || !out.concat((char)value)) {
            while (f.available()) {
                int rest = f.read();
                if (rest < 0 || rest == '\n') break;
            }
            out = "";
            return false;
        }
    }
    return true;
}
static bool r8(File& f, uint8_t& out) {
    int v = f.read();
    if (v < 0) return false;
    out = (uint8_t)v;
    return true;
}
static bool r16(File& f, uint16_t& out) {
    uint8_t a, b;
    if (!r8(f, a) || !r8(f, b)) return false;
    out = (uint16_t)a | ((uint16_t)b << 8);
    return true;
}
static bool rD(File& f, double& out) {
    return readExact(f, &out, sizeof(out));
}
static bool rS(File& f, String& out, uint16_t maxLen) {
    uint16_t n;
    if (!r16(f, n) || n > maxLen ||
        (uint32_t)f.position() + n > (uint32_t)f.size()) return false;
    out = "";
    if (!out.reserve(n)) return false;
    char buf[64];
    uint16_t left = n;
    while (left > 0) {
        size_t take = min((size_t)left, sizeof(buf));
        if (!readExact(f, buf, take) || !out.concat(buf, (unsigned int)take))
            return false;
        left -= (uint16_t)take;
    }
    return true;
}

// Extracted from `#appColor "#RRGGBB"` if present; defaults to 0.
static uint16_t parseAppColor(const String* lines, int lineCount, uint16_t fallback = 0) {
    for (int i = 0; i < lineCount; i++) {
        String t = lines[i]; t.trim();
        if (!t.startsWith("#appColor")) continue;
        int q1 = t.indexOf('"');
        int q2 = t.lastIndexOf('"');
        if (q1 < 0 || q2 <= q1) continue;
        String hex = t.substring(q1 + 1, q2);
        if (hex.length() > 0 && hex[0] == '#') hex = hex.substring(1);
        if (hex.length() != 6) continue;
        long v = strtol(hex.c_str(), nullptr, 16);
        uint8_t r = (uint8_t)((v >> 16) & 0xFF);
        uint8_t g = (uint8_t)((v >> 8) & 0xFF);
        uint8_t b = (uint8_t)(v & 0xFF);
        return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    }
    return fallback;
}

bool OSARuntime::serializeOsac(const String& dstPath) {
    if (!bc.valid) return false;
    if (!isSdReady) return false;
    SD.remove(dstPath.c_str());
    File f = SD.open(dstPath, FILE_WRITE);
    if (!f) return false;

    // Header
    for (int i = 0; i < 4; i++) f.write(OSAC_MAGIC[i]);
    w8(f, OSAC_VERSION);
    wS(f, appName);
    w16(f, parseAppColor(lines, lineCount, 0));
    bool isApp = false;
    for (int i = 0; i < lineCount; i++) {
        String t = lines[i]; t.trim();
        if (t.startsWith("#isApp")) {
            isApp = (t.indexOf("true") > 0);
            break;
        }
    }
    w8(f, isApp ? 1 : 0);
    w8(f, isException ? 1 : 0);
    w8(f, requiredPerms);

    // Bytecode body
    w16(f, (uint16_t)bc.codeLen);
    w16(f, (uint16_t)bc.setupEnd);
    w16(f, (uint16_t)(bc.loopStart & 0xFFFF));
    w16(f, (uint16_t)(bc.loopEnd   & 0xFFFF));
    for (int i = 0; i < bc.codeLen; i++) f.write(bc.code[i]);

    w8(f, (uint8_t)bc.numPoolLen);
    for (int i = 0; i < bc.numPoolLen; i++) wD(f, bc.numPool[i]);

    w8(f, (uint8_t)bc.strPoolLen);
    for (int i = 0; i < bc.strPoolLen; i++) wS(f, bc.strPool[i]);

    w8(f, (uint8_t)bc.namePoolLen);
    for (int i = 0; i < bc.namePoolLen; i++) wS(f, bc.namePool[i]);

    w8(f, (uint8_t)funcCount);
    for (int i = 0; i < funcCount; i++) {
        wS(f, funcs[i].name);
        w16(f, (uint16_t)funcs[i].bcStart);
        w8(f, (uint8_t)funcs[i].paramCount);
        for (int j = 0; j < funcs[i].paramCount; j++) wS(f, funcs[i].params[j]);
    }
    f.close();
    return true;
}

bool OSARuntime::loadOsac(const String& srcPath) {
    if (!isSdReady) { setError(0, "No SD card"); return false; }
    File f = SD.open(srcPath);
    if (!f) { setError(0, "Not found: " + srcPath); return false; }
    if ((size_t)f.size() > 96 * 1024) {
        f.close(); setError(0, ".osac exceeds 96 KB"); return false;
    }
    char magic[4];
    if (!readExact(f, magic, sizeof(magic))) {
        f.close(); setError(0, "Truncated .osac header"); return false;
    }
    if (magic[0] != 'O' || magic[1] != 'S' || magic[2] != 'A' || magic[3] != 'C') {
        f.close(); setError(0, "Bad .osac magic"); return false;
    }
    uint8_t ver;
    if (!r8(f, ver) || ver != OSAC_VERSION) {
        f.close(); setError(0, "Bad .osac version"); return false;
    }

    bc.clear();
    uint16_t ignoredColor, codeLen, setupEnd, loopStartRaw, loopEndRaw;
    uint8_t ignoredIsApp, serializedException, count;
    if (!rS(f, appName, 64)) {
        f.close(); setError(0, "Invalid .osac app name"); return false;
    }
    if (!r16(f, ignoredColor) || !r8(f, ignoredIsApp)) {
        f.close(); setError(0, "Truncated .osac header"); return false;
    }
    if (!r8(f, serializedException) || !r8(f, requiredPerms)) {
        f.close(); setError(0, "Truncated .osac header"); return false;
    }
    (void)serializedException;

    if (!r16(f, codeLen) || !r16(f, setupEnd) ||
        !r16(f, loopStartRaw) || !r16(f, loopEndRaw)) {
        f.close(); setError(0, "Truncated .osac layout"); return false;
    }
    bc.codeLen   = codeLen;
    bc.setupEnd  = setupEnd;
    bc.loopStart = (int16_t)loopStartRaw;
    bc.loopEnd   = (int16_t)loopEndRaw;
    if (bc.codeLen < 1 || bc.codeLen > OSA_BC_MAX || bc.setupEnd > bc.codeLen ||
        (bc.loopStart >= 0 && (bc.loopStart >= bc.codeLen ||
         bc.loopEnd <= bc.loopStart || bc.loopEnd > bc.codeLen))) {
        f.close(); setError(0, "Invalid .osac layout"); return false;
    }
    if (!readExact(f, bc.code, bc.codeLen)) {
        f.close(); setError(0, "Truncated .osac code"); return false;
    }

    if (!r8(f, count) || count > OSA_NUM_CONST) {
        f.close(); setError(0, "Invalid .osac number pool"); return false;
    }
    bc.numPoolLen = count;
    for (int i = 0; i < bc.numPoolLen; i++) {
        if (!rD(f, bc.numPool[i])) {
            f.close(); setError(0, "Truncated .osac number pool"); return false;
        }
    }

    if (!r8(f, count) || count > OSA_STR_CONST) {
        f.close(); setError(0, "Invalid .osac string pool"); return false;
    }
    bc.strPoolLen = count;
    size_t totalStringBytes = 0;
    for (int i = 0; i < bc.strPoolLen; i++) {
        if (!rS(f, bc.strPool[i], 4096)) {
            f.close(); setError(0, "Invalid .osac string"); return false;
        }
        totalStringBytes += bc.strPool[i].length();
        if (totalStringBytes > 32 * 1024) {
            f.close(); setError(0, ".osac string pool exceeds 32 KB"); return false;
        }
    }

    if (!r8(f, count) || count > OSA_NAME_CONST) {
        f.close(); setError(0, "Invalid .osac name pool"); return false;
    }
    bc.namePoolLen = count;
    for (int i = 0; i < bc.namePoolLen; i++) {
        if (!rS(f, bc.namePool[i], 64)) {
            f.close(); setError(0, "Invalid .osac identifier"); return false;
        }
    }

    if (!r8(f, count) || count > OSA_MAX_FUNCS) {
        f.close(); setError(0, "Invalid .osac function table"); return false;
    }
    funcCount = count;
    for (int i = 0; i < funcCount; i++) {
        uint16_t fnStart;
        uint8_t paramCount;
        if (!rS(f, funcs[i].name, 64) || !r16(f, fnStart) ||
            !r8(f, paramCount) || fnStart >= bc.codeLen || paramCount > 8) {
            f.close(); setError(0, "Invalid .osac function"); return false;
        }
        funcs[i].bcStart     = fnStart;
        funcs[i].paramCount  = paramCount;
        funcs[i].bodyStart   = funcs[i].bodyEnd = 0;
        for (int j = 0; j < funcs[i].paramCount; j++) {
            if (!rS(f, funcs[i].params[j], 64)) {
                f.close(); setError(0, "Invalid .osac parameter"); return false;
            }
        }
    }
    f.close();
    isException = readIsExceptionFromFile(srcPath);
    bc.valid = true;
    return true;
}

// ─── Compiler helpers ────────────────────────────────────────────────────────
namespace {

static bool emit1(OSABytecode& bc, uint8_t op) {
    if (bc.codeLen > OSA_BC_MAX - 1) {
        bc.buildError = OSA_BCERR_CODE_FULL;
        return false;
    }
    bc.code[bc.codeLen++] = op;
    return true;
}
static bool emit3(OSABytecode& bc, uint8_t op, int16_t operand) {
    if (bc.codeLen > OSA_BC_MAX - 3) {
        bc.buildError = OSA_BCERR_CODE_FULL;
        return false;
    }
    bc.code[bc.codeLen++] = op;
    bc.code[bc.codeLen++] = (uint8_t)(operand & 0xFF);
    bc.code[bc.codeLen++] = (uint8_t)((operand >> 8) & 0xFF);
    return true;
}
static bool emit4(OSABytecode& bc, uint8_t op, int16_t operand, uint8_t b) {
    // Check all four bytes first so a failed call never leaves a partial
    // instruction at the end of the stream.
    if (bc.codeLen > OSA_BC_MAX - 4) {
        bc.buildError = OSA_BCERR_CODE_FULL;
        return false;
    }
    bc.code[bc.codeLen++] = op;
    bc.code[bc.codeLen++] = (uint8_t)(operand & 0xFF);
    bc.code[bc.codeLen++] = (uint8_t)((operand >> 8) & 0xFF);
    bc.code[bc.codeLen++] = b;
    return true;
}
static int16_t readI16(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static bool patch16(OSABytecode& bc, int at, int16_t v) {
    if (at < 0 || at + 1 >= bc.codeLen || at + 1 >= OSA_BC_MAX) {
        bc.buildError = OSA_BCERR_BAD_PATCH;
        return false;
    }
    bc.code[at]     = (uint8_t)(v & 0xFF);
    bc.code[at + 1] = (uint8_t)((v >> 8) & 0xFF);
    return true;
}

// Tokenizer for one source line.
enum TK { TK_END, TK_NUM, TK_STR, TK_IDENT,
          TK_LP, TK_RP, TK_COMMA,
          TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
          TK_EQ, TK_EQEQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
          TK_AND, TK_OR, TK_NOT };
struct Tok { TK type; double num; String str; };

struct Lex {
    const String& s;
    int p;
    Lex(const String& src) : s(src), p(0) {}
    void skip() { while (p < (int)s.length() && (s[p] == ' ' || s[p] == '\t')) p++; }
    Tok peek() { int save = p; Tok t = next(); p = save; return t; }
    Tok next() {
        skip();
        Tok t; t.type = TK_END;
        if (p >= (int)s.length()) return t;
        char c = s[p];
        if (c >= '0' && c <= '9') {
            int q = p; bool seen = false;
            while (q < (int)s.length() && ((s[q] >= '0' && s[q] <= '9') || s[q] == '.')) {
                if (s[q] == '.') seen = true;
                q++;
            }
            t.type = TK_NUM; t.num = s.substring(p, q).toDouble();
            p = q; (void)seen; return t;
        }
        if (c == '"') {
            int q = p + 1; String acc;
            while (q < (int)s.length() && s[q] != '"') {
                if (s[q] == '\\' && q + 1 < (int)s.length()) {
                    char n = s[q + 1];
                    if      (n == 'n')  acc += '\n';
                    else if (n == 't')  acc += '\t';
                    else if (n == '"')  acc += '"';
                    else if (n == '\\') acc += '\\';
                    else acc += n;
                    q += 2;
                } else { acc += s[q]; q++; }
            }
            p = q + 1;
            t.type = TK_STR; t.str = acc; return t;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int q = p;
            while (q < (int)s.length() && ((s[q] >= 'a' && s[q] <= 'z') ||
                   (s[q] >= 'A' && s[q] <= 'Z') || (s[q] >= '0' && s[q] <= '9') ||
                   s[q] == '_' || s[q] == '.')) q++;
            t.str = s.substring(p, q); p = q;
            if      (t.str == "and") t.type = TK_AND;
            else if (t.str == "or")  t.type = TK_OR;
            else if (t.str == "not") t.type = TK_NOT;
            else                     t.type = TK_IDENT;
            return t;
        }
        p++;
        switch (c) {
            case '(': t.type = TK_LP; return t;
            case ')': t.type = TK_RP; return t;
            case ',': t.type = TK_COMMA; return t;
            case '+': t.type = TK_PLUS; return t;
            case '-': t.type = TK_MINUS; return t;
            case '*': t.type = TK_STAR; return t;
            case '/': t.type = TK_SLASH; return t;
            case '%': t.type = TK_PERCENT; return t;
            case '!': if (p < (int)s.length() && s[p] == '=') { p++; t.type = TK_NE; }
                      else                                    { t.type = TK_NOT; }
                      return t;
            case '<': if (p < (int)s.length() && s[p] == '=') { p++; t.type = TK_LE; }
                      else                                    { t.type = TK_LT; }
                      return t;
            case '>': if (p < (int)s.length() && s[p] == '=') { p++; t.type = TK_GE; }
                      else                                    { t.type = TK_GT; }
                      return t;
            case '=': if (p < (int)s.length() && s[p] == '=') { p++; t.type = TK_EQEQ; }
                      else                                    { t.type = TK_EQ; }
                      return t;
        }
        return t;
    }
};

} // anonymous namespace

// Forward refs for recursive descent.
static bool compExpr(OSARuntime* rt, Lex& lex);
static bool compOr  (OSARuntime* rt, Lex& lex);
static bool compAnd (OSARuntime* rt, Lex& lex);
static bool compCmp (OSARuntime* rt, Lex& lex);
static bool compSum (OSARuntime* rt, Lex& lex);
static bool compMul (OSARuntime* rt, Lex& lex);
static bool compUny (OSARuntime* rt, Lex& lex);
static bool compPri (OSARuntime* rt, Lex& lex);

// Friend-ish access — these emit through rt->bc and use rt->bcAdd*.
// Implementations below use OSARuntime's bc/bcAddNum/bcAddStr/bcAddName.

// Per-loop frame for tracking break/continue jumps to patch.
struct LoopFrame {
    int startPc;          // jump target for `continue`
    int breaks[16];       // PCs of OP_JMP operand bytes to patch with end-pc
    int breakCount = 0;
};
static LoopFrame g_loopStack[OSA_LOOP_DEPTH];
static int       g_loopDepth = 0;

// Recursive forward refs.
static bool compileLineRange(OSARuntime* rt, int from, int to);
static bool compileLineAt(OSARuntime* rt, int& i, int rangeEnd);

bool OSARuntime::compile() {
    bc.clear();
    g_loopDepth = 0;
    lastCompileError = OSA_BCERR_NONE;
    auto failCompile = [&](uint8_t fallback) -> bool {
        lastCompileError = bc.buildError != OSA_BCERR_NONE
                         ? bc.buildError : fallback;
        bc.clear();
        return false;
    };

    // Find function spans + main loop position the same way loadScript does.
    int topLoopStart = -1, topLoopEnd = -1;
    {
        int depth = 0;
        for (int i = 0; i < lineCount; i++) {
            const String& t = lines[i];
            if (depth == 0 && t == "loop") { topLoopStart = i; break; }
            if (isBlockOpen(t)) depth++;
            else if (t == "end" && depth > 0) depth--;
        }
    }
    if (topLoopStart >= 0) topLoopEnd = findMatchingEnd(topLoopStart);

    // Setup section — everything up to the main loop, skipping function defs.
    int setupEnd = (topLoopStart >= 0) ? topLoopStart : lineCount;
    if (!compileLineRange(this, 0, setupEnd))
        return failCompile(OSA_BCERR_UNSUPPORTED);
    if (!emit1(bc, OP_HALT)) return failCompile(OSA_BCERR_CODE_FULL);
    bc.setupEnd = bc.codeLen;

    // Main loop body.
    if (topLoopStart >= 0 && topLoopEnd > topLoopStart) {
        bc.loopStart = bc.codeLen;
        if (!emit1(bc, OP_LOOP_TICK)) return failCompile(OSA_BCERR_CODE_FULL);
        if (!compileLineRange(this, topLoopStart + 1, topLoopEnd)) {
            return failCompile(OSA_BCERR_UNSUPPORTED);
        }
        // The host calls runUpdate() repeatedly. Do one source-level loop
        // iteration per call instead of trapping Arduino's loopTask here.
        bc.loopEnd = bc.codeLen;
    }

    // Function bodies — appended after the main bytecode. Each starts at
    // funcs[i].bcStart and ends with OP_RET_VOID (in case the body never
    // returns explicitly). Callers reach them through OP_CALL_USER.
    for (int f = 0; f < funcCount; f++) {
        funcs[f].bcStart = bc.codeLen;
        if (!compileLineRange(this, funcs[f].bodyStart, funcs[f].bodyEnd)) {
            return failCompile(OSA_BCERR_UNSUPPORTED);
        }
        if (!emit1(bc, OP_RET_VOID)) return failCompile(OSA_BCERR_CODE_FULL);
    }

    if (bc.buildError != OSA_BCERR_NONE)
        return failCompile(bc.buildError);
    bc.valid = true;
    return true;
}

static bool compileLineRange(OSARuntime* rt, int from, int to) {
    int i = from;
    while (i < to) {
        if (!compileLineAt(rt, i, to)) return false;
        if (rt->bc.buildError != OSA_BCERR_NONE) return false;
    }
    return true;
}

static bool compileLineAt(OSARuntime* rt, int& i, int rangeEnd) {
    String s = rt->bcLines()[i]; s.trim();
    if (s.length() == 0 || s[0] == '#') { i++; return true; }

    // def name(...) — top-level compile() emits bodies separately at the end
    // of the bytecode. Here we just skip past the matching `end`.
    if (s.startsWith("def ")) {
        int end = rt->bcFindMatchingEnd(i);
        if (end < 0 || end > rangeEnd) return false;
        i = end + 1;
        return true;
    }

    // Inline control flow: `if X then STMT end` and
    // `while X do STMT end`. Compiling these avoids retaining every source
    // line in heap for UI-heavy apps such as OpenStore.
    if (s.startsWith("if ") && s.endsWith(" end")) {
        int thenPos = s.indexOf(" then ");
        if (thenPos <= 3) return false;
        String cond = s.substring(3, thenPos); cond.trim();
        String body = s.substring(thenPos + 6, s.length() - 4); body.trim();
        if (cond.length() == 0 || body.length() == 0) return false;

        Lex lex(cond);
        if (!compExpr(rt, lex) || !emit3(rt->bc, OP_JMP_IF_FALSE, 0))
            return false;
        int skipPatch = rt->bc.codeLen - 2;

        String saved = rt->bcLines()[i];
        rt->bcLines()[i] = body;
        int inner = i;
        bool bodyOk = compileLineAt(rt, inner, i + 1);
        rt->bcLines()[i] = saved;
        if (!bodyOk ||
            !patch16(rt->bc, skipPatch,
                     (int16_t)(rt->bc.codeLen - (skipPatch + 2))))
            return false;
        i++;
        return true;
    }

    if (s.startsWith("while ") && s.endsWith(" end")) {
        int doPos = s.indexOf(" do ");
        if (doPos <= 6) return false;
        String cond = s.substring(6, doPos); cond.trim();
        String body = s.substring(doPos + 4, s.length() - 4); body.trim();
        if (cond.length() == 0 || body.length() == 0) return false;

        int topPc = rt->bc.codeLen;
        Lex lex(cond);
        if (!compExpr(rt, lex) || !emit3(rt->bc, OP_JMP_IF_FALSE, 0))
            return false;
        int exitPatch = rt->bc.codeLen - 2;
        if (g_loopDepth >= OSA_LOOP_DEPTH) return false;
        { LoopFrame& frame = g_loopStack[g_loopDepth++];
          frame.startPc = topPc; frame.breakCount = 0; }

        String saved = rt->bcLines()[i];
        rt->bcLines()[i] = body;
        int inner = i;
        bool bodyOk = compileLineAt(rt, inner, i + 1);
        rt->bcLines()[i] = saved;
        if (!bodyOk) { g_loopDepth--; return false; }

        int backAt = rt->bc.codeLen;
        if (!emit3(rt->bc, OP_JMP, 0) ||
            !patch16(rt->bc, backAt + 1,
                     (int16_t)(topPc - (backAt + 3))) ||
            !patch16(rt->bc, exitPatch,
                     (int16_t)(rt->bc.codeLen - (exitPatch + 2)))) {
            g_loopDepth--;
            return false;
        }
        LoopFrame& frame = g_loopStack[--g_loopDepth];
        for (int k = 0; k < frame.breakCount; ++k) {
            if (!patch16(rt->bc, frame.breaks[k],
                         (int16_t)(rt->bc.codeLen - (frame.breaks[k] + 2))))
                return false;
        }
        i++;
        return true;
    }

    // ── if / elif / else / end ──────────────────────────────────────────────
    if (s.startsWith("if ")) {
        int thenPos = s.indexOf("then");
        if (thenPos < 0) return false;
        String tail = s.substring(thenPos + 4); tail.trim();
        // Inline form `if X then STMT end` — bail to tree-walker. STMT could
        // be `return`, `var x = …`, or anything else not safely modeled as a
        // pure expression statement.
        if (tail.length() > 0) return false;

        int finalEnd = rt->bcFindMatchingEnd(i);
        if (finalEnd < 0 || finalEnd > rangeEnd) return false;

        int branchPc = -1;            // current JMP_IF_FALSE operand to patch
        int forwardJumps[16];         // JMP-to-end operands to patch when done
        int forwardCount = 0;

        int j = i;
        while (j < finalEnd) {
            String h = rt->bcLines()[j]; h.trim();
            if (h.startsWith("if ") || h.startsWith("elif ")) {
                if (branchPc >= 0 &&
                    !patch16(rt->bc, branchPc,
                             (int16_t)(rt->bc.codeLen - (branchPc + 2))))
                    return false;
                int prefix = h.startsWith("if ") ? 3 : 5;
                int tp = h.indexOf("then");
                if (tp < 0) return false;
                String cond = h.substring(prefix, tp); cond.trim();
                Lex lex(cond);
                if (!compExpr(rt, lex)) return false;
                if (!emit3(rt->bc, OP_JMP_IF_FALSE, 0)) return false;
                branchPc = rt->bc.codeLen - 2;

                int next = rt->bcFindNextBranch(j + 1);
                if (next < 0 || next > finalEnd) return false;
                if (!compileLineRange(rt, j + 1, next)) return false;

                if (forwardCount >= 16) return false;
                if (!emit3(rt->bc, OP_JMP, 0)) return false;
                forwardJumps[forwardCount++] = rt->bc.codeLen - 2;
                j = next;
            } else if (h == "else") {
                if (branchPc >= 0) {
                    if (!patch16(rt->bc, branchPc,
                                 (int16_t)(rt->bc.codeLen - (branchPc + 2))))
                        return false;
                    branchPc = -1;
                }
                if (!compileLineRange(rt, j + 1, finalEnd)) return false;
                j = finalEnd;
            } else {
                return false;          // unexpected mid-block content
            }
        }
        if (branchPc >= 0 &&
            !patch16(rt->bc, branchPc,
                     (int16_t)(rt->bc.codeLen - (branchPc + 2))))
            return false;
        for (int k = 0; k < forwardCount; k++) {
            if (!patch16(rt->bc, forwardJumps[k],
                         (int16_t)(rt->bc.codeLen - (forwardJumps[k] + 2))))
                return false;
        }
        i = finalEnd + 1;
        return true;
    }

    // ── while cond do … end ────────────────────────────────────────────────
    if (s.startsWith("while ")) {
        int doPos = s.indexOf(" do");
        if (doPos < 0) return false;
        String tail = s.substring(doPos + 3); tail.trim();
        // Inline-while `while X do STMT end` → bail to tree-walker for the
        // same reason as inline-if above.
        if (tail.length() > 0) return false;
        String cond = s.substring(6, doPos); cond.trim();
        int finalEnd = rt->bcFindMatchingEnd(i);
        if (finalEnd < 0 || finalEnd > rangeEnd) return false;

        int topPc = rt->bc.codeLen;
        Lex lex(cond);
        if (!compExpr(rt, lex)) return false;
        if (!emit3(rt->bc, OP_JMP_IF_FALSE, 0)) return false;
        int exitPatch = rt->bc.codeLen - 2;

        if (g_loopDepth >= OSA_LOOP_DEPTH) return false;
        { LoopFrame& fr0 = g_loopStack[g_loopDepth++]; fr0.startPc = topPc; fr0.breakCount = 0; }

        if (!compileLineRange(rt, i + 1, finalEnd)) { g_loopDepth--; return false; }

        int backAt = rt->bc.codeLen;
        if (!emit3(rt->bc, OP_JMP, 0)) return false;
        if (!patch16(rt->bc, backAt + 1, (int16_t)(topPc - (backAt + 3))))
            return false;

        if (!patch16(rt->bc, exitPatch,
                     (int16_t)(rt->bc.codeLen - (exitPatch + 2))))
            return false;
        LoopFrame& fr = g_loopStack[--g_loopDepth];
        for (int k = 0; k < fr.breakCount; k++) {
            if (!patch16(rt->bc, fr.breaks[k],
                         (int16_t)(rt->bc.codeLen - (fr.breaks[k] + 2))))
                return false;
        }

        i = finalEnd + 1;
        return true;
    }

    // ── for IDENT = a to b do … end ────────────────────────────────────────
    if (s.startsWith("for ")) {
        int eqPos = s.indexOf('=');
        int toPos = s.indexOf(" to ");
        int doPos = s.indexOf(" do");
        if (eqPos < 0 || toPos < 0 || doPos < 0) return false;
        String tail = s.substring(doPos + 3); tail.trim();
        if (tail.length() > 0) return false;
        String var = s.substring(4, eqPos); var.trim();
        String startE = s.substring(eqPos + 1, toPos); startE.trim();
        String endE   = s.substring(toPos + 4, doPos); endE.trim();
        int finalEnd = rt->bcFindMatchingEnd(i);
        if (finalEnd < 0 || finalEnd > rangeEnd) return false;
        int nameIdx = rt->bcAddName(var);
        if (nameIdx < 0) return false;

        // i = start
        { Lex lex(startE); if (!compExpr(rt, lex)) return false; }
        if (!emit3(rt->bc, OP_DECLARE_VAR, (int16_t)nameIdx)) return false;

        // Compute end once, store in a hidden var.
        String endVar = String("__for_end_") + i;
        int endIdx = rt->bcAddName(endVar);
        if (endIdx < 0) return false;
        { Lex lex(endE); if (!compExpr(rt, lex)) return false; }
        if (!emit3(rt->bc, OP_DECLARE_VAR, (int16_t)endIdx)) return false;

        int topPc = rt->bc.codeLen;
        // while i <= __end__ do
        if (!emit3(rt->bc, OP_LOAD_VAR, (int16_t)nameIdx) ||
            !emit3(rt->bc, OP_LOAD_VAR, (int16_t)endIdx) ||
            !emit1(rt->bc, OP_LE) ||
            !emit3(rt->bc, OP_JMP_IF_FALSE, 0)) return false;
        int exitPatch = rt->bc.codeLen - 2;

        if (g_loopDepth >= OSA_LOOP_DEPTH) return false;
        { LoopFrame& fr0 = g_loopStack[g_loopDepth++]; fr0.startPc = topPc; fr0.breakCount = 0; }

        if (!compileLineRange(rt, i + 1, finalEnd)) { g_loopDepth--; return false; }

        // i = i + 1
        if (!emit3(rt->bc, OP_LOAD_VAR, (int16_t)nameIdx) ||
            !emit1(rt->bc, OP_PUSH_NUM1) ||
            !emit1(rt->bc, OP_ADD) ||
            !emit3(rt->bc, OP_STORE_VAR, (int16_t)nameIdx)) return false;

        int backAt = rt->bc.codeLen;
        if (!emit3(rt->bc, OP_JMP, 0)) return false;
        if (!patch16(rt->bc, backAt + 1, (int16_t)(topPc - (backAt + 3))))
            return false;

        if (!patch16(rt->bc, exitPatch,
                     (int16_t)(rt->bc.codeLen - (exitPatch + 2))))
            return false;
        LoopFrame& fr = g_loopStack[--g_loopDepth];
        for (int k = 0; k < fr.breakCount; k++) {
            if (!patch16(rt->bc, fr.breaks[k],
                         (int16_t)(rt->bc.codeLen - (fr.breaks[k] + 2))))
                return false;
        }

        i = finalEnd + 1;
        return true;
    }

    // ── break / continue ───────────────────────────────────────────────────
    if (s == "break") {
        if (g_loopDepth == 0) return false;
        LoopFrame& fr = g_loopStack[g_loopDepth - 1];
        if (fr.breakCount >= 16) return false;
        if (!emit3(rt->bc, OP_JMP, 0)) return false;
        fr.breaks[fr.breakCount++] = rt->bc.codeLen - 2;
        i++;
        return true;
    }
    if (s == "continue") {
        if (g_loopDepth == 0) return false;
        LoopFrame& fr = g_loopStack[g_loopDepth - 1];
        int at = rt->bc.codeLen;
        if (!emit3(rt->bc, OP_JMP, 0)) return false;
        if (!patch16(rt->bc, at + 1, (int16_t)(fr.startPc - (at + 3))))
            return false;
        i++;
        return true;
    }

    // ── return / exit ──────────────────────────────────────────────────────
    if (s == "exit()" || s == "exit") {
        if (!emit1(rt->bc, OP_EXIT)) return false;
        i++;
        return true;
    }
    if (s == "return" || s.startsWith("return ")) {
        String rhs = s.length() > 6 ? s.substring(7) : String(""); rhs.trim();
        if (rhs.length() == 0) {
            if (!emit1(rt->bc, OP_RET_VOID)) return false;
            i++;
            return true;
        }
        Lex lex(rhs);
        if (!compExpr(rt, lex)) return false;
        if (!emit1(rt->bc, OP_RET)) return false;
        i++;
        return true;
    }

    // Nested `loop` keyword — only top-level supported.
    if (s == "loop") return false;

    // ── Expression / assignment / var decl ─────────────────────────────────
    {
        Lex lex(s);
        Tok first = lex.peek();
        if (first.type == TK_IDENT && first.str == "var") {
            lex.next();
            Tok name = lex.next();
            if (name.type != TK_IDENT) return false;
            Tok eq = lex.next();
            if (eq.type != TK_EQ) return false;
            if (!compExpr(rt, lex)) return false;
            int nameIdx = rt->bcAddName(name.str);
            if (nameIdx < 0) return false;
            if (!emit3(rt->bc, OP_DECLARE_VAR, (int16_t)nameIdx)) return false;
            i++;
            return true;
        }
        if (first.type == TK_IDENT) {
            int save = lex.p;
            lex.next();
            Tok maybeEq = lex.next();
            if (maybeEq.type == TK_EQ) {
                if (!compExpr(rt, lex)) return false;
                int nameIdx = rt->bcAddName(first.str);
                if (nameIdx < 0) return false;
                if (!emit3(rt->bc, OP_STORE_VAR, (int16_t)nameIdx)) return false;
                i++;
                return true;
            }
            lex.p = save;
        }
        if (!compExpr(rt, lex)) return false;
        if (!emit1(rt->bc, OP_POP)) return false;
        i++;
        return true;
    }
}

// Recursive descent — mirrors evalOr/evalAnd/… and emits bytecode.

static bool compExpr(OSARuntime* rt, Lex& lex) { return compOr(rt, lex); }

static bool compOr(OSARuntime* rt, Lex& lex) {
    if (!compAnd(rt, lex)) return false;
    while (lex.peek().type == TK_OR) { lex.next();
        if (!compAnd(rt, lex)) return false;
        if (!emit1(rt->bc, OP_OR)) return false;
    }
    return true;
}
static bool compAnd(OSARuntime* rt, Lex& lex) {
    if (!compCmp(rt, lex)) return false;
    while (lex.peek().type == TK_AND) { lex.next();
        if (!compCmp(rt, lex)) return false;
        if (!emit1(rt->bc, OP_AND)) return false;
    }
    return true;
}
static bool compCmp(OSARuntime* rt, Lex& lex) {
    if (!compSum(rt, lex)) return false;
    Tok t = lex.peek();
    if (t.type == TK_EQEQ || t.type == TK_NE || t.type == TK_LT ||
        t.type == TK_LE   || t.type == TK_GT || t.type == TK_GE) {
        lex.next();
        if (!compSum(rt, lex)) return false;
        switch (t.type) {
            case TK_EQEQ: if (!emit1(rt->bc, OP_EQ)) return false; break;
            case TK_NE:   if (!emit1(rt->bc, OP_NE)) return false; break;
            case TK_LT:   if (!emit1(rt->bc, OP_LT)) return false; break;
            case TK_LE:   if (!emit1(rt->bc, OP_LE)) return false; break;
            case TK_GT:   if (!emit1(rt->bc, OP_GT)) return false; break;
            case TK_GE:   if (!emit1(rt->bc, OP_GE)) return false; break;
            default: break;
        }
    }
    return true;
}
static bool compSum(OSARuntime* rt, Lex& lex) {
    if (!compMul(rt, lex)) return false;
    while (true) {
        Tok t = lex.peek();
        if (t.type == TK_PLUS)       { lex.next(); if (!compMul(rt, lex) || !emit1(rt->bc, OP_ADD)) return false; }
        else if (t.type == TK_MINUS) { lex.next(); if (!compMul(rt, lex) || !emit1(rt->bc, OP_SUB)) return false; }
        else break;
    }
    return true;
}
static bool compMul(OSARuntime* rt, Lex& lex) {
    if (!compUny(rt, lex)) return false;
    while (true) {
        Tok t = lex.peek();
        if      (t.type == TK_STAR)    { lex.next(); if (!compUny(rt, lex) || !emit1(rt->bc, OP_MUL)) return false; }
        else if (t.type == TK_SLASH)   { lex.next(); if (!compUny(rt, lex) || !emit1(rt->bc, OP_DIV)) return false; }
        else if (t.type == TK_PERCENT) { lex.next(); if (!compUny(rt, lex) || !emit1(rt->bc, OP_MOD)) return false; }
        else break;
    }
    return true;
}
static bool compUny(OSARuntime* rt, Lex& lex) {
    Tok t = lex.peek();
    if (t.type == TK_MINUS) { lex.next(); return compUny(rt, lex) && emit1(rt->bc, OP_NEG); }
    if (t.type == TK_NOT)   { lex.next(); return compUny(rt, lex) && emit1(rt->bc, OP_NOT); }
    return compPri(rt, lex);
}
static bool compPri(OSARuntime* rt, Lex& lex) {
    Tok t = lex.next();
    if (t.type == TK_NUM) {
        if      (t.num == 0.0) { if (!emit1(rt->bc, OP_PUSH_NUM0)) return false; }
        else if (t.num == 1.0) { if (!emit1(rt->bc, OP_PUSH_NUM1)) return false; }
        else {
            int idx = rt->bcAddNum(t.num);
            if (idx < 0) return false;
            if (!emit3(rt->bc, OP_PUSH_NUM, (int16_t)idx)) return false;
        }
        return true;
    }
    if (t.type == TK_STR) {
        int idx = rt->bcAddStr(t.str);
        if (idx < 0) return false;
        if (!emit3(rt->bc, OP_PUSH_STR, (int16_t)idx)) return false;
        return true;
    }
    if (t.type == TK_LP) {
        if (!compExpr(rt, lex)) return false;
        if (lex.next().type != TK_RP) return false;
        return true;
    }
    if (t.type == TK_IDENT) {
        // Function call?
        if (lex.peek().type == TK_LP) {
            lex.next();   // eat '('
            int argc = 0;
            if (lex.peek().type != TK_RP) {
                while (true) {
                    if (!compExpr(rt, lex)) return false;
                    argc++;
                    if (argc > 12) return false;
                    Tok next = lex.peek();
                    if (next.type == TK_COMMA) { lex.next(); continue; }
                    break;
                }
            }
            if (lex.next().type != TK_RP) return false;
            // Prefer a user-defined function over a builtin of the same name.
            int userIdx = -1;
            for (int k = 0; k < rt->bcFuncCount(); k++)
                if (rt->bcFuncName(k) == t.str) { userIdx = k; break; }
            if (userIdx >= 0) {
                if (argc > 8) return false;
                if (!emit4(rt->bc, OP_CALL_USER, (int16_t)userIdx, (uint8_t)argc)) return false;
            } else {
                int idx = rt->bcAddName(t.str);
                if (idx < 0) return false;
                if (!emit4(rt->bc, OP_CALL_BUILTIN, (int16_t)idx, (uint8_t)argc)) return false;
            }
            return true;
        }
        // Plain variable read.
        int idx = rt->bcAddName(t.str);
        if (idx < 0) return false;
        return emit3(rt->bc, OP_LOAD_VAR, (int16_t)idx);
    }
    return false;
}

// ─── VM exec ────────────────────────────────────────────────────────────────
bool OSARuntime::exec(int pcStart, int pcEnd) {
    if (!bc.valid || pcStart < 0 || pcEnd < pcStart || pcEnd > bc.codeLen) {
        setError(-1, "VM: invalid execution range");
        return false;
    }

    int pc = pcStart;
    vmSp = 0;
    vmCallDepth = 0;
    vmHalted = false;
    uint32_t computeStarted = millis();
    uint16_t instructions = 0;
    uint32_t totalInstructions = 0;

    auto failVm = [&](const String& message) -> bool {
        vmHalted = true;
        setError(-1, message);
        return false;
    };
    auto pushStringCopy = [&](const String& source) -> bool {
        if (vmSp >= OSA_STACK_SIZE) return failVm("VM stack overflow");
        OSAVal copy;
        copy.isNum = false;
        if (source.length() > 0 &&
            (!copy.str.reserve(source.length()) ||
             !copy.str.concat(source.c_str(), source.length())))
            return failVm("VM: not enough memory for string copy");
        return vmPush(static_cast<OSAVal&&>(copy));
    };
    auto pushCopy = [&](const OSAVal& source) -> bool {
        return source.isNum ? vmPush(OSAVal(source.num))
                            : pushStringCopy(source.str);
    };
    auto readOperand16 = [&](uint16_t& value) -> bool {
        if (pc < 0 || pc > bc.codeLen - 2) return failVm("VM: truncated operand");
        value = (uint16_t)readI16(bc.code + pc);
        pc += 2;
        return true;
    };
    auto readArgCount = [&](uint8_t& value) -> bool {
        if (pc < 0 || pc >= bc.codeLen) return failVm("VM: truncated call");
        value = bc.code[pc++];
        return true;
    };

    while (!exitFlag && errLine < 0 && !vmHalted) {
        const int currentEnd = vmCallDepth > 0 ? bc.codeLen : pcEnd;
        if (pc == currentEnd && vmCallDepth == 0) break;
        if (pc < 0 || pc >= currentEnd || pc >= bc.codeLen)
            return failVm(vmCallDepth > 0 ? "VM: function ran past bytecode"
                                          : "VM: instruction outside section");

        // Keep pure bytecode loops cooperative. Calls that intentionally block
        // (widgets, delay, HTTP) reset this window after they return, so only
        // uninterrupted script computation is limited.
        // The wall-clock budget catches tight pure-bytecode loops. The hard
        // instruction budget also catches loops that repeatedly call tiny SDK
        // builtins (those calls reset the wall-clock compute window).
        if (++totalInstructions > 100000)
            return failVm("VM instruction limit exceeded");
        if (++instructions >= 512) {
            instructions = 0;
            yield();
            if (checkExitGesture() || checkOverlayGesture()) {
                exitFlag = true;
                return false;
            }
            if ((uint32_t)(millis() - computeStarted) > 75)
                return failVm("VM execution budget exceeded");
        }

        uint8_t op = bc.code[pc++];
        int stackNeeded = 0;
        switch (op) {
            case OP_POP: case OP_DUP: case OP_STORE_VAR: case OP_DECLARE_VAR:
            case OP_NEG: case OP_NOT: case OP_JMP_IF_FALSE:
            case OP_JMP_IF_TRUE: case OP_RET:
                stackNeeded = 1;
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT:
            case OP_GE: case OP_AND: case OP_OR:
                stackNeeded = 2;
                break;
            default:
                break;
        }
        if (vmSp < stackNeeded) return failVm("VM stack underflow");
        switch (op) {
            case OP_NOP: break;
            case OP_PUSH_NUM: {
                uint16_t idx;
                if (!readOperand16(idx)) return false;
                if (idx >= (uint16_t)bc.numPoolLen) return failVm("VM: bad number index");
                if (!vmPush(OSAVal(bc.numPool[idx]))) return false;
                break;
            }
            case OP_PUSH_STR: {
                uint16_t idx;
                if (!readOperand16(idx)) return false;
                if (idx >= (uint16_t)bc.strPoolLen) return failVm("VM: bad string index");
                if (!pushStringCopy(bc.strPool[idx])) return false;
                break;
            }
            case OP_PUSH_NUM0: if (!vmPush(OSAVal(0.0))) return false; break;
            case OP_PUSH_NUM1: if (!vmPush(OSAVal(1.0))) return false; break;
            case OP_POP:       (void)vmPop(); break;
            case OP_DUP:
                if (vmSp <= 0) return failVm("VM stack underflow");
                if (!pushCopy(vmStack[vmSp - 1])) return false;
                break;
            case OP_LOAD_VAR: {
                uint16_t idx;
                if (!readOperand16(idx)) return false;
                if (idx >= (uint16_t)bc.namePoolLen) return failVm("VM: bad name index");
                const OSAVal* value = nullptr;
                for (int i = varCount - 1; i >= 0; --i) {
                    if (vars[i].name == bc.namePool[idx]) {
                        value = &vars[i].val;
                        break;
                    }
                }
                if (value) {
                    if (!pushCopy(*value)) return false;
                } else if (!vmPush(OSAVal())) return false;
                break;
            }
            case OP_STORE_VAR: {
                uint16_t idx;
                if (!readOperand16(idx)) return false;
                if (idx >= (uint16_t)bc.namePoolLen) return failVm("VM: bad name index");
                setVar(bc.namePool[idx], vmPop());
                break;
            }
            case OP_DECLARE_VAR: {
                uint16_t idx;
                if (!readOperand16(idx)) return false;
                if (idx >= (uint16_t)bc.namePoolLen) return failVm("VM: bad name index");
                declareVar(bc.namePool[idx], vmPop());
                break;
            }
            case OP_ADD: { OSAVal b = vmPop(), a = vmPop();
                if (a.isNum && b.isNum) {
                    if (!vmPush(OSAVal(a.num + b.num))) return false;
                } else {
                    String left = a.isNum ? a.toString() : static_cast<String&&>(a.str);
                    String right = b.isNum ? b.toString() : static_cast<String&&>(b.str);
                    size_t total = left.length() + right.length();
                    if (total < left.length() || total > OSA_MAX_FILE_READ)
                        return failVm("VM: string exceeds 32768 byte limit");
                    if ((total > 0 && !left.reserve(total)) ||
                        (right.length() > 0 &&
                         !left.concat(right.c_str(), right.length())))
                        return failVm("VM: not enough memory for string concat");
                    if (!vmPush(OSAVal(static_cast<String&&>(left)))) return false;
                }
                break;
            }
            case OP_SUB: { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() - b.toNum()))) return false; break; }
            case OP_MUL: { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() * b.toNum()))) return false; break; }
            case OP_DIV: { OSAVal b = vmPop(), a = vmPop();
                double db = b.toNum();
                if (!vmPush(OSAVal(db == 0.0 ? 0.0 : a.toNum() / db))) return false;
                break;
            }
            case OP_MOD: { OSAVal b = vmPop(), a = vmPop();
                double db = b.toNum();
                if (!vmPush(OSAVal(db == 0.0 ? 0.0 : fmod(a.toNum(), db)))) return false;
                break;
            }
            case OP_NEG: { OSAVal a = vmPop(); if (!vmPush(OSAVal(-a.toNum()))) return false; break; }
            case OP_EQ:  { OSAVal b = vmPop(), a = vmPop();
                bool eq = (a.isNum && b.isNum) ? (a.num == b.num)
                                                : (a.toString() == b.toString());
                if (!vmPush(OSAVal(eq ? 1.0 : 0.0))) return false;
                break;
            }
            case OP_NE:  { OSAVal b = vmPop(), a = vmPop();
                bool eq = (a.isNum && b.isNum) ? (a.num == b.num)
                                                : (a.toString() == b.toString());
                if (!vmPush(OSAVal(eq ? 0.0 : 1.0))) return false;
                break;
            }
            case OP_LT:  { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() <  b.toNum() ? 1.0 : 0.0))) return false; break; }
            case OP_LE:  { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() <= b.toNum() ? 1.0 : 0.0))) return false; break; }
            case OP_GT:  { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() >  b.toNum() ? 1.0 : 0.0))) return false; break; }
            case OP_GE:  { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.toNum() >= b.toNum() ? 1.0 : 0.0))) return false; break; }
            case OP_AND: { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.truthy() && b.truthy() ? 1.0 : 0.0))) return false; break; }
            case OP_OR:  { OSAVal b = vmPop(), a = vmPop(); if (!vmPush(OSAVal(a.truthy() || b.truthy() ? 1.0 : 0.0))) return false; break; }
            case OP_NOT: { OSAVal a = vmPop(); if (!vmPush(OSAVal(a.truthy() ? 0.0 : 1.0))) return false; break; }
            case OP_JMP: {
                uint16_t raw;
                if (!readOperand16(raw)) return false;
                int target = pc + (int16_t)raw;
                int targetEnd = vmCallDepth > 0 ? bc.codeLen : pcEnd;
                if (target < 0 || target > targetEnd) return failVm("VM: bad jump target");
                pc = target;
                break;
            }
            case OP_JMP_IF_FALSE: {
                uint16_t raw;
                if (!readOperand16(raw)) return false;
                OSAVal v = vmPop();
                if (!v.truthy()) {
                    int target = pc + (int16_t)raw;
                    int targetEnd = vmCallDepth > 0 ? bc.codeLen : pcEnd;
                    if (target < 0 || target > targetEnd) return failVm("VM: bad jump target");
                    pc = target;
                }
                break;
            }
            case OP_JMP_IF_TRUE: {
                uint16_t raw;
                if (!readOperand16(raw)) return false;
                OSAVal v = vmPop();
                if (v.truthy()) {
                    int target = pc + (int16_t)raw;
                    int targetEnd = vmCallDepth > 0 ? bc.codeLen : pcEnd;
                    if (target < 0 || target > targetEnd) return failVm("VM: bad jump target");
                    pc = target;
                }
                break;
            }
            case OP_LOOP_TICK:
                yield();
                if (checkExitGesture() || checkOverlayGesture()) {
                    exitFlag = true;
                    return false;
                }
                break;
            case OP_CALL_BUILTIN: {
                uint16_t nameIdx;
                uint8_t argc;
                if (!readOperand16(nameIdx) || !readArgCount(argc)) return false;
                if (nameIdx >= (uint16_t)bc.namePoolLen || argc > 12)
                    return failVm("VM: invalid builtin call");
                if (vmSp < argc) return failVm("VM stack underflow");
                if (vmSp - argc >= OSA_STACK_SIZE) return failVm("VM stack overflow");
                OSAVal args[12];
                for (int i = argc - 1; i >= 0; i--) args[i] = vmPop();
                // Pass already-evaluated values directly. Re-serializing a
                // 24 KB JSON response into source text used to duplicate it,
                // escape it and parse it again, causing avoidable RAM spikes.
                directBuiltinArgs = args;
                directBuiltinArgc = argc;
                OSAVal r = callBuiltin(bc.namePool[nameIdx], "");
                directBuiltinArgs = nullptr;
                directBuiltinArgc = 0;
                // Do not count time spent inside a blocking native SDK call as
                // runaway bytecode time.
                computeStarted = millis();
                if (errLine >= 0 || exitFlag || vmHalted) return false;
                if (!vmPush(static_cast<OSAVal&&>(r))) return false;
                break;
            }
            case OP_CALL_USER: {
                uint16_t funcIdx;
                uint8_t argc;
                if (!readOperand16(funcIdx) || !readArgCount(argc)) return false;
                if (funcIdx >= (uint16_t)funcCount || argc > 8 ||
                    vmCallDepth >= OSA_STACK_MAX)
                    return failVm("VM: invalid user call");
                if (vmSp < argc) return failVm("VM stack underflow");
                if (vmSp - argc >= OSA_STACK_SIZE) return failVm("VM stack overflow");
                Func& fn = funcs[funcIdx];
                if (fn.bcStart < 0 || fn.bcStart >= bc.codeLen)
                    return failVm("VM: bad function entry");
                OSAVal args[8];
                int n = argc;
                for (int i = n - 1; i >= 0; i--) args[i] = vmPop();
                // Save resume point + scope size so locals don't leak.
                vmCallStack[vmCallDepth].returnPc       = pc;
                vmCallStack[vmCallDepth].savedVarCount  = varCount;
                vmCallDepth++;
                // Bind params as locals in the callee's scope.
                for (int i = 0; i < n && i < fn.paramCount; i++)
                    declareVar(fn.params[i], static_cast<OSAVal&&>(args[i]));
                pc = fn.bcStart;
                break;
            }
            case OP_RET: {
                OSAVal retval = vmPop();
                if (vmHalted) return false;
                if (vmCallDepth == 0) return failVm("VM: RET at top");
                VMFrame& fr = vmCallStack[--vmCallDepth];
                varCount = fr.savedVarCount;
                pc = fr.returnPc;
                if (!vmPush(static_cast<OSAVal&&>(retval))) return false;
                break;
            }
            case OP_RET_VOID: {
                if (vmCallDepth == 0) return failVm("VM: RET at top");
                VMFrame& fr = vmCallStack[--vmCallDepth];
                varCount = fr.savedVarCount;
                pc = fr.returnPc;
                if (!vmPush(OSAVal())) return false;
                break;
            }
            case OP_HALT:
                if (vmCallDepth != 0) return failVm("VM: HALT inside function");
                return true;
            case OP_EXIT: exitFlag = true; return true;
            default:
                return failVm(String("VM: invalid opcode ") + (int)op);
        }
        if (vmHalted || errLine >= 0) return false;
    }
    return !vmHalted && errLine < 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Constructor / reset
// ═══════════════════════════════════════════════════════════════════════════════

OSARuntime::OSARuntime(TFT_eSPI* t, XPT2046_Touchscreen* ts_)
    : tft(t), ts(ts_) {}

OSARuntime::~OSARuntime() {
    reset();
}

void OSARuntime::setError(int line, const String& msg) {
    if (errLine >= 0) return; // keep first error
    // Internal VM errors use -1 because they do not map to a source line.
    // Store them as line zero so hasError()/runUpdate() cannot mistake the
    // error sentinel for "no error".
    errLine = line < 0 ? 0 : line;
    errMsg  = msg;
    exitFlag = true;
}

void OSARuntime::reset() {
    // Walk every slot that *might* hold heap-allocated String content from the
    // previous script and release it. Counters get zeroed too so the next
    // loadScript starts clean.
    for (int i = 0; i < OSA_MAX_LINES; i++) releaseStringStorage(lines[i]);
    for (int i = 0; i < OSA_MAX_VARS; i++) {
        releaseStringStorage(vars[i].name);
        releaseValueStorage(vars[i].val);
    }
    for (int i = 0; i < OSA_MAX_FUNCS; i++) {
        releaseStringStorage(funcs[i].name);
        for (int j = 0; j < 8; j++) releaseStringStorage(funcs[i].params[j]);
    }
    for (int i = 0; i < OSA_STACK_SIZE; i++) releaseValueStorage(vmStack[i]);
    lineCount = varCount = funcCount = stackDepth = 0;
    loopStart = loopEnd = -1;
    exitFlag = returnFlag = breakFlag = continueFlag = false;
    releaseValueStorage(returnValue);
    releaseStringStorage(appName);
    releaseStringStorage(loadedScriptPath);
    releaseStringStorage(packageRoot);
    releaseStringStorage(errMsg);
    errLine = -1;
    releaseStringStorage(pendingLaunch);
    clearRichMenuCache();
    clearAppsScanCache();
    if (activeSprite) {
        activeSprite->deleteSprite();
        delete activeSprite;
        activeSprite = nullptr;
    }
    if (stashSprite) {
        stashSprite->deleteSprite();
        delete stashSprite;
        stashSprite = nullptr;
    }
    bc.clear();
    vmSp = 0;
    vmCallDepth = 0;
    vmHalted = false;
    directBuiltinArgs = nullptr;
    directBuiltinArgc = 0;
    lastCompileError = OSA_BCERR_NONE;
    scriptSliceStarted = 0;
    scriptOpsSinceYield = 0;
    scriptLoopOpsTotal = 0;
    requiredPerms = 0;
    isException = false;
    wantsOverlay = false;
    swipeHomeStartY = -1;
    swipeOverlayStartY = -1;
    touchSampleMs = UINT32_MAX;
    touchSampleDown = false;
    touchSampleX = touchSampleY = -1;
    releaseStringStorage(s_httpBearer);
    releaseStringStorage(s_httpError);
    releaseStringStorage(s_ioError);
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

void OSARuntime::sampleTouch() {
    uint32_t now = millis();
    // touch.down(), touch.x() and touch.y() are normally called together.
    // One XPT2046 transaction is enough for the whole group; repeated SPI
    // reads caused uneven frame times while scrolling.
    if (touchSampleMs != UINT32_MAX && now - touchSampleMs < 6) return;
    touchSampleMs = now;
    touchSampleDown = ts->touched();
    if (!touchSampleDown) {
        touchSampleX = touchSampleY = -1;
        return;
    }
    TS_Point p = ts->getPoint();
    touchSampleX = constrain((int)map(p.x, 300, 3800, 0, 240), 0, 239);
    touchSampleY = constrain((int)map(p.y, 300, 3800, 0, 320), 0, 319);
}

bool OSARuntime::checkExitGesture() {
    sampleTouch();
    if (!touchSampleDown) { swipeHomeStartY = -1; return false; }
    int ty = touchSampleY;
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

// Updates gesture state for touch.startX/Y/dx/dy/duration/released and
// gesture.swipe*. Called lazily from each consumer so callers never miss a
// frame, and the swipe one-shot is reset only on first read.
void OSARuntime::pollGesture() {
    sampleTouch();
    bool now = touchSampleDown;
    if (now) {
        int cx = touchSampleX;
        int cy = touchSampleY;
        if (!touchWasDown) {
            gestureStartX = cx;
            gestureStartY = cy;
            gestureStartT = millis();
            gestureActive = true;
            swipeOneShot  = 0;
        }
        gestureLastX = cx;
        gestureLastY = cy;
        releasedOneShot = false;
    } else {
        if (touchWasDown && gestureActive) {
            int dx = gestureLastX - gestureStartX;
            int dy = gestureLastY - gestureStartY;
            int absX = abs(dx), absY = abs(dy);
            const int swipeThresh = 40;
            if (absX > swipeThresh || absY > swipeThresh) {
                if (absY > absX) swipeOneShot = (dy < 0) ? 1 : 2;
                else             swipeOneShot = (dx < 0) ? 3 : 4;
            }
            releasedOneShot = true;
            gestureActive   = false;
        }
    }
    touchWasDown = now;
}

bool OSARuntime::checkOverlayGesture() {
    // Swipe-down from the top edge → request Control Center. Same shape as
    // checkExitGesture but inverted. main.cpp inspects `wantsOverlay` after
    // the script unwinds and opens the overlay runtime instead of going home.
    sampleTouch();
    if (!touchSampleDown) { swipeOverlayStartY = -1; return false; }
    int ty = touchSampleY;
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

bool OSARuntime::loadScript(String path) {
    Serial.printf("[RT] loadScript path='%s' free=%u\n", path.c_str(),
                  (unsigned)ESP.getFreeHeap());
    reset();
    lineCount = varCount = funcCount = stackDepth = 0;
    loopStart = loopEnd = -1;
    exitFlag = returnFlag = false;
    breakFlag = continueFlag = false;
    returnValue = OSAVal();
    errLine = -1; errMsg = "";
    execDepth = 0;
    appName = "";
    loadedScriptPath = path;
    packageRoot = "";
    requiredPerms = 0;
    isException = false;
    pendingLaunch = "";
    swipeHomeStartY = -1;
    swipeOverlayStartY = -1;
    wantsOverlay = false;
    vmHalted = false;
    vmCallDepth = 0;
    directBuiltinArgs = nullptr;
    directBuiltinArgc = 0;
    lastCompileError = OSA_BCERR_NONE;
    // Tear down any sprite the previous script left allocated so we don't
    // leak the 10s of KB of pixel buffer between launches.
    if (activeSprite) {
        activeSprite->deleteSprite();
        delete activeSprite;
        activeSprite = nullptr;
    }
    drawColor = txtColor = TFT_WHITE;
    textFont = 2;
    // Per-script HTTP state — never leak bearer tokens or status between apps.
    s_httpBearer = "";
    s_httpStatus = 0;
    s_httpError = "";
    s_ioError = "";

    // Package assets are readable only relative to this exact installed OPK.
    const String userPrefix = "/packages/";
    const String systemPrefix = "/system/packages/";
    int idStart = -1;
    if (path.startsWith(userPrefix)) idStart = userPrefix.length();
    else if (path.startsWith(systemPrefix)) idStart = systemPrefix.length();
    if (idStart >= 0) {
        int slash = path.indexOf('/', idStart);
        if (slash > idStart) packageRoot = path.substring(0, slash);
    }

    if (!isSdReady) { setError(0, "No SD card"); return false; }

    // .osac path: load the binary directly, skip the parser entirely. Source
    // is not on the device — the only readable form is the opcode pool, which
    // is enough to run but not to recover the .osa text.
    String lower = path; lower.toLowerCase();
    if (lower.endsWith(".osac")) {
        if (!loadOsac(path)) return false;
        // sandbox path uses appName so any sandboxed fread/fwrite/kv still work.
        if (isSdReady) {
            SD.mkdir("/apps");
            SD.mkdir(sandboxDir().c_str());
        }
        return true;
    }

    File f = SD.open(path);
    if (!f) { setError(0, "Not found: " + path); return false; }

    if ((size_t)f.size() > OSA_MAX_SOURCE_BYTES) {
        f.close();
        setError(0, "OSA source exceeds 128 KB");
        return false;
    }

    // Validate the physical line layout before allocating one String per
    // source line. Besides producing an exact error, this detects files that
    // were copied through an editor/tool which removed every newline.
    size_t longestBytes = 0;
    size_t currentBytes = 0;
    size_t newlineCount = 0;
    int physicalLine = 1;
    int longestLine = 1;
    while (f.available()) {
        int value = f.read();
        if (value < 0) break;
        if (value == '\n') {
            if (currentBytes > longestBytes) {
                longestBytes = currentBytes;
                longestLine = physicalLine;
            }
            currentBytes = 0;
            newlineCount++;
            physicalLine++;
        } else if (value != '\r') {
            currentBytes++;
        }
    }
    if (currentBytes > longestBytes) {
        longestBytes = currentBytes;
        longestLine = physicalLine;
    }
    Serial.printf("[RT] source bytes=%u newlines=%u longest=L%d/%u\n",
                  (unsigned)f.size(), (unsigned)newlineCount, longestLine,
                  (unsigned)longestBytes);
    if (longestBytes > OSA_MAX_LINE_BYTES) {
        f.close();
        String reason = String("Line ") + longestLine + " has " +
                        (unsigned)longestBytes + " bytes (limit " +
                        OSA_MAX_LINE_BYTES + ")";
        if (newlineCount == 0)
            reason += ". File has no line breaks";
        setError(longestLine - 1, reason);
        return false;
    }
    if (!f.seek(0)) {
        f.close();
        setError(0, "Could not rewind OSA source");
        return false;
    }
    while (f.available()) {
        if (lineCount >= OSA_MAX_LINES) {
            f.close();
            setError(0, "OSA source exceeds 512 lines");
            return false;
        }
        String raw;
        if (!readLineLimited(f, raw, OSA_MAX_LINE_BYTES)) {
            f.close();
            setError(lineCount, String("Not enough RAM reading OSA line ") +
                                (lineCount + 1));
            return false;
        }
        raw.trim();
        lines[lineCount++] = raw;
    }
    f.close();

    // Headers — shared helpers keep Settings + runtime in sync.
    appName       = readAppNameFromFile(path);
    requiredPerms = readRequiredPermsFromFile(path);
    isException   = readIsExceptionFromFile(path);

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

    // Try to compile the script into bytecode. On success, runShow/runUpdate
    // use the VM. On failure (any unsupported language feature, currently
    // including `def` functions) the tree-walking interpreter is the fallback.
    bool compiled = compile();
    if (compiled) {
        Serial.printf("[RT] bytecode=%d nums=%d strings=%d names=%d lines=%d free=%u\n",
                      bc.codeLen, bc.numPoolLen, bc.strPoolLen, bc.namePoolLen,
                      lineCount, (unsigned)ESP.getFreeHeap());
    } else {
        Serial.printf("[RT] compile fallback/error=%u lines=%d free=%u\n",
                      (unsigned)lastCompileError, lineCount,
                      (unsigned)ESP.getFreeHeap());
    }
    if (!compiled && lastCompileError != OSA_BCERR_NONE &&
        lastCompileError != OSA_BCERR_UNSUPPORTED) {
        const char* reason = "Bytecode compilation failed";
        if (lastCompileError == OSA_BCERR_CODE_FULL)          reason = "Bytecode limit exceeded";
        else if (lastCompileError == OSA_BCERR_NUM_POOL_FULL) reason = "Too many numeric constants";
        else if (lastCompileError == OSA_BCERR_STR_POOL_FULL) reason = "Too many string constants";
        else if (lastCompileError == OSA_BCERR_NAME_POOL_FULL)reason = "Too many identifiers";
        else if (lastCompileError == OSA_BCERR_BAD_PATCH)     reason = "Invalid bytecode jump";
        setError(0, reason);
        return false;
    }

    // A successfully compiled app no longer needs its source text at runtime.
    // Releasing the per-line buffers here is especially important before TLS
    // work in OpenStore on ESP32 boards without PSRAM.
    if (compiled) {
        for (int i = 0; i < OSA_MAX_LINES; i++) releaseStringStorage(lines[i]);
        lineCount = 0;
        loopStart = loopEnd = -1;
        Serial.printf("[RT] released source lines free=%u\n",
                      (unsigned)ESP.getFreeHeap());
    }

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
    scriptSliceStarted = millis();
    scriptOpsSinceYield = 0;
    scriptLoopOpsTotal = 0;
    if (bc.valid) {
        exec(0, bc.setupEnd);
        return;
    }
    int to = (loopStart >= 0) ? loopStart : lineCount;
    execRange(0, to);
}

bool OSARuntime::runUpdate() {
    if (exitFlag || errLine >= 0) return false;
    returnFlag = breakFlag = continueFlag = false;
    execDepth = 0;
    scriptSliceStarted = millis();
    scriptOpsSinceYield = 0;
    scriptLoopOpsTotal = 0;
    if (bc.valid && bc.loopStart >= 0) {
        exec(bc.loopStart, bc.loopEnd);
        return !(exitFlag || errLine >= 0);
    }
    if (loopStart < 0) return false;
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

bool OSARuntime::cooperativeTick(uint32_t interval) {
    if (++scriptLoopOpsTotal > 100000) {
        Serial.printf("[RT] tree loop limit total=%u\n",
                      (unsigned)scriptLoopOpsTotal);
        setError(-1, "Script instruction limit exceeded");
        return false;
    }
    if (++scriptOpsSinceYield < interval) return true;
    scriptOpsSinceYield = 0;
    yield();
    if (checkExitGesture() || checkOverlayGesture()) {
        exitFlag = true;
        return false;
    }
    if ((uint32_t)(millis() - scriptSliceStarted) > 75) {
        Serial.printf("[RT] tree budget elapsed=%u total=%u\n",
                      (unsigned)(millis() - scriptSliceStarted),
                      (unsigned)scriptLoopOpsTotal);
        setError(-1, "Script execution budget exceeded");
        return false;
    }
    return true;
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
                if (!cooperativeTick()) break;
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
        if (!cooperativeTick()) break;
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
        if (!cooperativeTick()) break;
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

void OSARuntime::setVar(const String& name, OSAVal val) {
    for (int i = varCount - 1; i >= 0; i--) {
        if (vars[i].name == name) {
            vars[i].val = static_cast<OSAVal&&>(val);
            return;
        }
    }
    if (varCount < OSA_MAX_VARS) {
        vars[varCount].name = name;
        vars[varCount].val  = static_cast<OSAVal&&>(val);
        varCount++;
    }
}

void OSARuntime::declareVar(const String& name, OSAVal val) {
    // Limit the lookup to the current scope so `var pick = ...` inside a
    // user function doesn't reach into the caller and clobber its `pick`.
    // (Found this when SysDemo's wifiScreen() was overwriting the main loop's
    // `pick` variable and re-entering wallpapersScreen on Back.)
    int scopeStart = 0;
    if (vmCallDepth > 0) scopeStart = vmCallStack[vmCallDepth - 1].savedVarCount;
    else if (stackDepth > 0) scopeStart = callStack[stackDepth - 1].retVarCount;
    for (int i = varCount - 1; i >= scopeStart; i--) {
        if (vars[i].name == name) {
            vars[i].val = static_cast<OSAVal&&>(val);
            return;
        }
    }
    if (varCount < OSA_MAX_VARS) {
        vars[varCount].name = name;
        vars[varCount].val  = static_cast<OSAVal&&>(val);
        varCount++;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// User function call
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::callUser(const String& name, const String& argsStr) {
    int fi = -1;
    for (int i = 0; i < funcCount; i++)
        if (funcs[i].name == name) { fi = i; break; }
    if (fi < 0) { setError(-1, "Undefined: " + name); return OSAVal(); }

    // Parse + evaluate arguments
    String argTokens[12]; int argc = 0;
    splitArgs(argsStr, argTokens, argc);

    if (stackDepth >= OSA_STACK_MAX) { setError(-1, "Stack overflow"); return OSAVal(); }

    // Save scope
    int savedVarCount = varCount;
    callStack[stackDepth++] = { savedVarCount };

    // Bind parameters
    for (int i = 0; i < funcs[fi].paramCount && i < argc; i++)
        declareVar(funcs[fi].params[i], eval(argTokens[i]));

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
            if (count < 12) {
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
    String identity = appName;
    bool packaged = packageRoot.length() > 0;
    if (packaged) {
        int slash = packageRoot.lastIndexOf('/');
        identity = "pkg_" + packageRoot.substring(slash + 1);
    }
    for (int i = 0; i < (int)identity.length() && dir.length() < 60; i++) {
        char c = identity[i];
        bool safePackageChar = packaged && (c == '.' || c == '-' || c == '_');
        dir += (isalnum((unsigned char)c) || safePackageChar) ? (char)tolower(c) : '_';
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
    safe.replace("\\", "_");
    safe.replace(":", "_");
    return sandboxDir() + "/" + safe;
}

String OSARuntime::packageAssetPath(const String& rel) const {
    if (packageRoot.length() == 0 || rel.length() == 0 || rel.length() > 160 ||
        rel.startsWith("/") || rel.indexOf('\\') >= 0 || rel.indexOf(':') >= 0)
        return "";
    int start = 0;
    while (start <= (int)rel.length()) {
        int slash = rel.indexOf('/', start);
        if (slash < 0) slash = rel.length();
        String part = rel.substring(start, slash);
        if (part.length() == 0 || part == "." || part == "..") return "";
        start = slash + 1;
        if (slash == (int)rel.length()) break;
    }
    return packageRoot + "/" + rel;
}

// ─────────────────────────────────────────────────────────────────────────────

String OSARuntime::permKey() const {
    return permKeyForPath(loadedScriptPath);
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

String OSARuntime::permKeyForPath(const String& scriptPath) {
    // Permission identity must not depend on the user-visible #app name. Two
    // unrelated store packages are free to use the same display name, but may
    // never inherit one another's grants. OPKs use the stable package root;
    // loose .osac binaries normalize to their source path.
    String normalized = scriptPath;
    normalized.toLowerCase();
    String packagePrefix = normalized.startsWith("/system/packages/")
                         ? "/system/packages/"
                         : (normalized.startsWith("/packages/") ? "/packages/" : "");
    if (packagePrefix.length() > 0) {
        int afterId = normalized.indexOf('/', packagePrefix.length());
        if (afterId > 0) normalized = normalized.substring(0, afterId);
    } else if (normalized.endsWith(".osac")) {
        normalized.remove(normalized.length() - 1);
    }

    uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit
    for (size_t i = 0; i < normalized.length(); ++i) {
        hash ^= (uint8_t)normalized[i];
        hash *= 1099511628211ULL;
    }
    char key[22];
    snprintf(key, sizeof(key), "perm_%08lx%08lx",
             (unsigned long)(hash >> 32), (unsigned long)(hash & 0xFFFFFFFFULL));
    return String(key);
}

// Reads the .osac header into out params. Returns true on success. Fields
// not present in the file (e.g. file truncated) are left at the input value.
static bool readOsacHeader(const String& path, String& appName,
                            uint16_t& appColor, bool& isApp,
                            bool& isException, uint8_t& perms) {
    if (!isSdReady) return false;
    File f = SD.open(path);
    if (!f) return false;
    char magic[4];
    if (!readExact(f, magic, sizeof(magic))) { f.close(); return false; }
    if (magic[0] != 'O' || magic[1] != 'S' || magic[2] != 'A' || magic[3] != 'C') {
        f.close(); return false;
    }
    uint8_t version, appFlag, serializedException;
    if (!r8(f, version) || version != OSAC_VERSION ||
        !rS(f, appName, 64) || !r16(f, appColor) ||
        !r8(f, appFlag) || !r8(f, serializedException) || !r8(f, perms)) {
        f.close(); return false;
    }
    isApp = appFlag != 0;
    (void)serializedException;
    isException = OSARuntime::readIsExceptionFromFile(path);
    f.close();
    return true;
}

String OSARuntime::readAppNameFromFile(const String& path) {
    String fallback;
    int slash = path.lastIndexOf('/');
    fallback = (slash >= 0) ? path.substring(slash + 1) : path;

    String lower = path; lower.toLowerCase();
    if (lower.endsWith(".osac")) {
        String name; uint16_t c; bool isApp, isE; uint8_t p;
        if (readOsacHeader(path, name, c, isApp, isE, p) && name.length() > 0) return name;
        return fallback;
    }

    if (!isSdReady) return fallback;
    File f = SD.open(path);
    if (!f) return fallback;

    // Scan the whole file — #app may not be the first line (comments, blanks).
    while (f.available()) {
        String raw;
        if (!readLineLimited(f, raw, OSA_MAX_LINE_BYTES)) { f.close(); return fallback; }
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
    String lower = path; lower.toLowerCase();
    if (lower.endsWith(".osac")) {
        String n; uint16_t c; bool isApp, isE; uint8_t p = 0;
        if (readOsacHeader(path, n, c, isApp, isE, p)) return p;
        return 0;
    }
    if (!isSdReady) return 0;
    File f = SD.open(path);
    if (!f) return 0;

    uint8_t mask = 0;
    while (f.available()) {
        String raw;
        if (!readLineLimited(f, raw, OSA_MAX_LINE_BYTES)) { f.close(); return 0; }
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
    String lower = path; lower.toLowerCase();
    if (lower.endsWith(".osac")) {
        String n; uint16_t c; bool isApp = false, isE; uint8_t p;
        if (readOsacHeader(path, n, c, isApp, isE, p)) return isApp;
        return false;
    }
    if (!isSdReady) return false;
    File f = SD.open(path);
    if (!f) return false;
    while (f.available()) {
        String raw;
        if (!readLineLimited(f, raw, OSA_MAX_LINE_BYTES)) { f.close(); return false; }
        raw.trim();
        if (!raw.startsWith("#isApp")) continue;
        String v = raw.substring(6); v.trim(); v.toLowerCase();
        f.close();
        return v == "true" || v == "1" || v == "yes";
    }
    f.close();
    return false;
}

bool OSARuntime::readIsExceptionFromFile(const String& path) {
    // A #exception header or serialized flag is never enough on its own.
    // Privilege is derived from a fixed legacy allowlist or the recognized
    // entry point of an allowlisted system OPK. Remote authenticity still
    // requires the future signed-catalog layer described in store/README.md.
    // A `#exception true` header in a user-supplied script is intentionally
    // ignored — that would let anyone with an SD reader grant themselves root.
    String lower = path;
    lower.toLowerCase();
    static const char* legacy[] = {
        "/system/apps/home.osa",          "/system/apps/home.osac",
        "/system/apps/lockscreen.osa",    "/system/apps/lockscreen.osac",
        "/system/apps/controlcenter.osa", "/system/apps/controlcenter.osac",
        "/system/apps/settings.osa",      "/system/apps/settings.osac",
        "/system/apps/files.osa",         "/system/apps/files.osac",
        "/system/apps/clock.osa",         "/system/apps/clock.osac",
        "/system/apps/calculator.osa",    "/system/apps/calculator.osac",
        "/system/apps/notes.osa",         "/system/apps/notes.osac",
        "/system/apps/compiler.osa",      "/system/apps/compiler.osac",
        "/system/apps/openstore.osa",     "/system/apps/openstore.osac"
    };
    for (const char* trusted : legacy) if (lower == trusted) return true;
    return PackageManager::isTrustedSystemEntry(path);
}

uint16_t OSARuntime::readIconColorFromFile(const String& path, uint16_t fallback) {
    String lower = path; lower.toLowerCase();
    if (lower.endsWith(".osac")) {
        String n; uint16_t c = 0; bool isApp, isE; uint8_t p;
        if (readOsacHeader(path, n, c, isApp, isE, p) && c != 0) return c;
        return fallback;
    }
    if (!isSdReady) return fallback;
    File f = SD.open(path);
    if (!f) return fallback;
    while (f.available()) {
        String raw;
        if (!readLineLimited(f, raw, OSA_MAX_LINE_BYTES)) { f.close(); return fallback; }
        raw.trim();
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
    const int cx = 20, cw = 200, cr = 14;
    const int bodyMaxWidth = cw - 28;
    const int maxBodyLines = 8;
    const int bodyLineHeight = 14;

    String bodyLines[maxBodyLines];
    tft->setTextFont(1); tft->setTextSize(1);
    int bodyLineCount = popupWrapBody(tft, body1, body2, bodyLines,
                                      maxBodyLines, bodyMaxWidth);
    const int contentHeight = 91 + bodyLineCount * bodyLineHeight;
    const int ch = max(126, contentHeight);
    const int cy = (320 - ch) / 2;

    // Snapshot the popup region so we can restore the underlying script UI on dismiss.
    // (~60 KB for a 200x150 area — well under the heap budget.) Falls back to redraw
    // hint if the allocation fails on a tight system.
    uint16_t* snapshot = (uint16_t*)malloc((size_t)cw * ch * sizeof(uint16_t));
    if (snapshot) tft->readRect(cx, cy, cw, ch, snapshot);

    tft->fillRoundRect(cx, cy, cw, ch, cr, TFT_WHITE);
    tft->drawRoundRect(cx, cy, cw, ch, cr, tft->color565(220, 220, 222));

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(tft->color565(20, 20, 22));
    tft->drawString(popupFitLine(tft, title, cw - 24), 120, cy + 24);

    tft->setTextFont(1);
    tft->setTextColor(tft->color565(110, 110, 118));
    for (int i = 0; i < bodyLineCount; ++i)
        tft->drawString(bodyLines[i], 120, cy + 48 + i * bodyLineHeight);

    const int divY = cy + ch - 44;
    tft->drawFastHLine(cx, divY, cw, tft->color565(218, 218, 222));
    tft->drawFastVLine(cx + cw / 2, divY, 44, tft->color565(218, 218, 222));

    tft->setTextFont(2);
    tft->setTextColor(tft->color565(0, 122, 255));
    tft->drawString(popupFitLine(tft, leftBtn, cw / 2 - 16),
                    cx + cw / 4, cy + ch - 22);
    uint16_t rCol = rightDanger ? tft->color565(255, 59, 48) : tft->color565(52, 199, 89);
    tft->setTextColor(rCol);
    tft->drawString(popupFitLine(tft, rightBtn, cw / 2 - 16),
                    cx + cw * 3 / 4, cy + ch - 22);

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

    BuiltinBudgetGuard builtinBudget(scriptSliceStarted);

    String argT[12];
    OSAVal a[12];
    int argc = 0;
    if (directBuiltinArgs) {
        argc = min(directBuiltinArgc, 12);
        for (int i = 0; i < argc; ++i)
            a[i] = static_cast<OSAVal&&>(directBuiltinArgs[i]);
        directBuiltinArgs = nullptr;
        directBuiltinArgc = 0;
    } else {
        splitArgs(argsStr, argT, argc);
        // Tree-walker path evaluates source arguments eagerly.
        for (int i = 0; i < argc; i++) a[i] = eval(argT[i]);
    }

    auto N = [&](int i, double def = 0) -> double {
        return (i < argc) ? a[i].toNum() : def;
    };
    auto S = [&](int i, const String& def = "") -> String {
        return (i < argc) ? a[i].toString() : def;
    };
    auto iN = [&](int i, int def = 0) -> int { return (int)N(i, def); };

    // ── Screen drawing ────────────────────────────────────────────────────────

    if (name == "clear" || name == "cls") {
        uint16_t c = (sysTheme == 1) ? tft->color565(28, 28, 30) : TFT_WHITE;
        if (activeSprite) activeSprite->fillSprite(c);
        else              tft->fillScreen(c);
        return OSAVal();
    }
    if (name == "bg") {
        uint16_t c = tft->color565(iN(0), iN(1), iN(2));
        if (activeSprite) activeSprite->fillSprite(c);
        else              tft->fillScreen(c);
        return OSAVal();
    }
    if (name == "text") {
        CV(setTextFont(textFont));
        CV(setTextSize(1));
        CV(setTextColor(txtColor));
        CV(setTextDatum(TL_DATUM));
        CV(drawString(S(2), iN(0), iN(1)));
        return OSAVal();
    }
    if (name == "textc") {
        CV(setTextFont(textFont));
        CV(setTextSize(1));
        CV(setTextColor(txtColor));
        CV(setTextDatum(MC_DATUM));
        CV(drawString(S(2), iN(0), iN(1)));
        return OSAVal();
    }
    if (name == "rect") {
        CV(fillRect(iN(0), iN(1), iN(2), iN(3), drawColor));
        return OSAVal();
    }
    if (name == "rrect") {
        CV(fillRoundRect(iN(0), iN(1), iN(2), iN(3), iN(4), drawColor));
        return OSAVal();
    }
    if (name == "frame") {
        CV(drawRect(iN(0), iN(1), iN(2), iN(3), drawColor));
        return OSAVal();
    }
    if (name == "circle") {
        CV(fillCircle(iN(0), iN(1), iN(2), drawColor));
        return OSAVal();
    }
    if (name == "ring") {
        CV(drawCircle(iN(0), iN(1), iN(2), drawColor));
        return OSAVal();
    }
    if (name == "line") {
        CV(drawLine(iN(0), iN(1), iN(2), iN(3), drawColor));
        return OSAVal();
    }
    if (name == "pixel") {
        CV(drawPixel(iN(0), iN(1), drawColor));
        return OSAVal();
    }
    if (name == "setcolor") {
        drawColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "setcolor565")  { drawColor = (uint16_t)iN(0); return OSAVal(); }
    if (name == "textcolor") {
        txtColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "textcolor565") { txtColor  = (uint16_t)iN(0); return OSAVal(); }
    if (name == "fontsize") {
        // TFT_eSPI ships fonts 1, 2, 4, 6, 7 — 6 is digits-only (big clock
        // glyphs), 7 is 7-segment. Anything in between rounds down.
        int fs = iN(0);
        if      (fs >= 7) textFont = 7;
        else if (fs >= 6) textFont = 6;
        else if (fs >= 4) textFont = 4;
        else if (fs >= 2) textFont = 2;
        else              textFont = 1;
        return OSAVal();
    }
    // textfit(text, width) returns a single line shortened with "...".
    if (name == "textfit") {
        tft->setTextFont(textFont); tft->setTextSize(1);
        return OSAVal(popupFitLine(tft, S(0), max(1, iN(1))));
    }
    // textblock(x, y, width, text, lineHeight, scroll, clipTop, clipBottom,
    //           [maxLines]) draws word-wrapped text and returns content height.
    // Only visible complete lines are painted, so a fixed header can be drawn
    // independently while the block scrolls underneath it.
    if (name == "textblock") {
        int x = iN(0), y = iN(1), width = max(1, iN(2));
        String numericText;
        const String* blockText = nullptr;
        if (argc > 3 && !a[3].isNum) blockText = &a[3].str;
        else {
            numericText = argc > 3 ? a[3].toString() : String("");
            blockText = &numericText;
        }
        tft->setTextFont(textFont); tft->setTextSize(1);
        if (activeSprite) {
            activeSprite->setTextFont(textFont); activeSprite->setTextSize(1);
            activeSprite->setTextColor(txtColor); activeSprite->setTextDatum(TL_DATUM);
        } else {
            tft->setTextColor(txtColor); tft->setTextDatum(TL_DATUM);
        }
        int fontHeight = tft->fontHeight();
        int lineHeight = max(fontHeight, iN(4, fontHeight + 2));
        int clipTop = max(0, iN(6, 0));
        int clipBottom = min(320, iN(7, 320));
        WrappedDrawContext context = { tft, activeSprite, x, y, lineHeight,
                                       max(0, iN(5, 0)), clipTop, clipBottom,
                                       fontHeight };
        int lineCount = visitWrappedText(tft, *blockText, width, iN(8, 0),
                                         drawWrappedLine, &context);
        return OSAVal((double)(lineCount * lineHeight));
    }
    if (name == "screenw") return OSAVal(240.0);
    if (name == "screenh") return OSAVal(320.0);

    // ── Off-screen sprite (double buffering for smooth animations) ────────────
    //   ok = gfx.begin(w, h)   — allocate; subsequent draws go to the sprite
    //   gfx.push(x, y)         — blit sprite to TFT at (x, y)
    //   gfx.end()              — release sprite, drawing returns to direct TFT
    if (name == "gfx.begin") {
        // Optional 3rd arg: depth 1/8/16 (default 16). 8-bit halves memory
        // (256-color palette) — perfect for solid-color sprites.
        int w = iN(0), h = iN(1);
        int depth = (argc >= 3) ? iN(2) : 16;
        if (depth != 1 && depth != 8 && depth != 16) depth = 16;
        if (w <= 0 || h <= 0) return OSAVal(0.0);
        if (activeSprite) {
            activeSprite->deleteSprite();
            delete activeSprite;
            activeSprite = nullptr;
        }
        activeSprite = new TFT_eSprite(tft);
        if (!activeSprite) return OSAVal(0.0);
        activeSprite->setColorDepth(depth);
        if (!activeSprite->createSprite(w, h)) {
            delete activeSprite;
            activeSprite = nullptr;
            return OSAVal(0.0);
        }
        activeSprite->fillSprite(TFT_BLACK);
        return OSAVal(1.0);
    }
    if (name == "gfx.push") {
        if (!activeSprite) return OSAVal(0.0);
        activeSprite->pushSprite(iN(0), iN(1));
        return OSAVal(1.0);
    }
    if (name == "gfx.pushClip") {
        if (!activeSprite) return OSAVal(0.0);
        int clipX = iN(2), clipY = iN(3);
        int clipW = iN(4), clipH = iN(5);
        if (clipW <= 0 || clipH <= 0) return OSAVal(0.0);
        tft->setViewport(clipX, clipY, clipW, clipH, false);
        activeSprite->pushSprite(iN(0), iN(1));
        tft->resetViewport();
        return OSAVal(1.0);
    }
    if (name == "gfx.origin") {
        if (!activeSprite) return OSAVal(0.0);
        // Drawing coordinates are translated before sprite clipping. This
        // lets a small reusable stripe render a window of a much taller UI.
        activeSprite->setOrigin(iN(0), iN(1));
        return OSAVal(1.0);
    }
    if (name == "gfx.end") {
        if (activeSprite) {
            activeSprite->deleteSprite();
            delete activeSprite;
            activeSprite = nullptr;
        }
        if (stashSprite) {
            stashSprite->deleteSprite();
            delete stashSprite;
            stashSprite = nullptr;
        }
        return OSAVal();
    }
    if (name == "gfx.active") return OSAVal(activeSprite ? 1.0 : 0.0);
    // ── gfx.stash / gfx.show / gfx.unstash ────────────────────────────────
    // "Build once, blit many" pattern: build a sprite with gfx.begin + draws,
    // call gfx.stash() to detach it (drawing returns to screen but the buffer
    // stays alive), then gfx.show(x, y) to blit it on demand. gfx.unstash
    // re-activates the stashed sprite for further drawing.
    if (name == "gfx.stash") {
        if (activeSprite) {
            if (stashSprite) {
                stashSprite->deleteSprite();
                delete stashSprite;
            }
            stashSprite = activeSprite;
            activeSprite = nullptr;
        }
        return OSAVal();
    }
    if (name == "gfx.show") {
        if (!stashSprite) return OSAVal(0.0);
        stashSprite->pushSprite(iN(0), iN(1));
        return OSAVal(1.0);
    }
    if (name == "gfx.unstash") {
        if (activeSprite) {
            activeSprite->deleteSprite();
            delete activeSprite;
        }
        activeSprite = stashSprite;
        stashSprite = nullptr;
        return OSAVal();
    }

    // ── Touch ─────────────────────────────────────────────────────────────────

    if (name == "touch.down") {
        sampleTouch();
        return OSAVal(touchSampleDown ? 1.0 : 0.0);
    }
    if (name == "touch.x") {
        sampleTouch();
        return OSAVal((double)touchSampleX);
    }
    if (name == "touch.y") {
        sampleTouch();
        return OSAVal((double)touchSampleY);
    }

    // ── System ────────────────────────────────────────────────────────────────

    if (name == "wait") {
        // Poll the global exit gesture during long waits so scripts with custom
        // touch loops (no ui.menu*) can still be swiped up to home.
        int ms = iN(0);
        if (ms <= 0) return OSAVal();
        unsigned long start = millis();
        while ((long)(millis() - start) < ms) {
            if (checkExitGesture() || checkOverlayGesture()) {
                // Waiting is an explicit cooperative yield, not script CPU time.
                // Start a fresh compute slice before returning to the script.
                scriptSliceStarted = millis();
                scriptOpsSinceYield = 0;
                scriptLoopOpsTotal = 0;
                return OSAVal();
            }
            yield();
            delay(5);
        }
        // Long-running UI/event loops intentionally call wait() every frame.
        // Without resetting the slice here, a one-second hold is mistaken for
        // an unresponsive script even though it continuously feeds the WDT.
        scriptSliceStarted = millis();
        scriptOpsSinceYield = 0;
        scriptLoopOpsTotal = 0;
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
    static const String emptyJson;
    const String& jsonSource = (argc > 0 && !a[0].isNum) ? a[0].str : emptyJson;
    if (name == "json.get") {
        String raw = jsonWalkPath(jsonSource, S(1));
        return OSAVal(jsonUnquote(raw));
    }
    if (name == "json.raw") {
        // Returns the raw JSON fragment without unquoting — useful for nested
        // objects you want to feed back into json.get later.
        return OSAVal(jsonWalkPath(jsonSource, S(1)));
    }
    if (name == "json.has") {
        return OSAVal(jsonWalkSpan(jsonSource, S(1)).valid() ? 1.0 : 0.0);
    }
    if (name == "json.size") {
        JsonSpan span = jsonWalkSpan(jsonSource, S(1));
        return OSAVal(span.valid() ? (double)jsonContainerSize(jsonSource, span.start) : 0.0);
    }

    // ── HTTP (network permission) ────────────────────────────────────────────
    if (name == "url_encode") return OSAVal(urlEncode(S(0)));
    if (name == "http.status") return OSAVal((double)s_httpStatus);
    if (name == "http.error") return OSAVal(s_httpError);
    if (name == "http.bearer") {
        if (!checkPerm(OSA_PERM_NETWORK, "Network",
                       "Send authenticated requests"))
            return OSAVal();
        if (argc < 1 || a[0].isNum || a[0].str.length() > 4096 ||
            a[0].str.indexOf('\r') >= 0 || a[0].str.indexOf('\n') >= 0) {
            s_httpError = "Bearer token exceeds 4096 byte limit";
            return OSAVal(0.0);
        }
        s_httpBearer = static_cast<String&&>(a[0].str);
        return OSAVal();
    }
    if (name == "http.get" || name == "http.post") {
        s_httpError = "";
        if (!checkPerm(OSA_PERM_NETWORK, "Network",
                       "Make HTTP requests over Wi-Fi"))
            return OSAVal("");
        if (WiFi.status() != WL_CONNECTED) {
            s_httpStatus = -1;
            s_httpError = "Wi-Fi is not connected";
            return OSAVal("");
        }
        String url = (argc > 0 && !a[0].isNum)
                   ? static_cast<String&&>(a[0].str) : S(0);
        if (url.length() == 0 || url.length() > 2048 ||
            url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0) {
            s_httpStatus = -2;
            s_httpError = "HTTP URL exceeds 2048 byte limit";
            return OSAVal("");
        }
        if (!url.startsWith("https://") && !url.startsWith("http://")) {
            s_httpStatus = -2;
            s_httpError = "Invalid URL scheme";
            return OSAVal("");
        }
        size_t requestLength = 0;
        if (name == "http.post" && argc >= 2) {
            requestLength = a[1].isNum ? a[1].toString().length()
                                       : a[1].str.length();
        }
        if (name == "http.post" && requestLength > OSA_HTTP_MAX_SEND) {
            s_httpStatus = -3;
            s_httpError = "HTTP request body too large";
            return OSAVal("");
        }
        if (ESP.getFreeHeap() < OSA_HTTP_MAX_BODY + 12 * 1024) {
            s_httpStatus = -4;
            s_httpError = "Not enough memory for HTTP response";
            return OSAVal("");
        }
        // HTTPS requires a secure client; HTTP uses the default client.
        bool secure = url.startsWith("https://");
        if (!secure && s_httpBearer.length() > 0) {
            s_httpBearer = "";
            s_httpStatus = -2;
            s_httpError = "Bearer credentials require HTTPS";
            return OSAVal("");
        }
        HTTPClient http;
        http.setConnectTimeout(8000);
        http.setTimeout(10000);
        WiFiClientSecure secureClient;
        bool ok = false;
        if (secure) {
            secureClient.setInsecure(); // encrypted, but no server identity verification
            ok = http.begin(secureClient, url);
        } else {
            ok = http.begin(url);
        }
        if (!ok) {
            s_httpStatus = -2;
            s_httpError = "Could not open HTTP connection";
            return OSAVal("");
        }

        // Credentials apply to one request only. Reusing them automatically
        // for a later, unrelated host could leak an API key.
        String bearer = static_cast<String&&>(s_httpBearer);
        s_httpBearer = "";
        if (bearer.length() > 0)
            http.addHeader("Authorization", "Bearer " + bearer);

        int code;
        if (name == "http.get") {
            code = http.GET();
        } else {
            String body = (argc >= 2 && !a[1].isNum)
                        ? static_cast<String&&>(a[1].str) : S(1);
            String ctype = (argc >= 3) ? S(2) : "application/json";
            if (ctype.length() > 128 || ctype.indexOf('\r') >= 0 ||
                ctype.indexOf('\n') >= 0) {
                s_httpStatus = -3;
                s_httpError = "Content-Type exceeds 128 byte limit";
                http.end();
                return OSAVal("");
            }
            http.addHeader("Content-Type", ctype);
            code = http.POST(body);
        }
        s_httpStatus = code;
        if (code <= 0) {
            s_httpError = HTTPClient::errorToString(code);
            http.end();
            return OSAVal("");
        }

        int declaredLength = http.getSize();
        if (declaredLength > (int)OSA_HTTP_MAX_BODY) {
            s_httpStatus = -3;
            s_httpError = "HTTP response exceeds 24576 byte limit";
            http.end();
            return OSAVal("");
        }

        BoundedStringStream sink(OSA_HTTP_MAX_BODY);
        size_t reserveHint = declaredLength > 0 ? (size_t)declaredLength : 1024;
        if (!sink.begin(reserveHint)) {
            s_httpStatus = -4;
            s_httpError = "Not enough memory for HTTP response";
            http.end();
            return OSAVal("");
        }
        int received = http.writeToStream(&sink);
        http.end();
        if (sink.tooLarge()) {
            s_httpStatus = -3;
            s_httpError = "HTTP response exceeds 24576 byte limit";
            return OSAVal("");
        }
        if (sink.outOfMemory()) {
            s_httpStatus = -4;
            s_httpError = "Not enough memory for HTTP response";
            return OSAVal("");
        }
        if (received < 0) {
            s_httpStatus = -5;
            s_httpError = "HTTP response read failed";
            return OSAVal("");
        }
        return OSAVal(sink.take());
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

    if (name == "io.error") return OSAVal(s_ioError);
    if (name == "asset.path") {
        s_ioError = "";
        String path = packageAssetPath(S(0));
        if (path.length() == 0 || !SD.exists(path.c_str())) {
            s_ioError = "Package asset not found";
            return OSAVal("");
        }
        return OSAVal(static_cast<String&&>(path));
    }
    if (name == "asset.exists") {
        String path = packageAssetPath(S(0));
        return OSAVal(path.length() > 0 && SD.exists(path.c_str()) ? 1.0 : 0.0);
    }
    if (name == "asset.size") {
        s_ioError = "";
        String path = packageAssetPath(S(0));
        File f = path.length() > 0 ? SD.open(path) : File();
        if (!f || f.isDirectory()) {
            if (f) f.close();
            s_ioError = "Package asset not found";
            return OSAVal(-1.0);
        }
        double size = (double)f.size();
        f.close();
        return OSAVal(size);
    }
    if (name == "asset.read") {
        String path = packageAssetPath(S(0));
        if (path.length() == 0) { s_ioError = "Invalid package asset path"; return OSAVal(""); }
        size_t offset = (size_t)max(0, iN(1));
        bool explicitLength = argc >= 3;
        size_t requested = explicitLength ? (size_t)max(0, iN(2)) : 0;
        String content;
        if (!readFileBounded(path, offset, requested, explicitLength, content))
            return OSAVal("");
        return OSAVal(static_cast<String&&>(content));
    }
    if (name == "fsize") {
        s_ioError = "";
        if (!isSdReady) { s_ioError = "No SD card"; return OSAVal(-1.0); }
        File f = SD.open(sandboxPath(S(0)));
        if (!f || f.isDirectory()) {
            if (f) f.close();
            s_ioError = "File not found";
            return OSAVal(-1.0);
        }
        double size = (double)f.size();
        f.close();
        return OSAVal(size);
    }
    if (name == "fread") {
        if (!isSdReady) { s_ioError = "No SD card"; return OSAVal(""); }
        size_t offset = (size_t)max(0, iN(1));
        bool explicitLength = argc >= 3;
        size_t requested = explicitLength ? (size_t)max(0, iN(2)) : 0;
        String content;
        if (!readFileBounded(sandboxPath(S(0)), offset, requested,
                             explicitLength, content)) return OSAVal("");
        return OSAVal(static_cast<String&&>(content));
    }
    if (name == "freadline") {
        s_ioError = "";
        if (!isSdReady) { s_ioError = "No SD card"; return OSAVal(""); }
        File f = SD.open(sandboxPath(S(0)));
        if (!f) { s_ioError = "File not found"; return OSAVal(""); }
        int target = iN(1);
        if (target < 0) { f.close(); s_ioError = "Invalid line number"; return OSAVal(""); }
        for (int i = 0; i <= target; i++) {
            String line;
            if (!readLineLimited(f, line, OSA_MAX_FILE_READ)) {
                f.close(); s_ioError = "Line exceeds 32768 byte limit"; return OSAVal("");
            }
            if (i == target) {
                f.close(); line.trim(); return OSAVal(static_cast<String&&>(line));
            }
            if (!f.available()) break;
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
        size_t wanted = (size_t)src.length() * (size_t)n;
        if (src.length() > 0 && (wanted / src.length() != (size_t)n || wanted > 32768))
            return OSAVal("");
        String out;
        if (!out.reserve(wanted)) return OSAVal("");
        for (int i = 0; i < n; i++) out += src;
        return OSAVal(out);
    }
    if (name == "padleft" || name == "padright") {
        String src = S(0); int n = iN(1);
        if (n < 0 || n > 32768) return OSAVal("");
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
                String ln;
                if (!readLineLimited(rf, ln, 2048)) break;
                ln.trim();
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
        InlineKbd kbd(tft, ts);
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
        bool   showBack  = (argc >= 3) ? (iN(2) != 0) : false;
        const int MAX_ITEMS = 32;
        String items[MAX_ITEMS];
        int    itemCount = 0;
        int start = 0;
        for (int i = 0; i <= (int)items_str.length() && itemCount < MAX_ITEMS; i++) {
            if (i == (int)items_str.length() || items_str[i] == '|') {
                items[itemCount++] = items_str.substring(start, i);
                start = i + 1;
            }
        }

        const int rowH       = 44;
        const int headerH    = showBack ? 50 : 40;
        const int rowY0      = headerH + 10;
        const int viewportH  = 320 - rowY0;
        const int contentH   = itemCount * rowH;
        const bool scrollable = contentH > viewportH;
        const int maxScroll  = scrollable ? (contentH - viewportH) : 0;
        int scrollY = 0;

        auto paint = [&]() {
            tft->fillScreen(Theme::bg());
            // Rows first; header is repainted on top so scrolled rows don't bleed.
            for (int i = 0; i < itemCount; i++) {
                int y = rowY0 + i * rowH - scrollY;
                if (y + rowH <= rowY0) continue;
                if (y >= rowY0 + viewportH) break;
                tft->fillRect(0, y, 240, rowH - 1, Theme::surface());
                tft->drawFastHLine(0, y + rowH - 1, 240, Theme::divider2());
                tft->setTextFont(2);
                tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM);
                tft->drawString(items[i], 16, y + rowH / 2);
                tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
                int valX = scrollable ? 217 : 222;
                tft->drawString(">", valX, y + rowH / 2);
            }
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

            if (scrollable) {
                int barH = viewportH * viewportH / contentH;
                if (barH < 12) barH = 12;
                int barY = rowY0 + (viewportH - barH) * scrollY /
                                    (maxScroll > 0 ? maxScroll : 1);
                tft->fillRect(236, rowY0, 4, viewportH, Theme::divider2());
                tft->fillRect(236, barY, 4, barH, Theme::hint());
            }
        };

        paint();
        enforceTapGap(ts);
        waitFullRelease(ts);

        bool wasTouched = false;
        int  startY = 0, startX = 0, startScroll = 0;
        bool didScroll = false;
        while (true) {
            yield();
            if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);
            if (ts->touched()) {
                TS_Point p = ts->getPoint();
                int tx = map(p.x, 300, 3800, 0, 240);
                int ty = map(p.y, 300, 3800, 0, 320);
                if (!wasTouched) {
                    wasTouched = true;
                    startX = tx; startY = ty; startScroll = scrollY; didScroll = false;
                } else if (scrollable) {
                    int dy = ty - startY;
                    if (!didScroll && abs(dy) > 8) didScroll = true;
                    if (didScroll) {
                        int newScroll = startScroll - dy;
                        if (newScroll < 0) newScroll = 0;
                        if (newScroll > maxScroll) newScroll = maxScroll;
                        if (newScroll != scrollY) { scrollY = newScroll; paint(); }
                    }
                }
            } else if (wasTouched) {
                wasTouched = false;
                if (!didScroll) {
                    if (showBack && startY < headerH && startX < 80) {
                        markTapAccepted();
                        return OSAVal(-1.0);
                    }
                    if (startY >= rowY0 && startY < rowY0 + viewportH) {
                        int idx = (startY - rowY0 + scrollY) / rowH;
                        if (idx >= 0 && idx < itemCount) {
                            markTapAccepted();
                            return OSAVal((double)idx);
                        }
                    }
                }
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
        if (name == "ui.menuStart") {
            // Optional second arg: showBack (1 = draw "< Back" in header).
            // Defaults to 0 so the root menu doesn't get an unwanted button.
            clearRichMenuCache();
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
            const int headerH    = s_rmShowBack ? 50 : 40;
            const int rowH       = 31;
            // Use the full screen below the header. The last row may be
            // partially clipped at the screen edge — better than leaving an
            // obvious empty strip at the bottom.
            const int viewportH  = 320 - headerH;
            const int contentH   = s_rmCount * rowH;
            const bool scrollable = contentH > viewportH;
            const int maxScroll  = scrollable ? (contentH - viewportH) : 0;
            int scrollY = 0;

            auto paint = [&]() {
                tft->fillScreen(Theme::bg());

                // Render rows first so a partially-scrolled row that overlaps
                // the header area can be hidden by repainting the header on top.
                for (int i = 0; i < s_rmCount; i++) {
                    int y = headerH + i * rowH - scrollY;
                    if (y + rowH <= headerH) continue;
                    if (y >= headerH + viewportH) break;
                    tft->fillRect(0, y, 240, rowH, Theme::surface());
                    if (i < s_rmCount - 1)
                        tft->drawFastHLine(0, y + rowH, 240, Theme::divider2());
                    tft->fillRoundRect(14, y + 3, 24, 24, 6, s_rmColors[i]);
                    tft->setTextColor(TFT_WHITE); tft->setTextDatum(MC_DATUM);
                    tft->setTextFont(2); tft->setTextSize(1);
                    tft->drawString(s_rmLetters[i], 26, y + 15);
                    tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM);
                    int valX = scrollable ? 220 : 225;
                    tft->drawString(s_rmTitles[i], 46, y + 15);
                    tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
                    tft->drawString(s_rmValues[i], valX, y + 15);
                }

                // Header repainted on top — covers any bleed from a scrolled row.
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

                // Scrollbar spans the full viewport so the track length matches
                // the row strip exactly.
                if (scrollable) {
                    int barH = viewportH * viewportH / contentH;
                    if (barH < 12) barH = 12;
                    int barY = headerH + (viewportH - barH) * scrollY /
                                          (maxScroll > 0 ? maxScroll : 1);
                    tft->fillRect(236, headerH, 4, viewportH, Theme::divider2());
                    tft->fillRect(236, barY, 4, barH, Theme::hint());
                }
            };

            paint();
            enforceTapGap(ts);
            waitFullRelease(ts);

            bool wasTouched      = false;
            int  touchStartY     = 0;
            int  touchStartX     = 0;
            int  touchStartScroll = 0;
            bool didScroll       = false;
            int  lastY           = 0;

            while (true) {
                yield();
                if (checkExitGesture() || checkOverlayGesture()) return OSAVal(-1.0);

                if (ts->touched()) {
                    TS_Point p = ts->getPoint();
                    int tx = map(p.x, 300, 3800, 0, 240);
                    int ty = map(p.y, 300, 3800, 0, 320);

                    if (!wasTouched) {
                        wasTouched      = true;
                        touchStartY     = ty;
                        touchStartX     = tx;
                        touchStartScroll = scrollY;
                        didScroll       = false;
                        lastY           = ty;
                    } else if (scrollable) {
                        int dy = ty - touchStartY;
                        if (!didScroll && abs(dy) > 8) didScroll = true;
                        if (didScroll) {
                            int newScroll = touchStartScroll - (ty - touchStartY);
                            if (newScroll < 0) newScroll = 0;
                            if (newScroll > maxScroll) newScroll = maxScroll;
                            if (newScroll != scrollY) {
                                scrollY = newScroll;
                                paint();
                            }
                        }
                        lastY = ty;
                    }
                } else if (wasTouched) {
                    wasTouched = false;
                    if (!didScroll) {
                        // Pure tap — hit-test against the (possibly scrolled) list.
                        int ty = touchStartY, tx = touchStartX;
                        if (s_rmShowBack && ty < headerH && tx < 80) {
                            markTapAccepted();
                            return OSAVal(-1.0);
                        }
                        if (ty >= headerH && ty < headerH + viewportH) {
                            int idx = (ty - headerH + scrollY) / rowH;
                            if (idx >= 0 && idx < s_rmCount) {
                                markTapAccepted();
                                return OSAVal((double)idx);
                            }
                        }
                    }
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
        // Compact layout: 4×3 grid sized so the bottom row ends at y=290,
        // safely above the swipe-up dead zone (ty > 290) checked below.
        const int btn = 50, gap = 5;
        const int gridW = 3 * btn + 2 * gap;          // 160
        const int gridX = (240 - gridW) / 2;          // 40
        const int gridY = 75;                         // row 3 bottom = 290
        const char* keys = "123456789<0>";

        auto drawHeader = [&]() {
            tft->fillRect(0, 0, 240, 45, Theme::header());
            tft->drawFastHLine(0, 45, 240, Theme::divider());
            tft->setTextFont(2); tft->setTextColor(tft->color565(0, 122, 255));
            tft->setTextDatum(ML_DATUM);
            tft->drawString("< Back", 10, 22);
            tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
            tft->drawString(prompt, 120, 22);
        };
        auto drawDots = [&]() {
            tft->fillRect(0, 48, 240, 24, Theme::bg());
            int dotW = 20 * maxDigits; if (dotW > 220) dotW = 220;
            int dotX = (240 - dotW) / 2;
            int step = dotW / maxDigits;
            for (int i = 0; i < maxDigits; i++) {
                int cx = dotX + i * step + step / 2;
                if (i < (int)buf.length())
                    tft->fillCircle(cx, 62, 6, Theme::text());
                else
                    tft->drawCircle(cx, 62, 6, Theme::divider2());
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

    // OpenStore package API. Installation is restricted to a trusted system
    // app and still requires an explicit confirmation on the device.
    if (name == "store.catalog") {
        if (!needException("store.catalog")) return OSAVal("");
        return OSAVal(PackageManager::fetchCatalog());
    }
    if (name == "store.source") {
        if (!needException("store.source")) return OSAVal("");
        return OSAVal(PackageManager::catalogSourceUrl());
    }
    if (name == "store.setSource") {
        if (!needException("store.setSource")) return OSAVal(0.0);
        return OSAVal(PackageManager::setCatalogSourceUrl(S(0)) ? 1.0 : 0.0);
    }
    if (name == "store.systemSource") {
        if (!needException("store.systemSource")) return OSAVal("");
        return OSAVal(PackageManager::systemPackageSourcePrefix());
    }
    if (name == "store.setSystemSource") {
        if (!needException("store.setSystemSource")) return OSAVal(0.0);
        String prefix = S(0);
        if (!showSystemPopup("System package source", prefix,
                             "Changes privileged update origin",
                             "Cancel", "Change", true)) return OSAVal(0.0);
        return OSAVal(PackageManager::setSystemPackageSourcePrefix(prefix) ? 1.0 : 0.0);
    }
    if (name == "store.refresh") {
        if (!needException("store.refresh")) return OSAVal(-1.0);
        return OSAVal(PackageManager::refreshCatalog()
                    ? (double)PackageManager::catalogCount() : -1.0);
    }
    if (name == "store.count") {
        if (!needException("store.count")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::catalogCount());
    }
    if (name == "store.visibleCount") {
        if (!needException("store.visibleCount")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::catalogVisibleCount(iN(0) != 0));
    }
    if (name == "store.visibleItem") {
        if (!needException("store.visibleItem")) return OSAVal(-1.0);
        return OSAVal((double)PackageManager::catalogVisibleIndex(
            iN(0) != 0, iN(1)));
    }
    if (name == "store.state") {
        if (!needException("store.state")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::catalogItemState(iN(0)));
    }
    if (name == "store.canUninstall") {
        if (!needException("store.canUninstall")) return OSAVal(0.0);
        return OSAVal(PackageManager::catalogCanUninstall(iN(0)) ? 1.0 : 0.0);
    }
    if (name == "store.id") {
        if (!needException("store.id")) return OSAVal("");
        return OSAVal(PackageManager::catalogId(iN(0)));
    }
    if (name == "store.name") {
        if (!needException("store.name")) return OSAVal("");
        return OSAVal(PackageManager::catalogName(iN(0)));
    }
    if (name == "store.remoteVersion") {
        if (!needException("store.remoteVersion")) return OSAVal("");
        return OSAVal(PackageManager::catalogVersion(iN(0)));
    }
    if (name == "store.remoteVersionCode") {
        if (!needException("store.remoteVersionCode")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::catalogVersionCode(iN(0)));
    }
    if (name == "store.scope") {
        if (!needException("store.scope")) return OSAVal("");
        return OSAVal(PackageManager::catalogScope(iN(0)));
    }
    if (name == "store.summary") {
        if (!needException("store.summary")) return OSAVal("");
        return OSAVal(PackageManager::catalogSummary(iN(0)));
    }
    if (name == "store.developer" || name == "store.owner") {
        if (!needException("store.developer")) return OSAVal("");
        return OSAVal(PackageManager::catalogDeveloper(iN(0)));
    }
    if (name == "store.description") {
        if (!needException("store.description")) return OSAVal("");
        return OSAVal(PackageManager::catalogDescription(iN(0)));
    }
    if (name == "store.color") {
        if (!needException("store.color")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::catalogColor(iN(0)));
    }
    if (name == "store.url") {
        if (!needException("store.url")) return OSAVal("");
        return OSAVal(PackageManager::catalogUrl(iN(0)));
    }
    if (name == "store.sha256") {
        if (!needException("store.sha256")) return OSAVal("");
        return OSAVal(PackageManager::catalogSha256(iN(0)));
    }
    if (name == "store.error") {
        if (!needException("store.error")) return OSAVal("");
        return OSAVal(PackageManager::lastError());
    }
    if (name == "store.versionCode") {
        if (!needException("store.versionCode")) return OSAVal(0.0);
        return OSAVal((double)PackageManager::installedVersionCode(S(0)));
    }
    if (name == "store.version") {
        if (!needException("store.version")) return OSAVal("");
        return OSAVal(PackageManager::installedVersion(S(0)));
    }
    if (name == "store.restartRequired") {
        if (!needException("store.restartRequired")) return OSAVal(0.0);
        return OSAVal(PackageManager::restartRequired() ? 1.0 : 0.0);
    }
    if (name == "store.install") {
        if (!needException("store.install")) return OSAVal(0.0);
        String id = S(2);
        String scope = argc >= 4 ? S(3) : "user";
        if (scope != "user" && scope != "system") {
            setError(-1, "store.install: invalid package scope");
            return OSAVal(0.0);
        }
        int current = PackageManager::installedVersionCode(id);
        bool directUserInstall = argc >= 5 && iN(4) != 0 && scope == "user";
        String action;
        if (scope == "system") action = "Full system privileges";
        else action = current > 0 ? "Update installed package?" : "Install this package?";
        String title = scope == "system" ? "System update" : "OpenStore";
        String button = (scope == "system" || current > 0) ? "Update" : "Install";
        if (!directUserInstall &&
            !showSystemPopup(title, id, action, "Cancel", button, scope == "system"))
            return OSAVal(0.0);
        tft->fillScreen(Theme::bg());
        tft->fillRect(0, 0, 240, 44, Theme::header());
        tft->drawFastHLine(0, 44, 240, Theme::divider());
        tft->setTextDatum(MC_DATUM);
        tft->setTextFont(2);
        tft->setTextColor(Theme::text());
        tft->drawString("OpenStore", 120, 22);
        tft->drawString((scope == "system" || current > 0)
                        ? "Updating package..." : "Installing package...", 120, 138);
        tft->setTextFont(1);
        tft->setTextColor(Theme::hint());
        tft->drawString("Downloading, verifying and extracting", 120, 166);
        tft->drawString("Do not remove the SD card", 120, 184);
        bool ok = PackageManager::installFromUrl(S(0), S(1), id, scope);
        return OSAVal(ok ? 1.0 : 0.0);
    }
    if (name == "store.remove") {
        if (!needException("store.remove")) return OSAVal(0.0);
        String id = S(0);
        String label = argc >= 2 ? S(1) : id;
        if (!showSystemPopup("Remove app?", label, "App data is kept", "Cancel", "Remove", true))
            return OSAVal(0.0);
        bool removed = PackageManager::removeUserPackage(id);
        if (removed) home.removeScriptsUnder("/packages/" + id + "/");
        return OSAVal(removed ? 1.0 : 0.0);
    }

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
    if (name == "fs.size") {
        if (!needException("fs.size")) return OSAVal(-1.0);
        s_ioError = "";
        if (!isSdReady) { s_ioError = "No SD card"; return OSAVal(-1.0); }
        File f = SD.open(S(0));
        if (!f || f.isDirectory()) {
            if (f) f.close();
            s_ioError = "File not found";
            return OSAVal(-1.0);
        }
        double size = (double)f.size();
        f.close();
        return OSAVal(size);
    }
    if (name == "fs.read") {
        if (!needException("fs.read")) return OSAVal("");
        if (!isSdReady) { s_ioError = "No SD card"; return OSAVal(""); }
        size_t offset = (size_t)max(0, iN(1));
        bool explicitLength = argc >= 3;
        size_t requested = explicitLength ? (size_t)max(0, iN(2)) : 0;
        String content;
        if (!readFileBounded(S(0), offset, requested, explicitLength, content))
            return OSAVal("");
        return OSAVal(static_cast<String&&>(content));
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
        if (!osaSetBluetoothEnabled(true)) {
            showSystemPopup("Bluetooth", osaBluetoothLastError(),
                            "Bluetooth remains off", "", "OK", false);
            return OSAVal(0.0);
        }
        Config::setInt("bluetooth", 1); Config::save();
        return OSAVal(1.0);
    }
    if (name == "bt.disable") {
        if (!needException("bt.disable")) return OSAVal();
        osaSetBluetoothEnabled(false);
        Config::setInt("bluetooth", 0); Config::save();
        return OSAVal(1.0);
    }
    if (name == "bt.enabled") return OSAVal(sysBTEnabled ? 1.0 : 0.0);
    if (name == "bt.error") return OSAVal(String(osaBluetoothLastError()));

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
        // Recursive helper plus explicit OPK-root handling. Package entry
        // scripts live two levels below /packages, so a generic one-level SD
        // walk would otherwise omit them from Settings -> Applications.
        struct ScanCtx {
            static void add(const String& full) {
                if (s_appsCount >= APPS_MAX_SLOTS) return;
                String lower = full; lower.toLowerCase();
                if (!lower.endsWith(".osa") && !lower.endsWith(".osac")) return;
                for (int i = 0; i < s_appsCount; ++i)
                    if (s_appsPaths[i] == full) return;
                s_appsPaths[s_appsCount] = full;
                s_appsNames[s_appsCount] = OSARuntime::readAppNameFromFile(full);
                s_appsCount++;
            }

            static void walk(const String& dirPath, int depth) {
                if (s_appsCount >= APPS_MAX_SLOTS || depth > 1) return;
                File dir = SD.open(dirPath);
                if (!dir || !dir.isDirectory()) {
                    if (dir) dir.close();
                    return;
                }
                File f = dir.openNextFile();
                while (f && s_appsCount < APPS_MAX_SLOTS) {
                    String full = f.name();
                    if (!full.startsWith("/")) full = "/" + full;
                    bool directoryEntry = f.isDirectory();
                    f.close();
                    if (full == "/apps" || full.startsWith("/apps/")) {
                        f = dir.openNextFile();
                        continue;
                    }
                    if (directoryEntry) {
                        if (depth == 0) walk(full, 1);
                    } else add(full);
                    f = dir.openNextFile();
                }
                dir.close();
            }

            static void packages(const String& root, bool systemPackages) {
                File dir = SD.open(root);
                if (!dir || !dir.isDirectory()) {
                    if (dir) dir.close();
                    return;
                }
                File item = dir.openNextFile();
                while (item && s_appsCount < APPS_MAX_SLOTS) {
                    String id = item.name();
                    int slash = id.lastIndexOf('/');
                    if (slash >= 0) id = id.substring(slash + 1);
                    bool directoryEntry = item.isDirectory();
                    item.close();
                    if (directoryEntry && !id.startsWith(".") &&
                        (!systemPackages || PackageManager::isOfficialSystemId(id))) {
                        String entry = PackageManager::installedEntry(id, systemPackages);
                        if (entry.length() > 0) add(entry);
                    }
                    item = dir.openNextFile();
                }
                dir.close();
            }
        };

        if (name == "apps.scan") {
            if (!needException("apps.scan")) return OSAVal(0.0);
            clearAppsScanCache();
            if (isSdReady) {
                // User OPKs first: they are the entries whose grants the user
                // most often needs to inspect, and the UI has a finite row cap.
                ScanCtx::packages("/packages", false);
                ScanCtx::walk("/", 0);
                ScanCtx::walk("/system/apps", 1);
                ScanCtx::packages("/system/packages", true);
            }
            return OSAVal((double)s_appsCount);
        }
        if (name == "apps.name") {
            if (!needException("apps.name")) return OSAVal("");
            int i = iN(0);
            if (i < 0 || i >= s_appsCount) return OSAVal("");
            return OSAVal(s_appsNames[i]);
        }
        if (name == "apps.path") {
            if (!needException("apps.path")) return OSAVal("");
            int i = iN(0);
            if (i < 0 || i >= s_appsCount) return OSAVal("");
            return OSAVal(s_appsPaths[i]);
        }
        if (name == "apps.needsPerm") {
            if (!needException("apps.needsPerm")) return OSAVal(0.0);
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal(0.0);
            uint8_t mask = OSARuntime::readRequiredPermsFromFile(s_appsPaths[i]);
            return OSAVal((mask & bit) ? 1.0 : 0.0);
        }
        if (name == "apps.hasPerm") {
            if (!needException("apps.hasPerm")) return OSAVal(0.0);
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal(0.0);
            int stored = Config::getInt(OSARuntime::permKeyForPath(s_appsPaths[i]), 0);
            return OSAVal((stored & 0x0F & bit) ? 1.0 : 0.0);
        }
        if (name == "apps.togglePerm") {
            if (!needException("apps.togglePerm")) return OSAVal();
            int i = iN(0);
            int bit = iN(1);
            if (i < 0 || i >= s_appsCount) return OSAVal();
            String key   = OSARuntime::permKeyForPath(s_appsPaths[i]);
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
    // osa.compile(srcPath, dstPath) — returns diagnostic codes so a UI can
    // tell *why* it failed: 1 ok, -1 perm denied, -2 alloc, -3 load fail,
    // -4 unsupported feature, -5 write fail, -6 bytecode full,
    // -7 number pool full, -8 string pool full, -9 identifier pool full,
    // -10 invalid jump patch.
    if (name == "osa.compile") {
        if (!needException("osa.compile")) return OSAVal(-1.0);
        String src = S(0), dst = S(1);
        OSARuntime* tmp = new OSARuntime(tft, ts);
        if (!tmp) return OSAVal(-2.0);
        if (!tmp->loadScript(src)) {
            uint8_t buildError = tmp->bytecodeError();
            delete tmp;
            if (buildError == OSA_BCERR_CODE_FULL) return OSAVal(-6.0);
            if (buildError == OSA_BCERR_NUM_POOL_FULL) return OSAVal(-7.0);
            if (buildError == OSA_BCERR_STR_POOL_FULL) return OSAVal(-8.0);
            if (buildError == OSA_BCERR_NAME_POOL_FULL) return OSAVal(-9.0);
            if (buildError == OSA_BCERR_BAD_PATCH) return OSAVal(-10.0);
            return OSAVal(-3.0);
        }
        if (!tmp->bc.valid)        { delete tmp; return OSAVal(-4.0); }
        bool wrote = tmp->serializeOsac(dst);
        delete tmp;
        return OSAVal(wrote ? 1.0 : -5.0);
    }

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

    // ═════════════════════════════════════════════════════════════════════════
    // Extended SDK — drawing extras, gestures, animation, home access
    // ═════════════════════════════════════════════════════════════════════════

    // ── Drawing extras ───────────────────────────────────────────────────────
    if (name == "triangle") {
        CV(fillTriangle(iN(0), iN(1), iN(2), iN(3), iN(4), iN(5), drawColor));
        return OSAVal();
    }
    if (name == "tframe") {
        CV(drawTriangle(iN(0), iN(1), iN(2), iN(3), iN(4), iN(5), drawColor));
        return OSAVal();
    }
    if (name == "rframe") {
        CV(drawRoundRect(iN(0), iN(1), iN(2), iN(3), iN(4), drawColor));
        return OSAVal();
    }
    if (name == "gradient") {
        // Vertical gradient from (r1,g1,b1) at top to (r2,g2,b2) at bottom.
        int x = iN(0), y = iN(1), w = iN(2), h = iN(3);
        int r1 = iN(4), g1 = iN(5), b1 = iN(6);
        int r2 = iN(7), g2 = iN(8), b2 = iN(9);
        if (h <= 0 || w <= 0) return OSAVal();
        for (int row = 0; row < h; row++) {
            float t = (float)row / (float)(h - 1 > 0 ? h - 1 : 1);
            int r = r1 + (int)((r2 - r1) * t);
            int g = g1 + (int)((g2 - g1) * t);
            int b = b1 + (int)((b2 - b1) * t);
            uint16_t c = tft->color565(r, g, b);
            if (activeSprite) activeSprite->drawFastHLine(x, y + row, w, c);
            else              tft->drawFastHLine(x, y + row, w, c);
        }
        return OSAVal();
    }

    // ── Text metrics / alignment ─────────────────────────────────────────────
    if (name == "textw") {
        tft->setTextFont(textFont); tft->setTextSize(1);
        return OSAVal((double)tft->textWidth(S(0)));
    }
    if (name == "texth") {
        tft->setTextFont(textFont); tft->setTextSize(1);
        return OSAVal((double)tft->fontHeight());
    }
    if (name == "textr") {
        CV(setTextFont(textFont)); CV(setTextSize(1));
        CV(setTextColor(txtColor)); CV(setTextDatum(TR_DATUM));
        CV(drawString(S(2), iN(0), iN(1)));
        return OSAVal();
    }
    if (name == "textmr") {
        CV(setTextFont(textFont)); CV(setTextSize(1));
        CV(setTextColor(txtColor)); CV(setTextDatum(MR_DATUM));
        CV(drawString(S(2), iN(0), iN(1)));
        return OSAVal();
    }
    if (name == "textml") {
        CV(setTextFont(textFont)); CV(setTextSize(1));
        CV(setTextColor(txtColor)); CV(setTextDatum(ML_DATUM));
        CV(drawString(S(2), iN(0), iN(1)));
        return OSAVal();
    }

    // ── Wallpaper (shared cache from main.cpp) ───────────────────────────────
    if (name == "wallpaper.draw") {
        if (!activeSprite) Wallpaper::draw(tft);
        return OSAVal();
    }
    if (name == "wallpaper.region") {
        if (!activeSprite)
            Wallpaper::drawRegion(tft, iN(0), iN(1), iN(2), iN(3));
        return OSAVal();
    }

    // ── Gesture / touch state ────────────────────────────────────────────────
    if (name == "touch.startX")  { pollGesture(); return OSAVal((double)gestureStartX); }
    if (name == "touch.startY")  { pollGesture(); return OSAVal((double)gestureStartY); }
    if (name == "touch.dx")      { pollGesture();
        if (!gestureActive && !touchWasDown) return OSAVal(0.0);
        return OSAVal((double)(gestureLastX - gestureStartX)); }
    if (name == "touch.dy")      { pollGesture();
        if (!gestureActive && !touchWasDown) return OSAVal(0.0);
        return OSAVal((double)(gestureLastY - gestureStartY)); }
    if (name == "touch.duration") { pollGesture();
        if (!gestureActive) return OSAVal(0.0);
        return OSAVal((double)(millis() - gestureStartT)); }
    if (name == "touch.released") {
        pollGesture();
        bool r = releasedOneShot;
        releasedOneShot = false;
        return OSAVal(r ? 1.0 : 0.0);
    }
    if (name == "gesture.swipeUp")    { pollGesture(); bool ok = (swipeOneShot == 1);
                                        if (ok) swipeOneShot = 0; return OSAVal(ok ? 1.0 : 0.0); }
    if (name == "gesture.swipeDown")  { pollGesture(); bool ok = (swipeOneShot == 2);
                                        if (ok) swipeOneShot = 0; return OSAVal(ok ? 1.0 : 0.0); }
    if (name == "gesture.swipeLeft")  { pollGesture(); bool ok = (swipeOneShot == 3);
                                        if (ok) swipeOneShot = 0; return OSAVal(ok ? 1.0 : 0.0); }
    if (name == "gesture.swipeRight") { pollGesture(); bool ok = (swipeOneShot == 4);
                                        if (ok) swipeOneShot = 0; return OSAVal(ok ? 1.0 : 0.0); }

    // ── Animation helpers ────────────────────────────────────────────────────
    if (name == "lerp") {
        double a = N(0), b = N(1), t = N(2);
        return OSAVal(a + (b - a) * t);
    }
    if (name == "clamp") {
        double v = N(0), lo = N(1), hi = N(2);
        if (v < lo) v = lo; if (v > hi) v = hi;
        return OSAVal(v);
    }
    if (name == "ease") {
        // type: 0=linear, 1=ease-in (quad), 2=ease-out (quad),
        //       3=ease-in-out (cubic), 4=cubic-in, 5=cubic-out
        double t = N(0); int type = iN(1);
        if (t < 0) t = 0; if (t > 1) t = 1;
        double out = t;
        switch (type) {
            case 1: out = t * t; break;
            case 2: out = 1.0 - (1.0 - t) * (1.0 - t); break;
            case 3: out = (t < 0.5) ? 4 * t * t * t
                                    : 1.0 - pow(-2.0 * t + 2.0, 3) / 2.0; break;
            case 4: out = t * t * t; break;
            case 5: { double u = 1.0 - t; out = 1.0 - u * u * u; break; }
            default: out = t;
        }
        return OSAVal(out);
    }

    // ── Time formatting helpers ──────────────────────────────────────────────
    if (name == "time.fmtHM" || name == "time.fmtHMS" || name == "time.fmtDate") {
        time_t now; time(&now); struct tm t; localtime_r(&now, &t);
        char buf[24];
        if (name == "time.fmtHM")  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        if (name == "time.fmtHMS") snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        if (name == "time.fmtDate")snprintf(buf, sizeof(buf), "%02d.%02d.%d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        return OSAVal(String(buf));
    }

    // ── Battery / system info ────────────────────────────────────────────────
    if (name == "battery") return OSAVal(97.0);   // mock; no fuel gauge on CYD
    if (name == "wifi.rssi") {
        if (WiFi.status() != WL_CONNECTED) return OSAVal(0.0);
        return OSAVal((double)WiFi.RSSI());
    }

    // ── Home enumeration (read-only) ─────────────────────────────────────────
    if (name == "home.appCount") return OSAVal((double)home.appCount);
    if (name == "home.appName") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount) return OSAVal("");
        return OSAVal(home.tiles[i].name);
    }
    if (name == "home.appColor") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount) return OSAVal(0.0);
        return OSAVal((double)home.tiles[i].color);
    }
    if (name == "home.appIsFolder") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount) return OSAVal(0.0);
        return OSAVal(home.tiles[i].isFolder ? 1.0 : 0.0);
    }
    if (name == "home.appPath") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount) return OSAVal("");
        return OSAVal(home.tiles[i].scriptPath);
    }
    if (name == "home.canUninstall") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount || home.tiles[i].isFolder)
            return OSAVal(0.0);
        String packageId;
        const String& path = home.tiles[i].scriptPath;
        bool allowed = userPackageFromEntry(path, packageId) ||
                       isLooseUserScript(path);
        return OSAVal(allowed ? 1.0 : 0.0);
    }
    if (name == "home.folderCount") {
        int i = iN(0);
        if (i < 0 || i >= home.appCount || !home.tiles[i].isFolder) return OSAVal(0.0);
        return OSAVal((double)home.tiles[i].childCount);
    }
    if (name == "home.folderAppName" || name == "home.folderAppColor" ||
        name == "home.folderAppPath") {
        int i = iN(0), j = iN(1);
        bool wantsNum = (name == "home.folderAppColor");
        if (i < 0 || i >= home.appCount || !home.tiles[i].isFolder)
            return wantsNum ? OSAVal(0.0) : OSAVal("");
        const HomeTile& f = home.tiles[i];
        if (j < 0 || j >= f.childCount)
            return wantsNum ? OSAVal(0.0) : OSAVal("");
        if (name == "home.folderAppName")  return OSAVal(f.children[j].name);
        if (name == "home.folderAppColor") return OSAVal((double)f.children[j].color);
        return OSAVal(f.children[j].scriptPath);
    }

    // ── Home mutations (privileged-ish — gate behind isException) ────────────
    if (name == "home.swap") {
        if (!isException) return OSAVal(0.0);
        int i = iN(0), j = iN(1);
        if (i < 0 || j < 0 || i >= home.appCount || j >= home.appCount) return OSAVal(0.0);
        HomeTile tmp = static_cast<HomeTile&&>(home.tiles[i]);
        home.tiles[i] = static_cast<HomeTile&&>(home.tiles[j]);
        home.tiles[j] = static_cast<HomeTile&&>(tmp);
        return OSAVal(1.0);
    }
    if (name == "home.makeFolder") {
        if (!isException) return OSAVal(0.0);
        return OSAVal(osaMakeFolder(iN(0)) ? 1.0 : 0.0);
    }
    if (name == "home.deleteFolder") {
        if (!isException) return OSAVal(0.0);
        return OSAVal(osaDeleteFolder(iN(0)) ? 1.0 : 0.0);
    }
    if (name == "home.uninstall") {
        if (!isException) return OSAVal(0.0);
        int i = iN(0);
        if (i < 0 || i >= home.appCount || home.tiles[i].isFolder)
            return OSAVal(0.0);

        String appTitle = home.tiles[i].name;
        String path = home.tiles[i].scriptPath;
        String packageId, packagePrefix;
        bool packaged = userPackageFromEntry(path, packageId, &packagePrefix);
        if (!packaged && !isLooseUserScript(path)) {
            showSystemPopup("Protected app", appTitle,
                            "System apps cannot be uninstalled", "", "OK", false);
            return OSAVal(0.0);
        }
        if (!showSystemPopup("Uninstall app?", appTitle, "App data is kept",
                             "Cancel", "Uninstall", true))
            return OSAVal(0.0);

        bool removed = false;
        String why;
        if (packaged) {
            removed = PackageManager::removeUserPackage(packageId);
            if (!removed) why = PackageManager::lastError();
        } else if (!isSdReady) {
            why = "SD card is not available";
        } else if (!SD.exists(path.c_str())) {
            // A stale shortcut is already effectively uninstalled.
            removed = true;
        } else {
            removed = SD.remove(path.c_str());
            if (!removed) why = "Could not remove app file";
        }

        if (!removed) {
            if (why.length() == 0) why = "Could not remove app";
            showSystemPopup("Uninstall failed", appTitle, why, "", "OK", false);
            return OSAVal(0.0);
        }

        if (packaged) home.removeScriptsUnder(packagePrefix);
        else          home.removeScriptPath(path);
        return OSAVal(1.0);
    }
    if (name == "home.addToFolder") {
        if (!isException) return OSAVal(0.0);
        return OSAVal(osaAddToFolder(iN(0), iN(1)) ? 1.0 : 0.0);
    }
    if (name == "home.saveOrder") {
        if (!isException) return OSAVal();
        home.saveOrder();
        return OSAVal();
    }
    if (name == "anim.openTile") {
        if (!isException) return OSAVal();
        osaPlayOpenAnim(iN(0));
        return OSAVal();
    }

    // ── theme.* — current theme palette as packed RGB565 ─────────────────────
    // Apps use these to stay consistent with system widgets across dark/light.
    if (name == "theme.bg")       return OSAVal((double)Theme::bg());
    if (name == "theme.surface")  return OSAVal((double)Theme::surface());
    if (name == "theme.header")   return OSAVal((double)Theme::header());
    if (name == "theme.divider")  return OSAVal((double)Theme::divider());
    if (name == "theme.divider2") return OSAVal((double)Theme::divider2());
    if (name == "theme.text")     return OSAVal((double)Theme::text());
    if (name == "theme.subtext")  return OSAVal((double)Theme::subtext());
    if (name == "theme.hint")     return OSAVal((double)Theme::hint());

    // Grid geometry — keeps the layout source of truth in C++.
    if (name == "home.iconX") {
        int i = iN(0); int col = i % 4;
        return OSAVal((double)(12 + col * 55));
    }
    if (name == "home.iconY") {
        int i = iN(0); int row = i / 4;
        return OSAVal((double)(30 + row * 80));
    }

    // Unknown built-in (user funcs were handled at the top)
    setError(-1, "Unknown: " + name);
    return OSAVal();
}
