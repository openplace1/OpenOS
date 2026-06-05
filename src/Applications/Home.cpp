#include "Home.h"
#include "../Config.h"

// '|' never appears in display names but survives a Config round-trip.
static const char  ORDER_SEP = '|';
static const char* ORDER_KEY = "home_order";
static const int   MAX_FOLDER_CHILDREN = 12;

// ─── HomeTile ────────────────────────────────────────────────────────────────
bool HomeTile::addChild(const HomeTile& c) {
    if (childCount >= MAX_FOLDER_CHILDREN) return false;
    for (int i = 0; i < childCount; i++) {
        if (children[i].name == c.name && children[i].scriptPath == c.scriptPath) return false;
    }
    if (childCap == 0) {
        childCap = 4;
        children = new HomeTile[childCap];
    } else if (childCount >= childCap) {
        int newCap = childCap * 2;
        if (newCap > MAX_FOLDER_CHILDREN) newCap = MAX_FOLDER_CHILDREN;
        HomeTile* nc = new HomeTile[newCap];
        for (int i = 0; i < childCount; i++) nc[i] = children[i];
        delete[] children;
        children = nc;
        childCap = newCap;
    }
    children[childCount++] = c;
    return true;
}

void HomeTile::freeChildren() {
    if (children) {
        delete[] children;
        children = nullptr;
    }
    childCount = childCap = 0;
}

// ─── Home ────────────────────────────────────────────────────────────────────
Home::Home(TFT_eSPI* tftInstance, XPT2046_Touchscreen* tsInstance) {
    tft = tftInstance;
    ts  = tsInstance;
}

void Home::addScript(const String& scriptPath, const String& displayName, uint16_t color) {
    if (appCount >= MAX_APPS) return;
    HomeTile& t = tiles[appCount++];
    t.name       = displayName;
    t.scriptPath = scriptPath;
    t.color      = color;
    t.isFolder   = false;
}

void Home::saveOrder() {
    String order;
    for (int i = 0; i < appCount; i++) {
        if (i > 0) order += ORDER_SEP;
        const HomeTile& t = tiles[i];
        if (t.isFolder) {
            order += "folder:";
            order += t.name;
            order += "(";
            for (int j = 0; j < t.childCount; j++) {
                if (j > 0) order += ",";
                order += t.children[j].name;
            }
            order += ")";
        } else {
            order += t.name;
        }
    }
    Config::set(ORDER_KEY, order);
    Config::save();
}

void Home::applyOrder() {
    String order = Config::get(ORDER_KEY, "");
    if (order.length() == 0) return;

    HomeTile reordered[MAX_APPS];
    int      rCount = 0;
    bool     consumed[MAX_APPS] = {false};

    int start = 0;
    for (int i = 0; i <= (int)order.length(); i++) {
        if (i == (int)order.length() || order[i] == ORDER_SEP) {
            String token = order.substring(start, i);
            start = i + 1;
            if (token.length() == 0) continue;

            if (token.startsWith("folder:")) {
                int openParen  = token.indexOf('(');
                int closeParen = token.lastIndexOf(')');
                if (openParen < 0 || closeParen <= openParen) continue;
                HomeTile& f = reordered[rCount];
                f.name     = token.substring(7, openParen);
                f.color    = tft->color565(70, 70, 86);
                f.isFolder = true;
                String innerList = token.substring(openParen + 1, closeParen);
                int innerStart = 0;
                for (int j = 0; j <= (int)innerList.length(); j++) {
                    if (j == (int)innerList.length() || innerList[j] == ',') {
                        String childName = innerList.substring(innerStart, j);
                        innerStart = j + 1;
                        if (childName.length() == 0) continue;
                        for (int k = 0; k < appCount; k++) {
                            if (!consumed[k] && tiles[k].name == childName) {
                                f.addChild(tiles[k]);
                                consumed[k] = true;
                                break;
                            }
                        }
                    }
                }
                if (rCount < MAX_APPS) rCount++;
            } else {
                for (int j = 0; j < appCount; j++) {
                    if (!consumed[j] && tiles[j].name == token) {
                        if (rCount < MAX_APPS) reordered[rCount++] = tiles[j];
                        consumed[j] = true;
                        break;
                    }
                }
            }
        }
    }

    // Anything new (newly installed app not in saved order) sticks at the end.
    for (int i = 0; i < appCount; i++) {
        if (!consumed[i] && rCount < MAX_APPS) reordered[rCount++] = tiles[i];
    }

    // Release any folders we're throwing away.
    for (int i = 0; i < appCount; i++) tiles[i].freeChildren();
    for (int i = 0; i < rCount; i++) tiles[i] = reordered[i];
    for (int i = rCount; i < MAX_APPS; i++) tiles[i] = HomeTile();
    appCount = rCount;
}
