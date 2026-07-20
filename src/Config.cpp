#include "Config.h"
#include <SD.h>

extern bool isSdReady;

static const char* CONFIG_PATH = "/user/config.ini";
static const char* CONFIG_TEMP = "/user/config.tmp";
static const char* CONFIG_BACKUP = "/user/config.bak";
static const int   MAX_KEYS    = 48;

static String _keys[MAX_KEYS];
static String _vals[MAX_KEYS];
static int    _count = 0;

static bool readConfigLine(File& file, String& line, size_t maximum) {
    line = "";
    while (file.available()) {
        int value = file.read();
        if (value < 0 || value == '\n') return true;
        if (value == '\r') continue;
        if (line.length() >= maximum || !line.concat((char)value)) {
            while (file.available()) {
                int rest = file.read();
                if (rest < 0 || rest == '\n') break;
            }
            line = "";
            return false;
        }
    }
    return true;
}

static int findKey(const String& key) {
    for (int i = 0; i < _count; i++)
        if (_keys[i] == key) return i;
    return -1;
}

namespace Config {

void load() {
    _count = 0;
    for (int i = 0; i < MAX_KEYS; ++i) {
        _keys[i] = "";
        _vals[i] = "";
    }
    if (!isSdReady) return;

    // Recover the last known-good file if power was lost between the two
    // renames of an atomic save. A completed config always wins.
    if (!SD.exists(CONFIG_PATH)) {
        if (SD.exists(CONFIG_BACKUP)) SD.rename(CONFIG_BACKUP, CONFIG_PATH);
        else if (SD.exists(CONFIG_TEMP)) SD.rename(CONFIG_TEMP, CONFIG_PATH);
    }

    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) return;

    while (f.available()) {
        String line;
        if (!readConfigLine(f, line, 2048)) continue;
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;
        int eq = line.indexOf('=');
        if (eq < 1) continue;
        if (eq > 48 || line.length() - eq - 1 > 2048) continue;
        if (_count >= MAX_KEYS) break;
        _keys[_count] = line.substring(0, eq);
        _vals[_count] = line.substring(eq + 1);
        _count++;
    }
    f.close();
}

void save() {
    if (!isSdReady) return;
    if (!SD.exists("/user")) SD.mkdir("/user");
    if (SD.exists(CONFIG_TEMP)) SD.remove(CONFIG_TEMP);
    File f = SD.open(CONFIG_TEMP, FILE_WRITE);
    if (!f) return;
    bool valid = true;
    for (int i = 0; i < _count; i++) {
        if (f.print(_keys[i]) == 0 || f.print('=') == 0 ||
            f.println(_vals[i]) == 0) valid = false;
    }
    f.flush();
    if (f.getWriteError()) valid = false;
    f.close();
    if (!valid) {
        if (SD.exists(CONFIG_TEMP)) SD.remove(CONFIG_TEMP);
        return;
    }

    if (SD.exists(CONFIG_BACKUP)) SD.remove(CONFIG_BACKUP);
    bool hadConfig = SD.exists(CONFIG_PATH);
    if (hadConfig && !SD.rename(CONFIG_PATH, CONFIG_BACKUP)) {
        SD.remove(CONFIG_TEMP);
        return;
    }
    if (!SD.rename(CONFIG_TEMP, CONFIG_PATH)) {
        if (hadConfig && SD.exists(CONFIG_BACKUP))
            SD.rename(CONFIG_BACKUP, CONFIG_PATH);
        return;
    }
    if (SD.exists(CONFIG_BACKUP)) SD.remove(CONFIG_BACKUP);
}

String get(const String& key, const String& def) {
    int i = findKey(key);
    return (i >= 0) ? _vals[i] : def;
}

void set(const String& key, const String& value) {
    if (key.length() < 1 || key.length() > 48 || value.length() > 2048 ||
        key.indexOf('=') >= 0 || key.indexOf('\n') >= 0 || key.indexOf('\r') >= 0 ||
        value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0) return;
    int i = findKey(key);
    if (i >= 0) {
        _vals[i] = value;
    } else if (_count < MAX_KEYS) {
        _keys[_count] = key;
        _vals[_count] = value;
        _count++;
    }
}

int getInt(const String& key, int def) {
    int i = findKey(key);
    return (i >= 0) ? _vals[i].toInt() : def;
}

void setInt(const String& key, int value) {
    set(key, String(value));
}

} // namespace Config
