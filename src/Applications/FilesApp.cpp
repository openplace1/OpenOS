#include "FilesApp.h"
#include "Theme.h"


#define SD_CS 5 
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18

FilesApp::FilesApp(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts = tsInstance;
    
    name = "Files"; 
    iconColor = tft->color565(52, 199, 89); 
    isApp = true;

    currentPath = "/";
    fileCount = 0;
    sdInitialized = false; 
    
    scrollOffset = 0;
    lastTouchY = -1;
    touchStartY = -1;
    isScrollingList = false;
    wasTouched = false;

    
    sdSPI = new SPIClass(HSPI); 
}

FilesApp::~FilesApp() {
    delete sdSPI;
}

void FilesApp::loadDirectory(String path) {
    fileCount = 0;
    scrollOffset = 0;

    if (!sdInitialized) return; 

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        return;
    }

    File file = dir.openNextFile();
    while (file && fileCount < MAX_FILES) {
        String dName = String(file.name());
        int lastSlash = dName.lastIndexOf('/');
        if (lastSlash >= 0) {
            dName = dName.substring(lastSlash + 1);
        }

        fileNames[fileCount] = dName;
        isDirectory[fileCount] = file.isDirectory();
        fileCount++;
        file = dir.openNextFile();
    }
}

void FilesApp::show() {
    tft->fillScreen(Theme::bg());
    
    
    if (!sdInitialized) {
        sdSPI->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        
        if (SD.begin(SD_CS, *sdSPI, 4000000)) {
            sdInitialized = true;
        } else {
            Serial.println("SD Card is ghosting! Wrong format or bad slot.");
        }
    }

    if (sdInitialized) {
        loadDirectory(currentPath);
    }

    drawTopBar();
    drawFileList();
}

void FilesApp::drawTopBar() {
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());

    tft->setTextFont(2);
    tft->setTextSize(1);

    if (currentPath != "/") {
        tft->setTextColor(tft->color565(0, 122, 255));
        tft->setTextDatum(ML_DATUM);
        tft->drawString("< Back", 10, 25);
    }

    tft->setTextColor(Theme::text());
    tft->setTextDatum(MC_DATUM);
    
    String title = (currentPath == "/") ? "Files" : currentPath;
    if (title.length() > 16) title = "..." + title.substring(title.length() - 13);
    tft->drawString(title, 120, 25);
}

void FilesApp::drawFileList() {
    tft->setViewport(0, 51, 240, 269); 
    tft->fillScreen(Theme::bg());

    if (!sdInitialized) {
        
        tft->setTextColor(tft->color565(255, 59, 48));
        tft->setTextDatum(MC_DATUM);
        tft->drawString("No card or bad format", 120, 100);
        tft->setTextColor(Theme::hint());
        tft->drawString("FAT32 required", 120, 130);
    }
    else if (fileCount == 0) {
        tft->setTextColor(Theme::hint());
        tft->setTextDatum(MC_DATUM);
        tft->drawString("Directory is empty", 120, 100);
    } else {
        for (int i = 0; i < fileCount; i++) {
            int rowY = (i * 45) - scrollOffset;

            if (rowY > -45 && rowY < 269) {
                tft->fillRect(0, rowY, 240, 45, Theme::surface());
                tft->drawFastHLine(0, rowY, 240, Theme::divider());

                if (isDirectory[i]) {
                    tft->fillRoundRect(15, rowY + 12, 22, 16, 2, tft->color565(0, 122, 255));
                    tft->fillRoundRect(15, rowY + 9, 10, 5, 2, tft->color565(0, 122, 255));
                } else {
                    tft->fillRect(18, rowY + 10, 16, 20, tft->color565(142, 142, 147));
                    tft->fillRect(20, rowY + 12, 12, 16, Theme::surface());
                }

                String dName = fileNames[i];
                if (dName.length() > 18) dName = dName.substring(0, 15) + "...";

                tft->setTextColor(Theme::text());
                tft->setTextDatum(ML_DATUM);
                tft->drawString(dName, 48, rowY + 22);

                if (isDirectory[i]) {
                    tft->setTextColor(Theme::hint());
                    tft->setTextDatum(MR_DATUM);
                    tft->drawString(">", 225, rowY + 22);
                }
            }
        }
        int bottomY = (fileCount * 45) - scrollOffset;
        if (bottomY > 0 && bottomY < 269) {
            tft->drawFastHLine(0, bottomY, 240, Theme::divider());
        }
    }
    tft->resetViewport();
}

void FilesApp::update() {
    if (ts->touched()) {
        TS_Point p = ts->getPoint();
        int touchX = map(p.x, 300, 3800, 0, 240);
        int touchY = map(p.y, 300, 3800, 0, 320);

        if (!wasTouched) {
            wasTouched = true;
            touchStartY = touchY; 
            lastTouchY = touchY;

            
            if (touchY < 50 && touchX < 80 && currentPath != "/") {
                int lastSlash = currentPath.lastIndexOf('/');
                if (lastSlash > 0) {
                    currentPath = currentPath.substring(0, lastSlash); 
                } else {
                    currentPath = "/"; 
                }
                show();
            }
            else if (touchY > 50 && sdInitialized) {
                isScrollingList = true;
            }
        }

        if (isScrollingList) {
            int delta = lastTouchY - touchY;
            if (abs(delta) > 2) { 
                int maxScroll = (fileCount * 45) - 269; 
                if (maxScroll < 0) maxScroll = 0;

                int oldScroll = scrollOffset;
                scrollOffset += delta;
                
                if (scrollOffset < 0) scrollOffset = 0;
                if (scrollOffset > maxScroll) scrollOffset = maxScroll;

                if (oldScroll != scrollOffset) {
                    drawFileList(); 
                }
                lastTouchY = touchY;
            }
        }

    } else {
        if (wasTouched) {
            
            if (abs(lastTouchY - touchStartY) < 10 && touchStartY > 50 && sdInitialized) {
                int tappedRow = (touchStartY - 51 + scrollOffset) / 45;
                
                if (tappedRow >= 0 && tappedRow < fileCount) {
                    if (isDirectory[tappedRow]) {
                        if (currentPath == "/") {
                            currentPath = "/" + fileNames[tappedRow];
                        } else {
                            currentPath = currentPath + "/" + fileNames[tappedRow];
                        }
                        show();
                    }
                }
            }
        }

        wasTouched = false;
        isScrollingList = false; 
    }
}