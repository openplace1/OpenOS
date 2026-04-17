#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <Arduino.h>
#include <TFT_eSPI.h>

class Wallpaper {
private:
    static uint16_t* cache;  // 240*320*2 = 150KB heap buffer
    static bool      cached;
    static bool loadWallpaperPath(String& outPath);
    static bool drawBmp(TFT_eSPI* tft, const String& path);
    static void drawFallback(TFT_eSPI* tft);

public:
    static uint16_t lastColor; // flat color set by drawFallback; 0 when BMP used

    // First call: reads from SD / generates fallback, stores in cache, pushes to display.
    // All subsequent calls: just pushes cache — no SD access.
    static void draw(TFT_eSPI* tft);

    // Push a rectangular region from cache to display (for targeted redraws).
    // Returns false if cache not yet loaded.
    static bool drawRegion(TFT_eSPI* tft, int x, int y, int w, int h);

    // Force reload on next draw() (call after user changes wallpaper).
    static void invalidate();
};

#endif
