#include "Home.h"
#include "Wallpaper.h"
#include <time.h>

extern int sysTheme;

Home::Home(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    appCount = 0;
}

void Home::addApp(App* newApp) {
    if (appCount < MAX_APPS && newApp->isApp) {
        apps[appCount] = newApp;
        appCount++;
    }
}

void Home::show(bool renderingDrag) {
    Wallpaper::draw(tft);
    drawStatusBar();
    drawApps(renderingDrag); 
}

void Home::drawApps(bool renderingDrag) {
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(MC_DATUM); 

    for(int i = 0; i < appCount; i++) {
        if(!apps[i]->isApp) continue;
        if(renderingDrag && i == draggedIndex) continue; 

        int col = i % 4;
        int row = i / 4;
        
        int iconX = 12 + (col * 55); 
        int iconY = 30 + (row * 80); 

        
        tft->fillRoundRect(iconX + 2, iconY + 3, 46, 46, 12, tft->color565(20, 20, 20));
        tft->fillRoundRect(iconX, iconY, 46, 46, 12, apps[i]->iconColor);

        String dispName = apps[i]->getDisplayName();
        
        tft->setTextColor(TFT_BLACK);
        tft->drawString(dispName, iconX + 23 + 1, iconY + 60 + 1);
        tft->setTextColor(TFT_WHITE);
        tft->drawString(dispName, iconX + 23, iconY + 60);
    }

    if (renderingDrag && draggedIndex != -1) {
        int iconX = dragX - 23;
        int iconY = dragY - 23;

        
        tft->fillRoundRect(iconX + 6, iconY + 8, 46, 46, 12, tft->color565(10, 10, 10));
        tft->fillRoundRect(iconX, iconY, 46, 46, 12, apps[draggedIndex]->iconColor);

        String dispName = apps[draggedIndex]->getDisplayName();
        tft->setTextColor(TFT_BLACK);
        tft->drawString(dispName, iconX + 23 + 1, iconY + 60 + 1);
        tft->setTextColor(TFT_WHITE);
        tft->drawString(dispName, iconX + 23, iconY + 60);
    }
}

void Home::drawStatusBar() {
    bool dark = (sysTheme == 1);
    uint16_t barBg   = dark ? tft->color565(20, 20, 22)  : tft->color565(245, 245, 247);
    uint16_t barText = dark ? TFT_WHITE                   : TFT_BLACK;
    uint16_t barIcon = dark ? TFT_WHITE                   : TFT_BLACK;

    tft->fillRect(0, 0, 240, 18, barBg);

    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(barText);

    tft->setTextDatum(TL_DATUM);
    tft->drawString("CYD", 5, 2);

    tft->setTextDatum(TC_DATUM);
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    tft->drawString(timeStr, 120, 2);

    tft->setTextDatum(TR_DATUM);
    tft->drawString("97%", 210, 2);
    tft->drawRect(215, 4, 16, 10, barIcon);
    tft->fillRect(217, 6, 12, 6, barIcon);
    tft->fillRect(231, 7, 2, 4, barIcon);
}

int Home::getAppIndexAt(int x, int y) {
    for(int i = 0; i < appCount; i++) {
        if(!apps[i]->isApp) continue;
        int col = i % 4;
        int row = i / 4;
        int iconX = 12 + (col * 55); 
        int iconY = 30 + (row * 80);

        if (x >= iconX && x <= iconX + 46 && y >= iconY && y <= iconY + 46) {
            return i;
        }
    }
    return -1; 
}

App* Home::update() {
    
    static int lastMinute = -1;
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (lastMinute == -1) {
        lastMinute = timeinfo.tm_min;
    } else if (lastMinute != timeinfo.tm_min) {
        lastMinute = timeinfo.tm_min;
        drawStatusBar();
    }
    

    if (ts->touched()) {
        TS_Point p = ts->getPoint(); 
        int touchX = map(p.x, 300, 3800, 0, 240);
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;
            touchStartTime = millis();
            draggedIndex = getAppIndexAt(touchX, touchY);
            isDragging = false;
        } else {
            if (draggedIndex != -1) {
                
                if (!isDragging && (millis() - touchStartTime > 400)) {
                    isDragging = true;
                    dragPositionSet = false;
                }

                if (isDragging) {
                    if (abs(dragX - touchX) > 3 || abs(dragY - touchY) > 3) {
                        int eraseX, eraseY;
                        if (!dragPositionSet) {
                            int col = draggedIndex % 4;
                            int row = draggedIndex / 4;
                            eraseX = 12 + col * 55 + 23;
                            eraseY = 30 + row * 80 + 23;
                        } else {
                            eraseX = dragX;
                            eraseY = dragY;
                        }
                        dragX = touchX;
                        dragY = touchY;
                        dragPositionSet = true;

                        if (Wallpaper::drawRegion(tft, eraseX - 28, eraseY - 28, 62, 76)) {
                            drawStatusBar();
                            drawApps(true);
                        } else {
                            show(true);
                        }
                    }
                }
            }
        }
    } else {
        if (wasTouched) {
            wasTouched = false;
            
            if (isDragging) {
                isDragging = false;
                dragPositionSet = false;
                int dropIndex = getAppIndexAt(dragX, dragY);
                
                
                if (dropIndex != -1 && dropIndex != draggedIndex) {
                    App* temp = apps[draggedIndex];
                    apps[draggedIndex] = apps[dropIndex];
                    apps[dropIndex] = temp;
                }
                show(false); 
            } else {
                if (draggedIndex != -1) {
                    
                    int col = draggedIndex % 4;
                    int row = draggedIndex / 4;
                    int iconX = 12 + (col * 55); 
                    int iconY = 30 + (row * 80);

                    int steps = 7; 
                    for (int frame = 1; frame <= steps; frame++) {
                        int currX = iconX - (iconX * frame / steps);
                        int currY = iconY - (iconY * frame / steps);
                        int currW = 46 + ((240 - 46) * frame / steps);
                        int currH = 46 + ((320 - 46) * frame / steps);
                        int currRad = 12 - (12 * frame / steps); 

                        tft->fillRoundRect(currX, currY, currW, currH, currRad, apps[draggedIndex]->iconColor);
                    }
                    
                    delay(80); 
                    App* launchedApp = apps[draggedIndex];
                    draggedIndex = -1; 
                    return launchedApp; 
                }
            }
            draggedIndex = -1;
        }
    }
    return nullptr; 
}