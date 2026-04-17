#ifndef LOCKSCREEN_H
#define LOCKSCREEN_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "OSKeyboard.h"

class Lockscreen {
private:
    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;
    OSKeyboard* kbd;

    bool isSwiping;
    bool isUnlocked;
    bool enteringPassword;
    int startTouchY;

    String systemPassword;
    String enteredPassword;
    bool showTypedPassword;
    bool wasTouched;
    bool numericMode;
    String statusMessage;
    unsigned long statusUntil;

    void drawStatusBar();
    void drawClock();
    void drawSwipeHint();
    void drawPasswordScreen();
    void drawNumericPad();
    void drawNumericButton(int x, int y, int r, const String& label, const String& sublabel = "");
    void drawPasscodeDots();
    void drawKeyboardPasswordScreen();
    void redrawPasswordInput();
    void getMappedTouch(int &x, int &y);
    void loadPassword();
    bool isNumericPassword(const String& pwd);
    void submitPassword();
    bool handleNumericTouch(int touchX, int touchY);
    void showWrongPassword();

public:
    Lockscreen(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    ~Lockscreen();

    void show();
    bool update();
    void lock();
};

#endif
