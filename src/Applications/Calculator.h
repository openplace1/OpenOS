#ifndef CALCULATOR_H
#define CALCULATOR_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "../App.h"

class Calculator : public App {
private:
    struct CalcButton {
        int x;
        int y;
        int w;
        int h;
        const char* label;
        uint16_t bgColor;
        uint16_t textColor;
    };

    String currentInput;
    String expressionText;
    double storedValue;
    char pendingOperator;
    bool resetInputOnNextDigit;
    bool wasTouched;

    void drawTopBar();
    void drawDisplay();
    void drawButtons();
    void drawButton(const CalcButton& btn);
    bool getMappedTouch(int& x, int& y);
    void handleButtonPress(const String& label);
    void appendDigit(const String& digit);
    void setOperator(char op);
    void evaluate();
    void clearAll();
    void backspace();
    String formatNumber(double value);

public:
    Calculator(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);

    void show() override;
    void update() override;
    void drawHeader() override;
};

#endif