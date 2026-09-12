#include "Toast.h"

#include <stdlib.h>

namespace Toast {
namespace {

static constexpr int WIDTH = 240;

static String    s_text;
static uint32_t  s_expiresAt = 0;
static uint32_t  s_lastPaintMs = 0;
static bool      s_active = false;
static uint16_t* s_snapshot = nullptr;

static void paint(TFT_eSPI* tft) {
    const uint16_t bg = tft->color565(28, 28, 32);
    const uint16_t edge = tft->color565(70, 70, 80);
    tft->fillRect(0, 0, WIDTH, HEIGHT, bg);
    tft->drawFastHLine(0, HEIGHT - 1, WIDTH, edge);
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(TFT_WHITE, bg);
    String shown = s_text;
    while (shown.length() > 1 && tft->textWidth(shown) > WIDTH - 16)
        shown.remove(shown.length() - 1);
    tft->drawString(shown, WIDTH / 2, HEIGHT / 2 - 1);
}

static void restore(TFT_eSPI* tft) {
    if (s_snapshot) {
        tft->pushImage(0, 0, WIDTH, HEIGHT, s_snapshot);
        free(s_snapshot);
        s_snapshot = nullptr;
    }
    s_active = false;
    s_text = "";
}

} // namespace

void show(TFT_eSPI* tft, const String& text) {
    if (!tft) return;
    if (s_active) {
        // Keep the original snapshot: it still holds what was under the strip
        // before any toast was drawn.
        s_text = text;
    } else {
        s_text = text;
        s_snapshot = (uint16_t*)malloc((size_t)WIDTH * HEIGHT * sizeof(uint16_t));
        if (s_snapshot) tft->readRect(0, 0, WIDTH, HEIGHT, s_snapshot);
        s_active = true;
    }
    s_expiresAt = millis() + DURATION_MS;
    paint(tft);
    s_lastPaintMs = millis();
}

void poll(TFT_eSPI* tft) {
    if (!s_active || !tft) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_expiresAt) >= 0) {
        restore(tft);
        return;
    }
    // Scripts repaint freely underneath; re-assert the strip a few times a
    // second so it stays readable without hammering the SPI bus.
    if (now - s_lastPaintMs >= 120) {
        paint(tft);
        s_lastPaintMs = now;
    }
}

bool active() { return s_active; }

} // namespace Toast
