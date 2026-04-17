#include "Calculator.h"
#include <math.h>

Calculator::Calculator(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;

    name = "Calc";
    iconColor = tft->color565(255, 149, 0);
    isApp = true;

    clearAll();
    wasTouched = false;
}

void Calculator::clearAll() {
    currentInput = "0";
    expressionText = "";
    storedValue = 0.0;
    pendingOperator = '\0';
    resetInputOnNextDigit = false;
}

String Calculator::formatNumber(double value) {
    if (isnan(value) || isinf(value)) return "Error";

    long long intPart = (long long)value;
    if (fabs(value - (double)intPart) < 0.0000001) {
        return String((long)value);
    }

    char buffer[32];
    dtostrf(value, 0, 6, buffer);
    String result = String(buffer);
    result.trim();

    while (result.indexOf('.') != -1 && result.endsWith("0")) {
        result.remove(result.length() - 1);
    }
    if (result.endsWith(".")) {
        result.remove(result.length() - 1);
    }

    if (result == "-0") result = "0";
    return result;
}

void Calculator::drawDisplay() {
    tft->fillRect(0, 0, 240, 72, TFT_BLACK);

    tft->setTextDatum(TR_DATUM);
    tft->setTextFont(2);
    tft->setTextSize(1);

    tft->setTextColor(tft->color565(150, 150, 150));
    String smallText = expressionText;
    if (smallText.length() > 22) {
        smallText = "..." + smallText.substring(smallText.length() - 19);
    }
    tft->drawString(smallText, 230, 16);

    tft->setTextColor(TFT_WHITE);
    tft->setTextFont(4);
    String displayText = currentInput;
    if (displayText.length() > 12) {
        displayText = displayText.substring(displayText.length() - 12);
    }
    tft->drawString(displayText, 230, 48);
}

void Calculator::drawButton(const CalcButton& btn) {
    tft->fillRoundRect(btn.x, btn.y, btn.w, btn.h, 10, btn.bgColor);
    tft->setTextColor(btn.textColor);
    tft->setTextDatum(MC_DATUM);
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->drawString(btn.label, btn.x + btn.w / 2, btn.y + btn.h / 2);
}

void Calculator::drawButtons() {
    uint16_t dark = tft->color565(58, 58, 60);
    uint16_t light = tft->color565(165, 165, 170);
    uint16_t orange = tft->color565(255, 149, 0);

    const CalcButton buttons[] = {
        {10, 80, 50, 40, "C", light, TFT_BLACK},
        {65, 80, 50, 40, "<", light, TFT_BLACK},
        {120, 80, 50, 40, "%", light, TFT_BLACK},
        {175, 80, 55, 40, "/", orange, TFT_WHITE},

        {10, 125, 50, 40, "7", dark, TFT_WHITE},
        {65, 125, 50, 40, "8", dark, TFT_WHITE},
        {120, 125, 50, 40, "9", dark, TFT_WHITE},
        {175, 125, 55, 40, "*", orange, TFT_WHITE},

        {10, 170, 50, 40, "4", dark, TFT_WHITE},
        {65, 170, 50, 40, "5", dark, TFT_WHITE},
        {120, 170, 50, 40, "6", dark, TFT_WHITE},
        {175, 170, 55, 40, "-", orange, TFT_WHITE},

        {10, 215, 50, 40, "1", dark, TFT_WHITE},
        {65, 215, 50, 40, "2", dark, TFT_WHITE},
        {120, 215, 50, 40, "3", dark, TFT_WHITE},
        {175, 215, 55, 40, "+", orange, TFT_WHITE},

        {10, 260, 105, 40, "0", dark, TFT_WHITE},
        {120, 260, 50, 40, ".", dark, TFT_WHITE},
        {175, 260, 55, 40, "=", orange, TFT_WHITE},
    };

    for (const auto& btn : buttons) {
        drawButton(btn);
    }
}

bool Calculator::getMappedTouch(int& x, int& y) {
    if (!ts->touched()) return false;

    TS_Point p = ts->getPoint();
    x = map(p.x, 300, 3800, 0, 240);
    y = map(p.y, 300, 3800, 0, 320);
    return true;
}

