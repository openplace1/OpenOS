#include "OSKeyboard.h"
#include "Theme.h"


const char keysL[3][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l',' '},
    {'z','x','c','v','b','n','m',' ',' ',' '}
};
const char keysU[3][10] = {
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L',' '},
    {'Z','X','C','V','B','N','M',' ',' ',' '}
};
const char keysN[3][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'-','/',':',';','(',')','$','&','@',' '},
    {'.',',','?','!','\'','"','=',' ',' ',' '}
};

OSKeyboard::OSKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    layoutState = 0; 
}

char OSKeyboard::getCharFromLayout(int row, int col) {
    if (layoutState == 0) return keysL[row][col];
    if (layoutState == 1) return keysU[row][col];
    return keysN[row][col];
}

void OSKeyboard::drawKey(int x, int y, int w, int h, String label, bool isSpecial) {
    uint16_t bgColor = isSpecial ? Theme::kbdSpec() : Theme::kbdKey();

    tft->fillRoundRect(x, y+2, w, h, 4, Theme::kbdShadow());
    tft->fillRoundRect(x, y, w, h, 4, bgColor);

    tft->setTextColor(Theme::kbdText());
    tft->setTextDatum(MC_DATUM); 
    
    
    tft->setTextFont(2);
    tft->setTextSize(1); 
    
    tft->drawString(label, x + (w / 2), y + (h / 2) + 2);
}

void OSKeyboard::draw() {
    
    tft->fillRect(0, 160, 240, 160, Theme::kbdBg());

    
    for(int i=0; i<10; i++) {
        drawKey(2 + i*24, 164, 20, 34, String(getCharFromLayout(0, i)), false);
    }
    
    for(int i=0; i<9; i++) {
        drawKey(14 + i*24, 202, 20, 34, String(getCharFromLayout(1, i)), false);
    }
    
    drawKey(2, 240, 32, 34, layoutState == 1 ? "ABC" : "abc", true);
    for(int i=0; i<7; i++) {
        drawKey(38 + i*24, 240, 20, 34, String(getCharFromLayout(2, i)), false);
    }
    drawKey(206, 240, 32, 34, "<X", true);

    
    drawKey(2, 278, 54, 34, layoutState == 2 ? "ABC" : "123", true);
    drawKey(60, 278, 120, 34, "space", false);
    drawKey(184, 278, 54, 34, "Enter", true);
}

char OSKeyboard::update() {
    if (!ts->touched()) return '\0';

    TS_Point p = ts->getPoint();
    int x = map(p.x, 300, 3800, 0, 240);
    int y = map(p.y, 300, 3800, 0, 320);

    
    if (y < 160) return '\0';

    char result = '\0';
    bool redrawNeeded = false;

    
    if (y >= 160 && y < 200) {
        
        int col = x / 24;
        if (col >= 0 && col < 10) result = getCharFromLayout(0, col);
    } 
    else if (y >= 200 && y < 240) {
        
        if (x > 12 && x < 228) {
            int col = (x - 12) / 24;
            if (col >= 0 && col < 9) result = getCharFromLayout(1, col);
        }
    } 
    else if (y >= 240 && y < 280) {
        
        if (x < 36) {
            
            layoutState = (layoutState == 1) ? 0 : 1; 
            redrawNeeded = true;
        } else if (x > 204) {
            
            result = '\b';
        } else {
            int col = (x - 36) / 24;
            if (col >= 0 && col < 7) result = getCharFromLayout(2, col);
        }
    } 
    else if (y >= 280 && y < 320) {
        
        if (x < 60) {
            
            layoutState = (layoutState == 2) ? 0 : 2;
            redrawNeeded = true;
        } else if (x > 180) {
            
            result = '\n';
        } else {
            
            result = ' ';
        }
    }

    if (redrawNeeded) {
        draw();
        delay(200); 
    } else if (result != '\0') {
        
        if (layoutState == 1) {
            layoutState = 0;
            draw();
        }
        delay(200); 
    }

    return result;
}