#include "TestKeyboard.h"

TestKeyboard::TestKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    
    name = "Notes"; 
    iconColor = tft->color565(255, 204, 0); 
    isApp = true;

    kbd = new OSKeyboard(tft, ts);
    textBuffer = "";
}

TestKeyboard::~TestKeyboard() {
    delete kbd;
}

void TestKeyboard::show() {
    tft->fillScreen(TFT_WHITE); 
    drawTextArea();
    kbd->draw();
}

void TestKeyboard::drawTextArea() {
    
    tft->fillRect(0, 0, 240, 160, TFT_WHITE);
    
    tft->setTextColor(TFT_BLACK);
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextDatum(TL_DATUM);
    
    tft->setTextWrap(true, true);
    tft->setCursor(5, 5); 
    tft->print(textBuffer);
    tft->print("_");
}

void TestKeyboard::update() {
    char c = kbd->update();

    if (c != '\0') {
        if (c == '\b') {
            
            if (textBuffer.length() > 0) {
                textBuffer.remove(textBuffer.length() - 1);
            }
        } else {
            
            if (textBuffer.length() < 150) {
                textBuffer += c;
            }
        }
        drawTextArea();
    }
}