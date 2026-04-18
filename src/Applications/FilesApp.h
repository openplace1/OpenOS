#ifndef FILESAPP_H
#define FILESAPP_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "App.h"

class FilesApp : public App {
private:
    static const int MAX_FILES = 64;

    String currentPath;
    String fileNames[MAX_FILES];
    bool isDirectory[MAX_FILES];
    int fileCount;
    bool sdInitialized;

    SPIClass* sdSPI;

    int scrollOffset;
    int lastTouchY;
    int touchStartY;
    bool isScrollingList;
    bool wasTouched;

    void loadDirectory(String path);
    void drawTopBar();
    void drawFileList();

public:
    FilesApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    ~FilesApp() override;

    void show() override;
    void update() override;
    void drawHeader() override;

    // Returns non-empty path when user tapped a .osa file; clears after read
    String getPendingOSA() {
        String p = _pendingOSA; _pendingOSA = ""; return p;
    }

private:
    String _pendingOSA;
};

#endif
