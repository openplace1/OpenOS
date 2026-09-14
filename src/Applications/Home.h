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
    String    icon;         // built-in icon name from #appIcon, may be empty
    uint16_t  color         = 0xFFFF;
    bool      isFolder      = false;
    HomeTile* children      = nullptr;
    int       childCount    = 0;
    int       childCap      = 0;

    HomeTile() = default;
    HomeTile(const HomeTile& other);
    HomeTile& operator=(const HomeTile& other);
    HomeTile(HomeTile&& other) noexcept;
    HomeTile& operator=(HomeTile&& other) noexcept;
    ~HomeTile();

    // Take ownership of `c` into this folder. Returns false if full.
    bool addChild(const HomeTile& c);
    // Frees the children array (called from Home dtor + delete-folder path).
    void freeChildren();
};

// Pure data store for the home grid. /system/apps/home.osa does all rendering
// and gesture handling via home.* runtime builtins.
class Home {
public:
    // Three pages of twelve; home.osa lays them out and pages through them.
    static const int MAX_APPS = 36;

    HomeTile tiles[MAX_APPS];
    int      appCount = 0;

    // Where the last launch came from on screen (tile top-left, 46 px) and
    // its colour: set by anim.openAt / anim.openTile from home.osa, used by
    // main.cpp for the open zoom and, on the way back, the close zoom.
    int      lastLaunchX     = 97;
    int      lastLaunchY     = 137;
    uint16_t lastLaunchColor = TFT_WHITE;
    bool     lastLaunchValid = false;
    // A swipe-up on Home asks the next Home instance to open its drawer.
    bool     drawerRequested = false;

    // Reorder: take the tile at `from` out and insert it at `to`, shifting
    // the ones between. Both indices within [0, appCount).
    bool moveTile(int from, int to);

    Home(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);

    // Build a leaf tile from a discovered .osa script + add to grid.
    void addScript(const String& scriptPath, const String& displayName, uint16_t color,
                   const String& icon = String());

    // applyOrder() restores the user's last arrangement; saveOrder() is
    // called from the OSA-side home.* mutation builtins after every change.
    void applyOrder();
    void saveOrder();

    // Removes every leaf whose script path is exactly `path` or begins with
    // `pathPrefix`. This keeps the live grid and folders in sync after an OPK
    // is uninstalled without requiring a reboot.
    int removeScriptPath(const String& path);
    int removeScriptsUnder(const String& pathPrefix);

private:
    TFT_eSPI*            tft;
    XPT2046_Touchscreen* ts;
};

#endif
