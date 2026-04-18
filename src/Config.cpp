#include "Config.h"
#include <SD.h>

extern bool isSdReady;

static const char* CONFIG_PATH = "/user/config.ini";
static const int   MAX_KEYS    = 48;

static String _keys[MAX_KEYS];
static String _vals[MAX_KEYS];
static int    _count = 0;

static int findKey(const String& key) {
    for (int i = 0; i < _count; i++)
        if (_keys[i] == key) return i;
    return -1;
}

namespace Config {

void load() {
    _count = 0;
    if (!isSdReady) return;

    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) return;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;
        int eq = line.indexOf('=');
        if (eq < 1) continue;
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
    SD.remove(CONFIG_PATH);
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < _count; i++) {
        f.print(_keys[i]);
        f.print('=');
        f.println(_vals[i]);
    }
    f.close();
}

String get(const String& key, const String& def) {
    int i = findKey(key);
    return (i >= 0) ? _vals[i] : def;
}

void set(const String& key, const String& value) {
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
