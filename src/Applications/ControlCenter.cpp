#include "ControlCenter.h"
#include <WiFi.h>
#include "BluetoothSerial.h"


extern bool sysWiFiEnabled;
extern bool sysBTEnabled;
extern int sysBrightness;
extern BluetoothSerial SerialBT;

#define TFT_BL 21 

ControlCenter::ControlCenter(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    
    sliderW = 180; sliderH = 8; 
    sliderX = 30; sliderY = 220;
    
    thumbX = sliderX + map(sysBrightness, 10, 255, 0, sliderW);
    
    isDragging = false;
    wasTouched = false;
}

void ControlCenter::setBrightness(int val) {
    if (val < 10) val = 10;
    if (val > 255) val = 255;
    sysBrightness = val; 
    analogWrite(TFT_BL, val); 
}

void ControlCenter::show() {
    
    thumbX = sliderX + map(sysBrightness, 10, 255, 0, sliderW);

    tft->fillRect(0, 0, 240, 320, tft->color565(30, 30, 35));
    
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("Control Center", 120, 20);

    
    drawToggle(20, 60, 95, 95, "Wi-Fi", sysWiFiEnabled, tft->color565(0, 122, 255));
    drawToggle(125, 60, 95, 95, "Bluetooth", sysBTEnabled, tft->color565(0, 122, 255));

    
    tft->fillRoundRect(20, 170, 200, 90, 15, tft->color565(50, 50, 55));
    tft->setTextDatum(BC_DATUM);
    tft->setTextColor(tft->color565(180, 180, 180));
    tft->drawString("Brightness", 120, 200);
    drawSlider();
    
    
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(tft->color565(100, 100, 100));
    tft->drawString("Swipe UP to close", 120, 290);
}

void ControlCenter::drawToggle(int x, int y, int w, int h, String label, bool state, uint16_t color) {
    uint16_t bgColor = state ? color : tft->color565(70, 70, 75);
    tft->fillRoundRect(x, y, w, h, 15, bgColor);
    
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(label, x + (w / 2), y + (h / 2));
}

void ControlCenter::drawSlider() {
    tft->fillRoundRect(sliderX, sliderY - (sliderH / 2), sliderW, sliderH, 4, tft->color565(80, 80, 85)); 
    int fillW = thumbX - sliderX; 
    if (fillW > 0) { 
        tft->fillRoundRect(sliderX, sliderY - (sliderH / 2), fillW, sliderH, 4, TFT_WHITE); 
    } 
    tft->fillCircle(thumbX, sliderY, 14, TFT_WHITE); 
}

bool ControlCenter::update() {
    if (ts->touched()) {
        TS_Point p = ts->getPoint();
        int touchX = map(p.x, 300, 3800, 0, 240);
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;
            
            
            if (touchX > 20 && touchX < 115 && touchY > 60 && touchY < 155) {
                sysWiFiEnabled = !sysWiFiEnabled;
                if (sysWiFiEnabled) WiFi.mode(WIFI_STA); else WiFi.mode(WIFI_OFF);
                drawToggle(20, 60, 95, 95, "Wi-Fi", sysWiFiEnabled, tft->color565(0, 122, 255));
                delay(150); 
            }
            
            else if (touchX > 125 && touchX < 220 && touchY > 60 && touchY < 155) {
                sysBTEnabled = !sysBTEnabled;
                if (sysBTEnabled) SerialBT.begin("OpenOS"); else SerialBT.end();
                drawToggle(125, 60, 95, 95, "Bluetooth", sysBTEnabled, tft->color565(0, 122, 255));
                delay(150); 
            }
            
            else if (touchY > sliderY - 25 && touchY < sliderY + 25) {
                isDragging = true;
            }
            
            else if (touchY > 250) {
                wasTouched = false;
                return true; 
            }
        }

        if (isDragging) {
            int newThumbX = touchX;
            if (newThumbX < sliderX) newThumbX = sliderX;
            if (newThumbX > sliderX + sliderW) newThumbX = sliderX + sliderW;
            
            if (newThumbX != thumbX) {
                
                tft->fillRect(sliderX - 15, sliderY - 15, sliderW + 30, 30, tft->color565(50, 50, 55));
                thumbX = newThumbX; 
                drawSlider(); 
                
                int newBrightness = map(thumbX, sliderX, sliderX + sliderW, 10, 255); 
                setBrightness(newBrightness);
            }
        }
    } else {
        wasTouched = false;
        isDragging = false;
    }
    return false;
}