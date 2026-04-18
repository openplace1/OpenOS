#include "TestKeyboard.h"
#include "Theme.h"

extern bool isSdReady;

TestKeyboard::TestKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts  = tsInstance;

    name      = "Notes";
    iconColor = tft->color565(255, 204, 0);
    isApp     = true;

    kbd             = new OSKeyboard(tft, ts);
    currentState    = STATE_LIST;
    noteCount       = 0;
    listScrollOffset = 0;
    listTouchStartY  = -1;
    wasTouched      = false;
    isScrolling     = false;
    currentFile     = "";
    textBuffer      = "";
}

TestKeyboard::~TestKeyboard() {
    delete kbd;
}

void TestKeyboard::show() {
    if (currentState == STATE_LIST) {
        loadNoteList();
        drawList();
    } else if (currentState == STATE_EDITOR) {
        drawEditor();
    } else if (currentState == STATE_DELETE_CONFIRM) {
        drawEditor();
        drawDeleteConfirm();
    }
}

// ── SD helpers ────────────────────────────────────────────────────────────────

void TestKeyboard::loadNoteList() {
    noteCount = 0;
    if (!isSdReady) return;
    if (!SD.exists("/user/notes")) SD.mkdir("/user/notes");

    File dir = SD.open("/user/notes");
    if (!dir) return;
    File f = dir.openNextFile();
    while (f && noteCount < MAX_NOTES) {
        String fname = f.name();
        int slash = fname.lastIndexOf('/');
        if (slash >= 0) fname = fname.substring(slash + 1);
        String lc = fname; lc.toLowerCase();
        if (lc.endsWith(".txt") && !f.isDirectory())
            notes[noteCount++] = fname;
        f = dir.openNextFile();
    }
    dir.close();
}

void TestKeyboard::loadNote(const String& filename) {
    textBuffer = "";
    if (!isSdReady) return;
    File f = SD.open("/user/notes/" + filename, FILE_READ);
    if (!f) return;
    while (f.available() && textBuffer.length() < 800)
        textBuffer += (char)f.read();
    f.close();
}

void TestKeyboard::saveNote() {
    if (!isSdReady || currentFile.length() == 0) return;
    if (!SD.exists("/user/notes")) SD.mkdir("/user/notes");
    SD.remove("/user/notes/" + currentFile);
    File f = SD.open("/user/notes/" + currentFile, FILE_WRITE);
    if (!f) return;
    f.print(textBuffer);
    f.close();
}

String TestKeyboard::nextNoteFilename() {
    if (!SD.exists("/user/notes")) SD.mkdir("/user/notes");
    for (int n = 1; n <= 99; n++) {
        String fname = "Note_" + String(n) + ".txt";
        if (!SD.exists("/user/notes/" + fname)) return fname;
    }
    return "Note_" + String(millis()) + ".txt";
}

// ── draw ──────────────────────────────────────────────────────────────────────

void TestKeyboard::drawHeader() {
    tft->fillRect(0, 0, 240, 52, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());
    tft->setTextFont(2); tft->setTextSize(1);
    if (currentState == STATE_EDITOR || currentState == STATE_DELETE_CONFIRM) {
        tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM);
        tft->drawString("< Notes", 8, 25);
        String dispName = currentFile;
        if (dispName.endsWith(".txt")) dispName = dispName.substring(0, dispName.length() - 4);
        dispName.replace("_", " ");
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString(dispName, 130, 25);
        tft->setTextColor(tft->color565(255, 59, 48)); tft->setTextDatum(MR_DATUM);
        tft->drawString("Del", 232, 25);
    } else {
        tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
        tft->drawString("Notes", 120, 25);
        tft->setTextFont(4);
        tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(MR_DATUM);
        tft->drawString("+", 228, 25);
    }
}

