#ifndef NOTIFICATIONSERVICE_H
#define NOTIFICATIONSERVICE_H

#include <Arduino.h>
#include <TFT_eSPI.h>

class NotificationService {
private:
    static const int MAX_NOTIFICATIONS = 5;

    TFT_eSPI* tft;
    String queue[MAX_NOTIFICATIONS];
    int queueCount;

    bool isVisible;
    unsigned long visibleSince;
    String currentMessage;
    int lastDrawnY;

    uint16_t bannerBgColor;
    uint16_t bannerTextColor;
    uint16_t bannerBorderColor;

    void drawBanner(const String& message);
    void clearBannerArea();

public:
    NotificationService(TFT_eSPI* tftInstance);

    void push(const String& message);
    int update(); // 0=nothing, 1=banner appeared, 2=banner dismissed
    bool hasActiveNotification() const;
};

#endif
