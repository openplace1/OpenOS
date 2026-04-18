#ifndef CLOCK_H
#define CLOCK_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "../App.h"

class ClockApp : public App {
private:
    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;

    bool wasTouched;
    int  lastDrawnMin;
    int  lastDrawnSec;

    void drawFace();
    void drawTime(bool fullRedraw);
    void drawDate();
    void drawNTPSection();
    void triggerSync();

public:
    ClockApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    void show() override;
    void update() override;
    void drawHeader() override;
};

#endif
