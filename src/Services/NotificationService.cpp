#include "NotificationService.h"

NotificationService::NotificationService(TFT_eSPI* tftInstance) {
    tft = tftInstance;
    queueCount = 0;

    isVisible = false;
    visibleSince = 0;
    currentMessage = "";
    lastDrawnY = 8;

    bannerBgColor = tft->color565(35, 35, 40);
    bannerTextColor = TFT_WHITE;
    bannerBorderColor = tft->color565(70, 70, 78);
}

void NotificationService::push(const String& message) {
    if (message.length() == 0) return;

    if (queueCount >= MAX_NOTIFICATIONS) {
        for (int i = 1; i < MAX_NOTIFICATIONS; i++) {
            queue[i - 1] = queue[i];
        }
        queueCount = MAX_NOTIFICATIONS - 1;
    }

    queue[queueCount++] = message;

    if (!isVisible) {
        currentMessage = queue[0];
        for (int i = 1; i < queueCount; i++) {
            queue[i - 1] = queue[i];
        }
        queueCount--;

        drawBanner(currentMessage);
        isVisible = true;
        visibleSince = millis();
    }
}

bool NotificationService::hasActiveNotification() const {
    return isVisible || queueCount > 0;
}

void NotificationService::clearBannerArea() {
    tft->fillRect(12, lastDrawnY - 2, 216, 56, TFT_BLACK);
}

void NotificationService::drawBanner(const String& message) {
    const int bannerX = 12;
    const int bannerY = 6;
    const int bannerW = 216;
    const int bannerH = 44;   // ends at y=50 — stays within the universal 50-px header

    lastDrawnY = bannerY;

    tft->fillRoundRect(bannerX, bannerY, bannerW, bannerH, 10, bannerBgColor);
    tft->drawRoundRect(bannerX, bannerY, bannerW, bannerH, 10, bannerBorderColor);

    // Accent dot
    tft->fillCircle(bannerX + 14, bannerY + 22, 4, tft->color565(0, 122, 255));

    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextDatum(ML_DATUM);

    tft->setTextColor(tft->color565(160, 160, 165));
    tft->drawString("OpenOS", bannerX + 24, bannerY + 12);

    tft->setTextColor(bannerTextColor);
    String shown = message;
    if (shown.length() > 26) shown = shown.substring(0, 23) + "...";
    tft->drawString(shown, bannerX + 24, bannerY + 30);
}

int NotificationService::update() {
    if (!isVisible && queueCount > 0) {
        currentMessage = queue[0];
        for (int i = 1; i < queueCount; i++) {
            queue[i - 1] = queue[i];
        }
        queueCount--;

        drawBanner(currentMessage);
        isVisible = true;
        visibleSince = millis();
        return 1; // banner appeared — drew itself, no full redraw needed
    }

    if (isVisible && millis() - visibleSince > 2200) {
        isVisible = false;
        currentMessage = "";
        return 2; // banner dismissed — caller must restore the area
    }

    return 0;
}
