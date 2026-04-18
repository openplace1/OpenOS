#include "OSAApp.h"
#include "Theme.h"
#include "../Services/NotificationService.h"

extern NotificationService notifyService;

// Called by the runtime's notify() built-in
void osa_notify(const char* msg) {
    notifyService.push(msg);
}

OSAApp::OSAApp(TFT_eSPI* t, XPT2046_Touchscreen* touchscreen)
    : runtime(t, touchscreen)
{
    tft  = t;
    ts   = touchscreen;
    name = "App";
    iconColor = tft->color565(255, 149, 0);
    isApp     = true;
}

bool OSAApp::loadScript(const String& path) {
    scriptLoaded = false;
    showDone     = false;
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

    tft->setTextColor(Theme::subtext());
    tft->setTextFont(1);
    String err = runtime.getError();
    // Word-wrap crude split at ~30 chars
    int start = 0;
    int y = 115;
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

    if (runtime.hasError()) {
        drawErrorScreen();
        return;
    }

    // If no loop section, show is all there is — draw a header so it doesn't look bare
    if (!runtime.hasLoop()) {
        // Script has already drawn; nothing extra needed
    }

    showDone = true;
}

void OSAApp::update() {
    if (!scriptLoaded || !showDone) return;
    if (runtime.hasError()) return;

    if (!runtime.hasLoop()) {
        // One-shot script — nothing to update
        return;
    }

    if (!runtime.runUpdate()) {
        // Script called exit() or hit an error
        if (runtime.hasError()) drawErrorScreen();
        // Signal main loop to go back to homescreen by setting exitFlag
        // We can't directly call the state machine, but we can re-use
        // the "swipe up" mechanism — just stop responding so the OS
        // global swipe-up at y>300 is the exit path.
        // For cleaner UX: set showDone=false so update() is a no-op
        showDone = false;
    }
}

void OSAApp::drawHeader() {
    // Called when a notification banner is dismissed — redraw just the header area.
    // Scripts can draw anywhere so we just redraw a minimal top bar.
    drawHeaderBar();
}
