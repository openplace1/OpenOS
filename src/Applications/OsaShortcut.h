#ifndef OSA_SHORTCUT_H
#define OSA_SHORTCUT_H

#include "../App.h"

// Home-screen tile that launches a .osa script via the shared OSAApp runtime.
// show()/update() are intentionally empty: main.cpp inspects scriptPath after
// Home::update() returns and reroutes the tap to OSAApp::loadScript.
class OsaShortcut : public App {
public:
    OsaShortcut(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance,
                const String& path, const String& displayName, uint16_t color);
    void show()   override {}
    void update() override {}
};

#endif
