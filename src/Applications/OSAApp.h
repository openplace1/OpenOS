#pragma once
#include "../Runtime/OSARuntime.h"
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// Thin host around OSARuntime. main.cpp keeps two of these alive — one for
// the active script (home / lockscreen / app), one for overlays (Control
// Center). Not derived from anything — there is no App hierarchy anymore.
class OSAApp {
public:
    OSAApp(TFT_eSPI* tft, XPT2046_Touchscreen* ts);

    bool loadScript(const String& path);

    void show();
    void update();
    void showLoadError();
    String lastError() const { return runtime.getError(); }
    // Repaints just the top bar — used after a notification banner clears.
    void drawHeader();

    // Set when the script exited cleanly OR the runtime tripped the global
    // swipe-up-to-home gesture. main.cpp polls this and routes back home.
    bool wantsExit = false;

    String pendingLaunch() const { return runtime.pendingLaunch; }
    void   clearPendingLaunch()  { runtime.pendingLaunch = ""; }

    bool   wantsOverlay() const  { return runtime.wantsOverlay; }
    void   clearWantsOverlay()   { runtime.wantsOverlay = false; }

    void recycle() {
        scriptLoaded = false;
        showDone     = false;
        wantsExit    = false;
        runtime.reset();
    }

    // Pre-render the setup section onto a sprite on core 0 while an animation
    // plays on core 1. finishPreRender() returns true if it blitted, false if
    // it fell back (OOM or script overrode the sprite).
    void beginPreRender();
    bool finishPreRender();

private:
    TFT_eSPI*            tft;
    XPT2046_Touchscreen* ts;
    OSARuntime           runtime;
    String               name           = "App";
    bool                 scriptLoaded   = false;
    bool                 showDone       = false;

    TFT_eSprite*         preRenderSprite = nullptr;
    volatile bool        preRenderDone   = false;
    static void          preRenderTaskFn(void* arg);

    void drawErrorScreen();
    void drawHeaderBar();
};
