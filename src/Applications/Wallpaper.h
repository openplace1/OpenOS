#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <Arduino.h>
#include <TFT_eSPI.h>

class Wallpaper {
private:
    static bool loadWallpaperPath(String& outPath);
    static bool drawBmp(TFT_eSPI* tft, const String& path);
    static void drawFallback(TFT_eSPI* tft);

public:
    static void draw(TFT_eSPI* tft);
};

#endif
