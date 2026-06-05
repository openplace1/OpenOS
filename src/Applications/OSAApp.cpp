#include "OSAApp.h"
#include "Theme.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// notify() builtin used to forward here; with NotificationService removed
// it's a silent no-op (the builtin itself still exists for back-compat).
void osa_notify(const char*) {}

OSAApp::OSAApp(TFT_eSPI* t, XPT2046_Touchscreen* touchscreen)
    : tft(t), ts(touchscreen), runtime(t, touchscreen)
{}

bool OSAApp::loadScript(const String& path) {
    scriptLoaded = showDone = wantsExit = false;
    if (!runtime.loadScript(path)) return false;
    name = runtime.appName.length() > 0 ? runtime.appName : "App";
    scriptLoaded = true;
    return true;
}

void OSAApp::drawHeaderBar() {
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    String title = name;
    if (title.length() > 20) title = title.substring(0, 18) + "..";
    tft->drawString(title, 120, 25);
}

void OSAApp::drawErrorScreen() {
    tft->fillScreen(Theme::bg());
    drawHeaderBar();
    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(255, 59, 48));
    tft->setTextDatum(MC_DATUM);
    tft->drawString("Script Error", 120, 90);

    tft->setTextColor(Theme::subtext()); tft->setTextFont(1);
    String err = runtime.getError();
    int start = 0, y = 115;
    while (start < (int)err.length() && y < 290) {
        int end = min(start + 30, (int)err.length());
        tft->drawString(err.substring(start, end), 120, y);
        start = end; y += 16;
    }
}

void OSAApp::show() {
    if (!scriptLoaded) {
        tft->fillScreen(Theme::bg());
        drawHeaderBar();
        tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
        tft->drawString("No script loaded", 120, 160);
        return;
    }
    showDone = false;
    tft->fillScreen(Theme::bg());
    runtime.runShow();
    if (runtime.hasError()) { drawErrorScreen(); return; }
    showDone = true;
}

void OSAApp::update() {
    if (!scriptLoaded || !showDone || runtime.hasError()) return;
    if (!runtime.hasLoop()) return;
    if (!runtime.runUpdate()) {
        if (runtime.hasError()) drawErrorScreen();
        showDone = false;
        wantsExit = true;
    }
}

void OSAApp::drawHeader() { drawHeaderBar(); }

// ─── Parallel pre-render ─────────────────────────────────────────────────────
// Runs the script's setup section into a sprite on core 0 while the
// open-animation plays on core 1. finishPreRender() blits the result.

void OSAApp::preRenderTaskFn(void* arg) {
    OSAApp* self = (OSAApp*)arg;
    self->runtime.runShow();
    self->preRenderDone = true;
    vTaskDelete(nullptr);
}

void OSAApp::beginPreRender() {
    if (!scriptLoaded) return;
    preRenderDone   = false;
    preRenderSprite = new TFT_eSprite(tft);
    if (!preRenderSprite) return;
    preRenderSprite->setColorDepth(8);
    if (!preRenderSprite->createSprite(240, 320)) {
        delete preRenderSprite;
        preRenderSprite = nullptr;
        return;
    }
    preRenderSprite->fillSprite(TFT_BLACK);
    runtime.setActiveSprite(preRenderSprite);
    BaseType_t r = xTaskCreatePinnedToCore(preRenderTaskFn, "preRender",
                                           8192, this, 1, nullptr, 0);
    if (r != pdPASS) {
        runtime.setActiveSprite(nullptr);
        preRenderSprite->deleteSprite();
        delete preRenderSprite;
        preRenderSprite = nullptr;
    }
}

bool OSAApp::finishPreRender() {
    if (!preRenderSprite) return false;
    while (!preRenderDone) { yield(); delay(2); }
    bool ours = (runtime.getActiveSprite() == preRenderSprite);
    if (ours) {
        preRenderSprite->pushSprite(0, 0);
        runtime.setActiveSprite(nullptr);
        preRenderSprite->deleteSprite();
        delete preRenderSprite;
    }
    preRenderSprite = nullptr;
    showDone = true;
    return ours;
}
