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
    int bannerX = 15;
    int bannerY = 8;
    int bannerW = 210;
    int bannerH = 50;

    lastDrawnY = bannerY;

    tft->fillRoundRect(bannerX, bannerY, bannerW, bannerH, 12, bannerBgColor);
    tft->drawRoundRect(bannerX, bannerY, bannerW, bannerH, 12, bannerBorderColor);

    tft->setTextFont(2);
    tft->setTextSize(1);

    tft->setTextDatum(TL_DATUM);
    tft->setTextColor(tft->color565(180, 180, 185));
    tft->drawString("Notification", bannerX + 12, bannerY + 8);

    tft->setTextColor(bannerTextColor);
    tft->setTextDatum(ML_DATUM);

    String shown = message;
    if (shown.length() > 26) {
        shown = shown.substring(0, 23) + "...";
    }
    tft->drawString(shown, bannerX + 12, bannerY + 31);
}

bool NotificationService::update() {
    if (!isVisible && queueCount > 0) {
        currentMessage = queue[0];
        for (int i = 1; i < queueCount; i++) {
            queue[i - 1] = queue[i];
        }
        queueCount--;

        drawBanner(currentMessage);
        isVisible = true;
        visibleSince = millis();
        return true;
    }

    if (isVisible && millis() - visibleSince > 2200) {
        clearBannerArea();
        isVisible = false;
        currentMessage = "";
        return true;
    }

    return false;
}
