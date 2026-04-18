#ifndef TESTKEYBOARD_H
#define TESTKEYBOARD_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SD.h>
#include "App.h"
#include "OSKeyboard.h"

class TestKeyboard : public App {
private:
    enum NotesState { STATE_LIST, STATE_EDITOR, STATE_DELETE_CONFIRM };

    static const int MAX_NOTES = 20;

    OSKeyboard* kbd;
    NotesState currentState;

    String notes[MAX_NOTES];
    int noteCount;
    int listScrollOffset;
    int listTouchStartY;
    bool wasTouched;
    bool isScrolling;

    String currentFile;
    String textBuffer;

    void loadNoteList();
    void loadNote(const String& filename);
    void saveNote();
    String nextNoteFilename();

    void drawList();
    void drawEditor();
    void drawTextArea();
    void drawDeleteConfirm();

public:
    TestKeyboard(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance);
    ~TestKeyboard() override;

    void show() override;
    void update() override;
    void drawHeader() override;
};

#endif
