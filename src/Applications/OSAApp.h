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

private:
    OSARuntime runtime;
    bool       scriptLoaded = false;
    bool       showDone     = false;

    void drawErrorScreen();
    void drawHeaderBar();
};
