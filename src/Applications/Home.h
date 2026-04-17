#ifndef HOME_H
#define HOME_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "App.h"

class Home {
private:
    static const int MAX_APPS = 16;

    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;

    App* apps[MAX_APPS];
    int appCount;

    bool wasTouched = false;
    bool isDragging = false;
    int draggedIndex = -1;
    int dragX = 0;
    int dragY = 0;
    unsigned long touchStartTime = 0;

    void drawStatusBar();
    void drawApps(bool renderingDrag);
    int getAppIndexAt(int x, int y);

public:
    Home(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);

    void addApp(App* newApp);
    void show(bool renderingDrag = false);
    App* update();
};

#endif
