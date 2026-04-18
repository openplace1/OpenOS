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
    // Redraws only the header bar — called after a notification banner dismisses.
    // Default falls back to full show(); override for flicker-free partial redraw.
    virtual void drawHeader() { show(); }

    String getDisplayName() const {
        return name;
    }
};

#endif
