#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

class App {
public:
    TFT_eSPI* tft = nullptr;
    XPT2046_Touchscreen* ts = nullptr;
    String name = "";
    uint16_t iconColor = TFT_WHITE;
    bool isApp = false;

    virtual ~App() = default;

    virtual void show() = 0;
    virtual void update() = 0;

    String getDisplayName() const {
        return name;
    }
};

#endif
