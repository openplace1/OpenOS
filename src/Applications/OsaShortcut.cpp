#include "OsaShortcut.h"

OsaShortcut::OsaShortcut(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance,
                         const String& path, const String& displayName, uint16_t color) {
    tft        = tftInstance;
    ts         = tsInstance;
    scriptPath = path;
    name       = displayName;
    iconColor  = color;
    isApp      = true;
}
