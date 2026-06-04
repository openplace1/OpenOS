#pragma once
#include "App.h"
#include "../Runtime/OSARuntime.h"
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

class OSAApp : public App {
public:
    OSAApp(TFT_eSPI* tft, XPT2046_Touchscreen* ts);

    bool loadScript(const String& path); // call before show()

    void show()       override;
    void update()     override;
    void drawHeader() override;

    // Set to true when the script exited cleanly OR the runtime tripped the
    // global swipe-up-to-home gesture. main.cpp polls this after update() and
    // routes back to the home screen.
    bool wantsExit = false;

    // If the script called app.launch(path), this is the path to load next
    // instead of going home. Empty otherwise.
    String pendingLaunch() const { return runtime.pendingLaunch; }
    void   clearPendingLaunch()  { runtime.pendingLaunch = ""; }

    // Set by the swipe-down-from-top gesture inside any blocking widget.
    // main.cpp opens Control Center over this app instead of going home.
    bool   wantsOverlay() const  { return runtime.wantsOverlay; }
    void   clearWantsOverlay()   { runtime.wantsOverlay = false; }

private:
    OSARuntime runtime;
    bool       scriptLoaded = false;
    bool       showDone     = false;

    void drawErrorScreen();
    void drawHeaderBar();
};
