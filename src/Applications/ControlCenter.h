#ifndef CONTROLCENTER_H
#define CONTROLCENTER_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

class ControlCenter {
private:
    TFT_eSPI* tft;
    XPT2046_Touchscreen* ts;

    int sliderX, sliderY, sliderW, sliderH;
    int thumbX;
    bool isDragging;
    bool wasTouched;
    bool airplaneMode;

    uint16_t bg()   { return tft->color565(28,  28,  30);  }
    uint16_t card() { return tft->color565(44,  44,  46);  }
    uint16_t blue() { return tft->color565(0,   122, 255); }
    uint16_t grn()  { return tft->color565(52,  199, 89);  }
    uint16_t org()  { return tft->color565(255, 149, 0);   }
    uint16_t pur()  { return tft->color565(175, 82,  222); }
    uint16_t dim()  { return tft->color565(120, 120, 120); }

    void drawBigTile(int x, int y, int w, int h, bool active,
                     uint16_t activeColor, const char* label,
                     const char* status, int iconType);
    void drawSmallTile(int x, int y, int w, int h, bool active,
                       uint16_t activeColor, const char* label, int iconType);
    void drawBrightnessCard();
    void drawSlider();

    void iconWiFi(int cx, int cy, uint16_t color, uint16_t bgc);
    void iconBT(int cx, int cy, uint16_t color, uint16_t bgc);
    void iconAirplane(int cx, int cy, uint16_t color);
    void iconClock(int cx, int cy, uint16_t color, uint16_t bgc);
    void iconMoon(int cx, int cy, uint16_t color, uint16_t bgc);
    void iconSun(int cx, int cy, uint16_t color);

    void setBrightness(int val);
    void triggerNTPSync();
    void applyThemeToggle();

public:
    ControlCenter(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    void show();
    void drawHeader(); // restore top area after notification dismiss
    bool update();
};

#endif
