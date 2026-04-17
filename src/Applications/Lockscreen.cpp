#include "Lockscreen.h"
#include "Wallpaper.h"
#include <time.h>
#include <SD.h>

extern bool isSdReady;

Lockscreen::Lockscreen(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    kbd = new OSKeyboard(tft, ts);

    isSwiping = false;
    isUnlocked = false;
    enteringPassword = false;
    startTouchY = 0;

    systemPassword = "";
    enteredPassword = "";
    showTypedPassword = false;
    wasTouched = false;
    numericMode = false;
    statusMessage = "";
    statusUntil = 0;
}

Lockscreen::~Lockscreen() {
    delete kbd;
}

void Lockscreen::loadPassword() {
    systemPassword = "";
    numericMode = false;

    if (!isSdReady) return;
    File f = SD.open("/user/pwd.txt", FILE_READ);
    if (!f) return;

    systemPassword = f.readStringUntil('\n');
    systemPassword.trim();
    f.close();

    numericMode = isNumericPassword(systemPassword);
}

bool Lockscreen::isNumericPassword(const String& pwd) {
    if (pwd.length() == 0) return false;
    for (int i = 0; i < pwd.length(); i++) {
        char c = pwd[i];
        if (c < '0' || c > '9') return false;
    }
    return true;
}

void Lockscreen::show() {
    loadPassword();
    Wallpaper::draw(tft);
    drawStatusBar();
    drawClock();

    if (systemPassword.length() == 0 && !enteringPassword) {
        drawSwipeHint();
    } else if (!enteringPassword) {
        tft->setTextFont(2);
        tft->setTextColor(tft->color565(180, 180, 180));
        tft->setTextDatum(MC_DATUM);
        tft->drawString("^", 120, 275);
        tft->drawString("Swipe up to unlock", 120, 295);
    } else {
        drawPasswordScreen();
    }
}

void Lockscreen::drawStatusBar() {
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(TL_DATUM);
    tft->drawString("CYD", 5, 4);

    tft->setTextDatum(TR_DATUM);
    tft->drawString("Carrier", 210, 4);
    tft->drawRect(215, 6, 16, 8, TFT_WHITE);
    tft->fillRect(217, 8, 12, 4, TFT_WHITE);
    tft->fillRect(231, 8, 2, 4, TFT_WHITE);
}