void Calculator::appendDigit(const String& digit) {
    if (resetInputOnNextDigit) {
        currentInput = "0";
        resetInputOnNextDigit = false;
    }

    if (digit == ".") {
        if (currentInput.indexOf('.') == -1) {
            currentInput += ".";
        }
        return;
    }

    if (currentInput == "0") currentInput = digit;
    else if (currentInput == "-0") currentInput = "-" + digit;
    else if (currentInput.length() < 16) currentInput += digit;
}

void Calculator::setOperator(char op) {
    double currentValue = currentInput.toDouble();

    if (pendingOperator != '\0' && !resetInputOnNextDigit) {
        evaluate();
        currentValue = currentInput.toDouble();
    } else {
        storedValue = currentValue;
    }

    pendingOperator = op;
    expressionText = formatNumber(storedValue) + " " + String(op);
    resetInputOnNextDigit = true;
}

void Calculator::evaluate() {
    if (pendingOperator == '\0') return;

    double currentValue = currentInput.toDouble();
    double result = storedValue;

    switch (pendingOperator) {
        case '+': result = storedValue + currentValue; break;
        case '-': result = storedValue - currentValue; break;
        case '*': result = storedValue * currentValue; break;
        case '/':
            if (fabs(currentValue) < 0.0000001) {
                currentInput = "Error";
                expressionText = "Division by zero";
                storedValue = 0.0;
                pendingOperator = '\0';
                resetInputOnNextDigit = true;
                return;
            }
            result = storedValue / currentValue;
            break;
    }

    expressionText = formatNumber(storedValue) + " " + String(pendingOperator) + " " + formatNumber(currentValue) + " =";
    currentInput = formatNumber(result);
    storedValue = result;
    pendingOperator = '\0';
    resetInputOnNextDigit = true;
}

void Calculator::backspace() {
    if (resetInputOnNextDigit || currentInput == "Error") {
        currentInput = "0";
        resetInputOnNextDigit = false;
        return;
    }

    if (currentInput.length() > 1) {
        currentInput.remove(currentInput.length() - 1);
        if (currentInput == "-") currentInput = "0";
    } else {
        currentInput = "0";
    }
}

void Calculator::handleButtonPress(const String& label) {
    if (currentInput == "Error" && label != "C") {
        clearAll();
    }

    if ((label[0] >= '0' && label[0] <= '9') || label == ".") {
        appendDigit(label);
    } else if (label == "C") {
        clearAll();
    } else if (label == "<") {
        backspace();
    } else if (label == "%") {
        double value = currentInput.toDouble() / 100.0;
        currentInput = formatNumber(value);
        resetInputOnNextDigit = true;
    } else if (label == "+" || label == "-" || label == "*" || label == "/") {
        setOperator(label[0]);
    } else if (label == "=") {
        evaluate();
    }

    drawDisplay();
}

void Calculator::show() {
    tft->fillScreen(tft->color565(28, 28, 30));
    drawDisplay();
    drawButtons();
}

void Calculator::update() {
    int x, y;
    if (!getMappedTouch(x, y)) {
        wasTouched = false;
        return;
    }

    if (wasTouched) return;
    wasTouched = true;

    struct Hit {
        int x;
        int y;
        int w;
        int h;
        const char* label;
    };

    const Hit hits[] = {
        {10, 80, 50, 40, "C"},
        {65, 80, 50, 40, "<"},
        {120, 80, 50, 40, "%"},
        {175, 80, 55, 40, "/"},

        {10, 125, 50, 40, "7"},
        {65, 125, 50, 40, "8"},
        {120, 125, 50, 40, "9"},
        {175, 125, 55, 40, "*"},

        {10, 170, 50, 40, "4"},
        {65, 170, 50, 40, "5"},
        {120, 170, 50, 40, "6"},
        {175, 170, 55, 40, "-"},

        {10, 215, 50, 40, "1"},
        {65, 215, 50, 40, "2"},
        {120, 215, 50, 40, "3"},
        {175, 215, 55, 40, "+"},

        {10, 260, 105, 40, "0"},
        {120, 260, 50, 40, "."},
        {175, 260, 55, 40, "="},
    };

    for (const auto& hit : hits) {
        if (x >= hit.x && x <= hit.x + hit.w && y >= hit.y && y <= hit.y + hit.h) {
            handleButtonPress(hit.label);
            break;
        }
    }
}
