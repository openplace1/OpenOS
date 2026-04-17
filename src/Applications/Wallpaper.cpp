#include "Wallpaper.h"
#include <SD.h>

extern bool isSdReady;
extern bool sysWallpaperEnabled;
extern int  sysTheme;

uint16_t  Wallpaper::lastColor = 0;
uint16_t* Wallpaper::cache     = nullptr;
bool      Wallpaper::cached    = false;

// ── helpers ──────────────────────────────────────────────────────────────────

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

// ── internal draw (write to cache when available, else direct to display) ────

void Wallpaper::drawFallback(TFT_eSPI* tft) {
    lastColor = (sysTheme == 1) ? tft->color565(28, 28, 32) : tft->color565(200, 200, 205);
    if (cache) {
        // Fill cache — draw() will push to display afterwards
        uint16_t* p = cache;
        for (int i = 0; i < 240 * 320; i++) *p++ = lastColor;
    } else {
        tft->fillScreen(lastColor);
    }
}

bool Wallpaper::drawBmp(TFT_eSPI* tft, const String& path) {
    lastColor = 0;
    if (!isSdReady) return false;

    File bmpFile = SD.open(path);
    if (!bmpFile) return false;

    if (wp_read16(bmpFile) != 0x4D42) { bmpFile.close(); return false; }

    wp_read32(bmpFile);
    wp_read32(bmpFile);
    uint32_t imageOffset = wp_read32(bmpFile);
    uint32_t headerSize  = wp_read32(bmpFile);

    int32_t width  = (int32_t)wp_read32(bmpFile);
    int32_t height = (int32_t)wp_read32(bmpFile);

    if (wp_read16(bmpFile) != 1) { bmpFile.close(); return false; }

    uint16_t depth       = wp_read16(bmpFile);
    uint32_t compression = wp_read32(bmpFile);

    if (headerSize < 40 || depth != 24 || compression != 0 || width <= 0 || height == 0) {
        bmpFile.close(); return false;
    }

    bool flip = true;
    if (height < 0) { height = -height; flip = false; }

    uint32_t rowSize = (width * 3 + 3) & ~3;

    uint8_t* sdbuffer = new uint8_t[rowSize];
    if (!sdbuffer) { bmpFile.close(); return false; }

    int drawW = (width  > 240) ? 240 : (int)width;
    int drawH = (height > 320) ? 320 : (int)height;
    int srcOffsetX = (width  > 240) ? (width  - 240) / 2 : 0;
    int srcOffsetY = (height > 320) ? (height - 320) / 2 : 0;
    int dstX = (width  < 240) ? (240 - width)  / 2 : 0;
    int dstY = (height < 320) ? (320 - height) / 2 : 0;

    uint16_t lineBuffer[240];

    // Fill background for areas not covered by the image
    drawFallback(tft);

    for (int y = 0; y < drawH; y++) {
        int srcY    = y + srcOffsetY;
        int fileRow = flip ? (height - 1 - srcY) : srcY;

        bmpFile.seek(imageOffset + (uint32_t)fileRow * rowSize);
        bmpFile.read(sdbuffer, rowSize);

        for (int x = 0; x < drawW; x++) {
            int srcX = x + srcOffsetX;
            int idx  = srcX * 3;
            uint8_t b = sdbuffer[idx];
            uint8_t g = sdbuffer[idx + 1];
            uint8_t r = sdbuffer[idx + 2];
            lineBuffer[x] = tft->color565(r, g, b);
        }

        if (cache) {
            memcpy(cache + (dstY + y) * 240 + dstX, lineBuffer, drawW * 2);
        } else {
            tft->startWrite();
            tft->setAddrWindow(dstX, dstY + y, drawW, 1);
            tft->pushColors(lineBuffer, drawW, true);
            tft->endWrite();
        }
    }

    delete[] sdbuffer;
    bmpFile.close();
    return true;
}

// ── public API ────────────────────────────────────────────────────────────────

void Wallpaper::draw(TFT_eSPI* tft) {
    // Fast path — already cached
    if (cached) {
        if (cache) {
            tft->startWrite();
            tft->setAddrWindow(0, 0, 240, 320);
            tft->pushColors(cache, 240 * 320, true);
            tft->endWrite();
        } else {
            // No pixel buffer but flat colour is known
            tft->fillScreen(lastColor);
        }
        return;
    }

    // Wallpaper disabled — use flat fallback colour, cache it
    if (!sysWallpaperEnabled) {
        if (!cache) {
            cache = (uint16_t*)ps_malloc(240 * 320 * 2);
            if (!cache) cache = (uint16_t*)malloc(240 * 320 * 2);
        }
        drawFallback(tft);
        if (cache) {
            tft->startWrite();
            tft->setAddrWindow(0, 0, 240, 320);
            tft->pushColors(cache, 240 * 320, true);
            tft->endWrite();
        } else {
            tft->fillScreen(lastColor);
        }
        cached = true;
        return;
    }

    // Try PSRAM first (4 MB on boards that have it), then internal SRAM
    if (!cache) {
        cache = (uint16_t*)ps_malloc(240 * 320 * 2);
        if (!cache) cache = (uint16_t*)malloc(240 * 320 * 2);
        Serial.printf("[Wallpaper] free heap: %u  cache: %s\n",
                      ESP.getFreeHeap(), cache ? "OK" : "no buffer");
    }

    // Render into cache (or directly to display when cache is null)
    String wallpaperPath;
    bool drawn = false;
    if (loadWallpaperPath(wallpaperPath))
        drawn = drawBmp(tft, wallpaperPath);
    if (!drawn && isSdReady)
        drawn = drawBmp(tft, "/system/assets/wallpapers/default.bmp");
    if (!drawn)
        drawFallback(tft);

    if (cache) {
        // Push full pixel buffer to display
        tft->startWrite();
        tft->setAddrWindow(0, 0, 240, 320);
        tft->pushColors(cache, 240 * 320, true);
        tft->endWrite();
        cached = true;
    } else if (!drawn && lastColor) {
        // Only cache the flat-colour fast path when no BMP was drawn.
        // If a BMP was drawn, drawFallback() set lastColor internally but we
        // need the actual pixels — without a buffer we must keep re-reading SD.
        cached = true;
    }
}

bool Wallpaper::drawRegion(TFT_eSPI* tft, int x, int y, int w, int h) {
    if (!cached || !cache) return false;

    // Clamp to screen bounds
    if (x < 0)       { w += x; x = 0; }
    if (y < 0)       { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 320) h = 320 - y;
    if (w <= 0 || h <= 0) return true;

    tft->startWrite();
    tft->setAddrWindow(x, y, w, h);
    for (int row = 0; row < h; row++) {
        tft->pushColors(cache + (y + row) * 240 + x, w, true);
    }
    tft->endWrite();
    return true;
}

void Wallpaper::invalidate() {
    cached = false; // keep buffer allocated for reuse
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
