#include "Wallpaper.h"
#include <SD.h>

extern bool isSdReady;


static uint16_t wp_read16(File& f) {
    uint16_t result;
    ((uint8_t*)&result)[0] = f.read();
    ((uint8_t*)&result)[1] = f.read();
    return result;
}

static uint32_t wp_read32(File& f) {
    uint32_t result;
    ((uint8_t*)&result)[0] = f.read();
    ((uint8_t*)&result)[1] = f.read();
    ((uint8_t*)&result)[2] = f.read();
    ((uint8_t*)&result)[3] = f.read();
    return result;
}

bool Wallpaper::loadWallpaperPath(String& outPath) {
    outPath = "";

    if (!isSdReady) return false;

    File cfg = SD.open("/system/wp.txt");
    if (!cfg) return false;

    String line = cfg.readStringUntil('\n');
    cfg.close();

    line.trim();
    if (line.length() == 0) return false;

    outPath = line;
    return true;
}

void Wallpaper::drawFallback(TFT_eSPI* tft) {
    tft->fillScreen(tft->color565(100, 100, 105));
}

bool Wallpaper::drawBmp(TFT_eSPI* tft, const String& path) {
    if (!isSdReady) return false;

    File bmpFile = SD.open(path);
    if (!bmpFile) return false;

    if (wp_read16(bmpFile) != 0x4D42) {
        bmpFile.close();
        return false;
    }

    wp_read32(bmpFile); 
    wp_read32(bmpFile); 
    uint32_t imageOffset = wp_read32(bmpFile);
    uint32_t headerSize = wp_read32(bmpFile);

    int32_t width = (int32_t)wp_read32(bmpFile);
    int32_t height = (int32_t)wp_read32(bmpFile);

    if (wp_read16(bmpFile) != 1) {
        bmpFile.close();
        return false;
    }

    uint16_t depth = wp_read16(bmpFile);
    uint32_t compression = wp_read32(bmpFile);

    if (headerSize < 40 || depth != 24 || compression != 0 || width <= 0 || height == 0) {
        bmpFile.close();
        return false;
    }

    bool flip = true;
    if (height < 0) {
        height = -height;
        flip = false;
    }

    uint32_t rowSize = (width * 3 + 3) & ~3;

    uint8_t* sdbuffer = new uint8_t[rowSize];
    if (!sdbuffer) {
        bmpFile.close();
        return false;
    }

    int drawW = width;
    int drawH = height;
    if (drawW > 240) drawW = 240;
    if (drawH > 320) drawH = 320;

    int srcOffsetX = 0;
    int srcOffsetY = 0;

    if (width > 240) srcOffsetX = (width - 240) / 2;
    if (height > 320) srcOffsetY = (height - 320) / 2;

    int dstX = 0;
    int dstY = 0;

    if (width < 240) dstX = (240 - width) / 2;
    if (height < 320) dstY = (320 - height) / 2;

    uint16_t lineBuffer[240];

    
    drawFallback(tft);

    for (int y = 0; y < drawH; y++) {
        int srcY = y + srcOffsetY;
        int fileRow = flip ? (height - 1 - srcY) : srcY;

        bmpFile.seek(imageOffset + (uint32_t)fileRow * rowSize);
        bmpFile.read(sdbuffer, rowSize);

        for (int x = 0; x < drawW; x++) {
            int srcX = x + srcOffsetX;
            int idx = srcX * 3;

            uint8_t b = sdbuffer[idx];
            uint8_t g = sdbuffer[idx + 1];
            uint8_t r = sdbuffer[idx + 2];

            lineBuffer[x] = tft->color565(r, g, b);
        }

        tft->startWrite();
        tft->setAddrWindow(dstX, dstY + y, drawW, 1);
        tft->pushColors(lineBuffer, drawW, true);
        tft->endWrite();
    }

    delete[] sdbuffer;
    bmpFile.close();
    return true;
}

void Wallpaper::draw(TFT_eSPI* tft) {
    String wallpaperPath;

    if (loadWallpaperPath(wallpaperPath)) {
        if (drawBmp(tft, wallpaperPath)) {
            return;
        }
    }

    
    if (isSdReady) {
        if (drawBmp(tft, "/system/assets/wallpapers/default.bmp")) {
            return;
        }
    }

    
    drawFallback(tft);
}