void Lockscreen::drawClock() {
    tft->setTextFont(6);
    tft->setTextSize(1);
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(MC_DATUM);

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    tft->drawString(timeStr, 120, 80);

    tft->setTextFont(2);
    tft->setTextColor(tft->color565(220, 220, 220));

    char dateStr[30];
    sprintf(dateStr, "%02d.%02d.%d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    tft->drawString(String(dateStr), 120, 130);
}

void Lockscreen::drawSwipeHint() {
    tft->setTextFont(2);
    tft->setTextColor(tft->color565(180, 180, 180));
    tft->setTextDatum(MC_DATUM);

    tft->drawString("^", 120, 275);
    tft->drawString("Swipe up to unlock", 120, 295);
}




void Lockscreen::drawPasscodeDots() {
    int dotY = 78;
    int count = systemPassword.length();
    if (count < 4) count = 4;
    if (count > 8) count = 8;

    int spacing = 22;
    int totalW = (count - 1) * spacing;
    int startX = 120 - totalW / 2;

    for (int i = 0; i < count; i++) {
        int x = startX + i * spacing;
        if (i < (int)enteredPassword.length()) {
            tft->fillCircle(x, dotY, 5, TFT_WHITE);
        } else {
            tft->drawCircle(x, dotY, 5, TFT_WHITE);
        }
    }
}





void Lockscreen::drawNumericButton(int x, int y, int r, const String& label, const String& sublabel) {
    uint16_t fill = tft->color565(88, 88, 92);
    uint16_t edge = tft->color565(145, 145, 150);

    tft->fillCircle(x, y, r, fill);
    tft->drawCircle(x, y, r, edge);

    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(TFT_WHITE);
    tft->setTextFont(4);
    tft->drawString(label, x, y - 4);

    if (sublabel.length() > 0) {
        tft->setTextFont(1);
        tft->setTextColor(tft->color565(220, 220, 220));
        tft->drawString(sublabel, x, y + 11);
    }
}








void Lockscreen::drawNumericPad() {
    
    uint16_t btnFill   = tft->color565(30, 30, 30);  
    uint16_t btnBorder = tft->color565(255, 255, 255);  

    const int COLS[3]  = {40, 120, 200};
    const int ROWS[4]  = {118, 166, 214, 262};
    const int BUTTON_WIDTH = 68;   
    const int BUTTON_HEIGHT = 40;   
    const int BUTTON_RADIUS = 7;    

    
    auto drawBtn = [&](int cx, int cy, const char* lbl) {
        int x = cx - BUTTON_WIDTH / 2;
        int y = cy - BUTTON_HEIGHT / 2;
        tft->fillRoundRect(x, y, BUTTON_WIDTH, BUTTON_HEIGHT, BUTTON_RADIUS, btnFill);
        tft->drawRoundRect(x, y, BUTTON_WIDTH, BUTTON_HEIGHT, BUTTON_RADIUS, btnBorder);
        tft->setTextFont(4);
        tft->setTextColor(TFT_WHITE);
        tft->setTextDatum(MC_DATUM);
        tft->drawString(lbl, cx, cy);
    };

    
    drawBtn(COLS[0], ROWS[0], "1");
    drawBtn(COLS[1], ROWS[0], "2");
    drawBtn(COLS[2], ROWS[0], "3");

    drawBtn(COLS[0], ROWS[1], "4");
    drawBtn(COLS[1], ROWS[1], "5");
    drawBtn(COLS[2], ROWS[1], "6");

    drawBtn(COLS[0], ROWS[2], "7");
    drawBtn(COLS[1], ROWS[2], "8");
    drawBtn(COLS[2], ROWS[2], "9");

    
    drawBtn(COLS[1], ROWS[3], "0");
    drawBtn(COLS[2], ROWS[3], "<");
    
}

void Lockscreen::drawKeyboardPasswordScreen() {
    tft->fillRoundRect(15, 145, 210, 100, 14, tft->color565(245, 245, 248));
    tft->drawRoundRect(15, 145, 210, 100, 14, tft->color565(200, 200, 205));

    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(TFT_BLACK);
    tft->drawString("Enter Passcode", 120, 163);

    tft->fillRect(30, 182, 130, 34, TFT_WHITE);
    tft->drawRect(30, 182, 130, 34, tft->color565(200, 200, 200));
    tft->fillRect(165, 182, 45, 34, tft->color565(220, 220, 220));
    tft->drawRect(165, 182, 45, 34, tft->color565(200, 200, 200));

    tft->setTextColor(TFT_BLACK);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(showTypedPassword ? "Hide" : "Show", 187, 199);

    redrawPasswordInput();

    tft->setTextColor(tft->color565(120, 120, 120));
    tft->drawString("Enter on keyboard = unlock", 120, 230);

    kbd->draw();
}




void Lockscreen::drawPasswordScreen() {
    
    uint16_t bgColor = tft->color565(30, 30, 30);
    tft->fillScreen(bgColor);

    
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(tft->color565(100, 210, 255));
    tft->setTextDatum(ML_DATUM);
    tft->drawString("< Cancel", 10, 22);

    
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextColor(TFT_WHITE);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("Enter Passcode", 120, 50);

    if (numericMode) {
        drawPasscodeDots();
        drawNumericPad();
    } else {
        drawKeyboardPasswordScreen();
    }

    
    if (statusMessage.length() > 0 && millis() < statusUntil) {
        tft->setTextFont(2);
        tft->setTextSize(1);
        tft->setTextDatum(MC_DATUM);
        tft->setTextColor(tft->color565(255, 120, 120));
        tft->drawString(statusMessage, 120, numericMode ? 96 : 132);
    }
}

void Lockscreen::redrawPasswordInput() {
    if (numericMode) {
        drawPasswordScreen();
        return;
    }

    tft->fillRect(31, 183, 128, 32, TFT_WHITE);
    tft->setTextDatum(ML_DATUM);
    tft->setTextFont(2);
    tft->setTextSize(1);

    if (enteredPassword.length() == 0) {
        tft->setTextColor(tft->color565(150, 150, 150));
        tft->drawString("Passcode...", 40, 199);
    } else {
        tft->setTextColor(TFT_BLACK);
        if (showTypedPassword) {
            tft->drawString(enteredPassword + "_", 40, 199);
        } else {
            String masked = "";
            for (int i = 0; i < enteredPassword.length(); i++) masked += "*";
            tft->drawString(masked + "_", 40, 199);
        }
    }
}

void Lockscreen::getMappedTouch(int &x, int &y) {
    TS_Point p = ts->getPoint();
    x = map(p.x, 300, 3800, 0, 240);
    y = map(p.y, 300, 3800, 0, 320);
}

void Lockscreen::submitPassword() {
    if (enteredPassword == systemPassword) {
        isUnlocked = true;
        enteringPassword = false;
        tft->fillScreen(TFT_BLACK);
        return;
    }
    showWrongPassword();
}

void Lockscreen::showWrongPassword() {
    enteredPassword = "";
    statusMessage = "Wrong passcode";
    statusUntil = millis() + 900;
    drawPasswordScreen();
    delay(250);
}






bool Lockscreen::handleNumericTouch(int touchX, int touchY) {
    const int COLS[3] = {40, 120, 200};
    const int ROWS[4] = {118, 166, 214, 262};
    const int BW = 68;
    const int BH = 40;

    
    const char* digits[9] = {"1","2","3","4","5","6","7","8","9"};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int cx = COLS[col];
            int cy = ROWS[row];
            if (abs(touchX - cx) <= BW / 2 && abs(touchY - cy) <= BH / 2) {
                if (enteredPassword.length() < 24) {
                    enteredPassword += digits[row * 3 + col];
                    drawPasswordScreen();
                    if (enteredPassword.length() == systemPassword.length()) {
                        submitPassword();
                    }
                }
                return true;
            }
        }
    }

    
    if (abs(touchX - COLS[1]) <= BW / 2 && abs(touchY - ROWS[3]) <= BH / 2) {
        if (enteredPassword.length() < 24) {
            enteredPassword += "0";
            drawPasswordScreen();
            if (enteredPassword.length() == systemPassword.length()) {
                submitPassword();
            }
        }
        return true;
    }

    
    if (abs(touchX - COLS[2]) <= BW / 2 && abs(touchY - ROWS[3]) <= BH / 2) {
        if (enteredPassword.length() > 0) {
            enteredPassword.remove(enteredPassword.length() - 1);
            drawPasswordScreen();
        }
        return true;
    }

    
    if (touchY < 38 && touchX < 100) {
        enteringPassword = false;
        enteredPassword = "";
        statusMessage = "";
        show();
        return true;
    }

    return false;
}

