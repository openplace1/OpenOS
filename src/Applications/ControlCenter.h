#ifndef CONTROLCENTER_H
#define CONTROLCENTER_H

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

    void drawToggle(int x, int y, int w, int h, String label, bool state, uint16_t color);
    void drawSlider();
    void setBrightness(int val); 

public:
    ControlCenter(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    
    void show();
    bool update(); 
};

#endif