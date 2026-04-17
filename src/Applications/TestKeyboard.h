#ifndef TESTKEYBOARD_H
#define TESTKEYBOARD_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "App.h"
#include "OSKeyboard.h"

class TestKeyboard : public App {
private:
    OSKeyboard* kbd;
    String textBuffer;

    void drawTextArea();

public:
    TestKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    ~TestKeyboard() override;

    void show() override;
    void update() override;
};

#endif
