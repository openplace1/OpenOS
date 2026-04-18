#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Simple key=value config stored at /user/config.ini on SD card.
// Call Config::load() once after SD mount, then get/set freely,
// and Config::save() to persist.
namespace Config {
    void load();
    void save();

    String get(const String& key, const String& def = "");
    void   set(const String& key, const String& value);

    int  getInt(const String& key, int def = 0);
    void setInt(const String& key, int value);
}

#endif