void TestKeyboard::drawList() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString("Notes", 120, 25);
    tft->setTextFont(4);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(MR_DATUM);
    tft->drawString("+", 228, 25);

    if (!isSdReady) {
        tft->setTextFont(2); tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
        tft->drawString("No SD card", 120, 180);
        return;
    }

    if (noteCount == 0) {
        tft->setTextFont(2); tft->setTextColor(Theme::hint()); tft->setTextDatum(MC_DATUM);
        tft->drawString("No notes yet", 120, 175);
        tft->drawString("Tap + to create one", 120, 198);
        return;
    }

    const int rowH = 50;
    tft->setTextFont(2); tft->setTextSize(1);
    for (int i = 0; i < noteCount; i++) {
        int y = 50 + i * rowH - listScrollOffset;
        if (y + rowH < 0 || y > 320) continue;
        tft->fillRect(0, y, 240, rowH, Theme::surface());
        tft->drawFastHLine(0, y + rowH, 240, Theme::divider2());

        String dispName = notes[i];
        if (dispName.endsWith(".txt")) dispName = dispName.substring(0, dispName.length() - 4);
        dispName.replace("_", " ");

        tft->setTextColor(Theme::text()); tft->setTextDatum(ML_DATUM);
        tft->drawString(dispName, 15, y + 25);
        tft->setTextColor(Theme::hint()); tft->setTextDatum(MR_DATUM);
        tft->drawString(">", 225, y + 25);
    }
}