bool Lockscreen::update() {
    if (isUnlocked) return true;

    static int lastMinute = -1;
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (lastMinute == -1) {
        lastMinute = timeinfo.tm_min;
    } else if (lastMinute != timeinfo.tm_min && !enteringPassword) {
        lastMinute = timeinfo.tm_min;
        show();
    }

    if (statusMessage.length() > 0 && millis() > statusUntil) {
        statusMessage = "";
        if (enteringPassword) drawPasswordScreen();
    }

    if (!enteringPassword) {
        if (ts->touched()) {
            int touchX, touchY;
            getMappedTouch(touchX, touchY);

            if (!isSwiping) {
                isSwiping = true;
                startTouchY = touchY;
            } else {
                if (startTouchY - touchY > 60) {
                    isSwiping = false;
                    if (systemPassword.length() == 0) {
                        isUnlocked = true;
                        tft->fillScreen(TFT_BLACK);
                        return true;
                    } else {
                        enteringPassword = true;
                        enteredPassword = "";
                        showTypedPassword = false;
                        statusMessage = "";
                        drawPasswordScreen();
                        return false;
                    }
                }
            }
        } else {
            isSwiping = false;
        }
        return false;
    }

    if (numericMode) {
        if (ts->touched()) {
            int touchX, touchY;
            getMappedTouch(touchX, touchY);
            if (!wasTouched) {
                wasTouched = true;
                handleNumericTouch(touchX, touchY);
            }
        } else {
            wasTouched = false;
        }
        return isUnlocked;
    }

    if (ts->touched()) {
        int touchX, touchY;
        getMappedTouch(touchX, touchY);

        if (!wasTouched) {
            wasTouched = true;

            if (touchY > 182 && touchY < 216 && touchX > 165 && touchX < 210) {
                showTypedPassword = !showTypedPassword;
                redrawPasswordInput();
                delay(150);
                return false;
            }

            if (touchY > 286) {
                enteringPassword = false;
                enteredPassword = "";
                showTypedPassword = false;
                statusMessage = "";
                show();
                return false;
            }
        }
    } else {
        wasTouched = false;
    }

    char c = kbd->update();
    if (c != '\0') {
        if (c == '\b') {
            if (enteredPassword.length() > 0) enteredPassword.remove(enteredPassword.length() - 1);
        } else if (c == '\n') {
            submitPassword();
        } else {
            if (enteredPassword.length() < 24) enteredPassword += c;
        }
        redrawPasswordInput();
    }

    return isUnlocked;
}

void Lockscreen::lock() {
    isUnlocked = false;
    isSwiping = false;
    enteringPassword = false;
    enteredPassword = "";
    showTypedPassword = false;
    wasTouched = false;
    statusMessage = "";
    loadPassword();
    show();
}