#ifndef OSKEYBOARD_H
#define OSKEYBOARD_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

class OSKeyboard {
private:
    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;
    int layoutState;

    char getCharFromLayout(int row, int col);
    void drawKey(int x, int y, int w, int h, String label, bool isSpecial);

public:
    OSKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);

    void draw();
    char update();
};

#endif
