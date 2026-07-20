#include "Home.h"
#include "../Config.h"
#include <new>

// '|' never appears in display names but survives a Config round-trip.
static const char  ORDER_SEP = '|';
static const char* ORDER_KEY = "home_order";
static const int   MAX_FOLDER_CHILDREN = 12;

// ─── HomeTile ────────────────────────────────────────────────────────────────
HomeTile::HomeTile(const HomeTile& other) {
    *this = other;
}

HomeTile& HomeTile::operator=(const HomeTile& other) {
    if (this == &other) return *this;

    // Allocate first so an OOM leaves the current tile intact.
    HomeTile* copiedChildren = nullptr;
    if (other.childCount > 0) {
        copiedChildren = new (std::nothrow) HomeTile[other.childCount];
        if (!copiedChildren) return *this;
        for (int i = 0; i < other.childCount; ++i) {
            copiedChildren[i] = other.children[i];
        }
    }

    freeChildren();
    name       = other.name;
    scriptPath = other.scriptPath;
    color      = other.color;
    isFolder   = other.isFolder;
    children   = copiedChildren;
    childCount = other.childCount;
    childCap   = other.childCount;
    return *this;
}

HomeTile::HomeTile(HomeTile&& other) noexcept {
    *this = static_cast<HomeTile&&>(other);
}

HomeTile& HomeTile::operator=(HomeTile&& other) noexcept {
    if (this == &other) return *this;
    freeChildren();
    name       = static_cast<String&&>(other.name);
    scriptPath = static_cast<String&&>(other.scriptPath);
    color      = other.color;
    isFolder   = other.isFolder;
    children   = other.children;
    childCount = other.childCount;
    childCap   = other.childCap;

    other.children   = nullptr;
    other.childCount = 0;
    other.childCap   = 0;
    other.isFolder   = false;
    return *this;
}

HomeTile::~HomeTile() {
    freeChildren();
}

bool HomeTile::addChild(const HomeTile& c) {
    if (childCount >= MAX_FOLDER_CHILDREN) return false;
    for (int i = 0; i < childCount; i++) {
        if (children[i].name == c.name && children[i].scriptPath == c.scriptPath) return false;
    }
    if (childCap == 0) {
        HomeTile* initial = new (std::nothrow) HomeTile[4];
        if (!initial) return false;
        children = initial;
        childCap = 4;
    } else if (childCount >= childCap) {
        int newCap = childCap * 2;
        if (newCap > MAX_FOLDER_CHILDREN) newCap = MAX_FOLDER_CHILDREN;
        HomeTile* nc = new (std::nothrow) HomeTile[newCap];
        if (!nc) return false;
        for (int i = 0; i < childCount; i++) {
            nc[i] = static_cast<HomeTile&&>(children[i]);
        }
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
    // Home is rescanned whenever it is reopened so apps copied to the SD card
    // become available without a reboot.  Do not add a second tile for an app
    // that is already on the grid or stored inside a folder.
    for (int i = 0; i < appCount; i++) {
        const HomeTile& existing = tiles[i];
        if (!existing.isFolder && existing.scriptPath == scriptPath) return;
        if (existing.isFolder) {
            for (int j = 0; j < existing.childCount; j++) {
                if (existing.children[j].scriptPath == scriptPath) return;
            }
        }
    }
    if (appCount >= MAX_APPS) return;
    HomeTile& t = tiles[appCount++];
    t.name       = displayName;
    t.scriptPath = scriptPath;
    t.color      = color;
    t.isFolder   = false;
}

static void removeHomeTileAt(HomeTile* tiles, int& count, int index) {
    if (index < 0 || index >= count) return;
    tiles[index].freeChildren();
    for (int i = index; i < count - 1; ++i) {
        tiles[i] = static_cast<HomeTile&&>(tiles[i + 1]);
    }
    tiles[count - 1] = HomeTile();
    --count;
}

static int removeMatchingScripts(HomeTile* tiles, int& count,
                                 const String& value, bool prefix) {
    int removed = 0;
    for (int i = count - 1; i >= 0; --i) {
        HomeTile& tile = tiles[i];
        if (tile.isFolder) {
            for (int j = tile.childCount - 1; j >= 0; --j) {
                const String& path = tile.children[j].scriptPath;
                bool match = prefix ? path.startsWith(value) : path == value;
                if (!match) continue;
                tile.children[j].freeChildren();
                for (int k = j; k < tile.childCount - 1; ++k)
                    tile.children[k] = static_cast<HomeTile&&>(tile.children[k + 1]);
                tile.children[tile.childCount - 1] = HomeTile();
                --tile.childCount;
                ++removed;
            }
            continue;
        }
        bool match = prefix ? tile.scriptPath.startsWith(value)
                            : tile.scriptPath == value;
        if (match) {
            removeHomeTileAt(tiles, count, i);
            ++removed;
        }
    }
    return removed;
}

int Home::removeScriptPath(const String& path) {
    int removed = removeMatchingScripts(tiles, appCount, path, false);
    if (removed > 0) saveOrder();
    return removed;
}

int Home::removeScriptsUnder(const String& pathPrefix) {
    int removed = removeMatchingScripts(tiles, appCount, pathPrefix, true);
    if (removed > 0) saveOrder();
    return removed;
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
                if (rCount >= MAX_APPS) break;
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
                                if (f.addChild(tiles[k])) consumed[k] = true;
                                break;
                            }
                        }
                    }
                }
                rCount++;
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
    for (int i = 0; i < rCount; i++) {
        tiles[i] = static_cast<HomeTile&&>(reordered[i]);
    }
    for (int i = rCount; i < MAX_APPS; i++) tiles[i] = HomeTile();
    appCount = rCount;
}
