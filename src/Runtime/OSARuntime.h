#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define OSA_MAX_LINES  256
#define OSA_MAX_VARS   64
#define OSA_MAX_FUNCS  16
#define OSA_STACK_MAX  8

// Permission bits (granted = bits 0-3, denied = bits 4-7 in Config)
#define OSA_PERM_NOTIFY   0x01   // notify()
#define OSA_PERM_SYSTEM   0x04   // setbright, setwallpaper

// ─── Value type ──────────────────────────────────────────────────────────────

struct OSAVal {
    bool   isNum = true;
    double num   = 0;
    String str;

    OSAVal() = default;
    OSAVal(double n)       : isNum(true),  num(n) {}
    OSAVal(const String& s): isNum(false), str(s) {}
    OSAVal(bool b)         : isNum(true),  num(b ? 1.0 : 0.0) {}

    bool   truthy()   const;
    String toString() const;
    double toNum()    const;
};

// ─── Runtime ─────────────────────────────────────────────────────────────────

class OSARuntime {
public:
    OSARuntime(TFT_eSPI* tft, XPT2046_Touchscreen* ts);

    bool   loadScript(const String& path); // loads .osa from SD
    void   runShow();                      // execute setup section
    bool   runUpdate();                    // execute loop body; false = exit
    bool   hasLoop()  const { return loopStart >= 0; }
    bool   hasError() const { return errLine >= 0; }
    String getError() const { return errMsg; }
    String appName;  // from #app "Name" header

private:
    TFT_eSPI*           tft;
    XPT2046_Touchscreen* ts;

    // Script storage
    String lines[OSA_MAX_LINES];
    int    lineCount = 0;
    int    loopStart = -1; // index of 'loop' line
    int    loopEnd   = -1; // index of matching 'end'

    // Variables
    struct Var { String name; OSAVal val; };
    Var vars[OSA_MAX_VARS];
    int varCount = 0;

    // User functions
    struct Func {
        String name;
        int    bodyStart; // first line after def header
        int    bodyEnd;   // matching end line
        String params[8];
        int    paramCount = 0;
    };
    Func funcs[OSA_MAX_FUNCS];
    int  funcCount = 0;

    // Call stack
    struct Frame { int retVarCount; };
    Frame callStack[OSA_STACK_MAX];
    int   stackDepth = 0;

    // Draw state
    uint16_t drawColor = TFT_WHITE;
    uint16_t txtColor  = TFT_WHITE;
    uint8_t  textFont  = 2;

    // Execution flags
    bool   exitFlag    = false;
    bool   returnFlag  = false;
    OSAVal returnValue;
    int    errLine     = -1;
    String errMsg;
    int    execDepth   = 0;

    // ── Execution ────────────────────────────────────────────────────────────
    void execRange(int from, int to);
    void execLine(int lineNo, int& pc);
    void processIf(int lineNo, int& pc);
    void processWhile(int lineNo, int& pc);
    void processFor(int lineNo, int& pc);

    // ── Helpers ──────────────────────────────────────────────────────────────
    int  findMatchingEnd(int lineNo);  // finds 'end' matching block at lineNo
    int  findNextBranch(int from);     // finds elif/else/end at depth 0

    void registerFuncs();              // pre-scan all 'def' blocks
    OSAVal callUser(const String& name, const String& argsStr);

    // ── Variables ────────────────────────────────────────────────────────────
    OSAVal getVar(const String& name) const;
    void   setVar(const String& name, const OSAVal& val);

    // ── Expression evaluator (recursive descent) ─────────────────────────────
    OSAVal eval(const String& expr);
    OSAVal evalOr     (const String& s, int& p);
    OSAVal evalAnd    (const String& s, int& p);
    OSAVal evalCompare(const String& s, int& p);
    OSAVal evalAddSub (const String& s, int& p);
    OSAVal evalMulDiv (const String& s, int& p);
    OSAVal evalUnary  (const String& s, int& p);
    OSAVal evalPrimary(const String& s, int& p);

    OSAVal callBuiltin(const String& name, const String& argsStr);
    void   splitArgs  (const String& argsStr, String* out, int& count);

    static void   skipWS     (const String& s, int& p);
    static bool   matchKw    (const String& s, int& p, const char* kw);
    static String peekIdent  (const String& s, int p);

    void setError(int line, const String& msg);

    // ── Permissions ──────────────────────────────────────────────────────────
    String permKey()  const;
    bool   checkPerm (uint8_t bit, const String& label, const String& detail);
    // Shared popup: returns true = right button, false = left button
    bool   showSystemPopup(const String& title, const String& body1, const String& body2,
                           const String& leftBtn, const String& rightBtn, bool rightDanger = false);
    bool   showPermPopup(const String& label, const String& detail);

    // ── Sandbox ───────────────────────────────────────────────────────────────
    String sandboxDir()  const;                    // /apps/<name>
    String sandboxPath(const String& rel) const;   // /apps/<name>/<safe-rel>
};