void TestKeyboard::drawEditor() {
    tft->fillScreen(Theme::bg());
    tft->fillRect(0, 0, 240, 50, Theme::header());
    tft->drawFastHLine(0, 50, 240, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(tft->color565(0, 122, 255)); tft->setTextDatum(ML_DATUM);
    tft->drawString("< Notes", 8, 25);

    String dispName = currentFile;
    if (dispName.endsWith(".txt")) dispName = dispName.substring(0, dispName.length() - 4);
    dispName.replace("_", " ");
    tft->setTextColor(Theme::text()); tft->setTextDatum(MC_DATUM);
    tft->drawString(dispName, 130, 25);

    tft->setTextColor(tft->color565(255, 59, 48)); tft->setTextDatum(MR_DATUM);
    tft->drawString("Del", 232, 25);

    drawTextArea();
    kbd->draw();
}

void TestKeyboard::drawTextArea() {
    tft->fillRect(0, 50, 240, 110, Theme::surface());

    // Wrap text into lines (~28 chars or newline)
    String lines[24];
    int lineCount = 0;
    String cur = "";
    String buf = textBuffer + "_";

    for (int i = 0; i < (int)buf.length() && lineCount < 24; i++) {
        char c = buf[i];
        if (c == '\n') {
            lines[lineCount++] = cur; cur = "";
        } else if (cur.length() >= 28) {
            lines[lineCount++] = cur; cur = String(c);
        } else {
            cur += c;
        }
    }
    if (lineCount < 24) lines[lineCount++] = cur;

    // Always show the last 7 lines so cursor stays visible
    int startLine = (lineCount > 7) ? lineCount - 7 : 0;

    tft->setTextFont(2); tft->setTextSize(1);
    tft->setTextColor(Theme::text()); tft->setTextDatum(TL_DATUM);
    for (int i = startLine; i < lineCount; i++)
        tft->drawString(lines[i], 5, 54 + (i - startLine) * 16);
}

void TestKeyboard::drawDeleteConfirm() {
    for (int row = 0; row < 320; row += 2)
        tft->drawFastHLine(0, row, 240, tft->color565(0, 0, 0));

    int cx = 20, cy = 105, cw = 200, ch = 110;
    tft->fillRoundRect(cx, cy, cw, ch, 14, Theme::surface());
    tft->drawRoundRect(cx, cy, cw, ch, 14, Theme::divider());

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(Theme::text());
    tft->drawString("Delete note?", 120, cy + 22);

    String dispName = currentFile;
    if (dispName.endsWith(".txt")) dispName = dispName.substring(0, dispName.length() - 4);
    dispName.replace("_", " ");
    tft->setTextColor(Theme::subtext());
    tft->drawString(dispName, 120, cy + 46);
    tft->drawString("This cannot be undone.", 120, cy + 64);

    tft->drawFastHLine(cx, cy + 80, cw, Theme::divider2());
    tft->drawFastVLine(cx + cw / 2, cy + 80, ch - 80, Theme::divider2());

    tft->setTextColor(tft->color565(0, 122, 255));
    tft->drawString("Cancel", cx + cw / 4, cy + 95);
    tft->setTextColor(tft->color565(255, 59, 48));
    tft->drawString("Delete", cx + cw * 3 / 4, cy + 95);
}

// ── update ────────────────────────────────────────────────────────────────────

void TestKeyboard::update() {
    if (currentState == STATE_LIST) {
        if (ts->touched()) {
            TS_Point p = ts->getPoint();
            int touchX = map(p.x, 300, 3800, 0, 240);
            int touchY = map(p.y, 300, 3800, 0, 320);

            if (!wasTouched) {
                wasTouched  = true;
                listTouchStartY = touchY;
                isScrolling = false;
            } else if (abs(touchY - listTouchStartY) > 8) {
                isScrolling = true;
                int maxScroll = noteCount * 50 - 280; if (maxScroll < 0) maxScroll = 0;
                listScrollOffset += (listTouchStartY - touchY);
                if (listScrollOffset < 0) listScrollOffset = 0;
                if (listScrollOffset > maxScroll) listScrollOffset = maxScroll;
                listTouchStartY = touchY;
                drawList();
            }
        } else {
            if (wasTouched && !isScrolling) {
                TS_Point p = ts->getPoint();
                int touchX = map(p.x, 300, 3800, 0, 240);
                int touchY = map(p.y, 300, 3800, 0, 320);

                if (touchY < 50 && touchX > 190) {
                    // "+" — new note
                    if (isSdReady) {
                        currentFile = nextNoteFilename();
                        textBuffer  = "";
                        saveNote();
                        currentState = STATE_EDITOR;
                        show();
                    }
                } else if (touchY >= 50) {
                    int idx = (touchY - 50 + listScrollOffset) / 50;
                    if (idx >= 0 && idx < noteCount) {
                        currentFile = notes[idx];
                        loadNote(currentFile);
                        currentState = STATE_EDITOR;
                        show();
                    }
                }
            }
            wasTouched = false;
        }
    } else if (currentState == STATE_DELETE_CONFIRM) {
        if (ts->touched()) {
            TS_Point p = ts->getPoint();
            int touchX = map(p.x, 300, 3800, 0, 240);
            int touchY = map(p.y, 300, 3800, 0, 320);

            if (!wasTouched) {
                wasTouched = true;
                // Card: x=20-220, y=105-215; buttons y=185-215
                if (touchY > 185 && touchY < 215) {
                    if (touchX > 20 && touchX < 120) {
                        // Cancel
                        currentState = STATE_EDITOR;
                        show();
                    } else if (touchX > 120 && touchX < 220) {
                        // Delete
                        SD.remove("/user/notes/" + currentFile);
                        currentFile = "";
                        textBuffer  = "";
                        currentState = STATE_LIST;
                        listScrollOffset = 0;
                        show();
                    }
                }
            }
        } else {
            wasTouched = false;
        }
    } else {
        // Editor — intercept header tap before keyboard gets it
        if (ts->touched()) {
            TS_Point p = ts->getPoint();
            int touchX = map(p.x, 300, 3800, 0, 240);
            int touchY = map(p.y, 300, 3800, 0, 320);

            if (!wasTouched) {
                wasTouched = true;
                if (touchY < 50 && touchX < 95) {
                    saveNote();
                    currentState = STATE_LIST;
                    listScrollOffset = 0;
                    show();
                    return;
                } else if (touchY < 50 && touchX > 190) {
                    currentState = STATE_DELETE_CONFIRM;
                    drawDeleteConfirm();
                    return;
                }
            }
        } else {
            wasTouched = false;
        }

        char c = kbd->update();
        if (c != '\0') {
            if (c == '\b') {
                if (textBuffer.length() > 0) textBuffer.remove(textBuffer.length() - 1);
            } else if (textBuffer.length() < 800) {
                textBuffer += c;
            }
            drawTextArea();
        }
    }
}
