#ifndef HOME_H
#define HOME_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// Pure data tile. No virtual hierarchy — folders are just tiles with
// isFolder=true plus a dynamically-allocated children array (each child is
// itself a HomeTile copy, never a shared pointer).
struct HomeTile {
    String    name;
    String    scriptPath;   // empty for folders
    uint16_t  color         = 0xFFFF;
    bool      isFolder      = false;
    HomeTile* children      = nullptr;
    int       childCount    = 0;
    int       childCap      = 0;

    // Take ownership of `c` into this folder. Returns false if full.
    bool addChild(const HomeTile& c);
    // Frees the children array (called from Home dtor + delete-folder path).
    void freeChildren();
};

// Pure data store for the home grid. /system/apps/home.osa does all rendering
// and gesture handling via home.* runtime builtins.
class Home {
public:
    static const int MAX_APPS = 16;

    HomeTile tiles[MAX_APPS];
    int      appCount = 0;

    // Set by anim.openTile; read by main.cpp on close transitions.
    int      lastLaunchX     = 120;
    int      lastLaunchY     = 160;
    uint16_t lastLaunchColor = TFT_WHITE;

    Home(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);

    // Build a leaf tile from a discovered .osa script + add to grid.
    void addScript(const String& scriptPath, const String& displayName, uint16_t color);

    // applyOrder() restores the user's last arrangement; saveOrder() is
    // called from the OSA-side home.* mutation builtins after every change.
    void applyOrder();
    void saveOrder();

private:
    TFT_eSPI*            tft;
    XPT2046_Touchscreen* ts;
};

#endif
