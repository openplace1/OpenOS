#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define OSA_MAX_LINES  512
#define OSA_MAX_VARS   96
#define OSA_MAX_FUNCS  24
#define OSA_STACK_MAX  10
#define OSA_MAX_SOURCE_BYTES (128 * 1024)
#define OSA_MAX_LINE_BYTES   4096

// ─── Bytecode VM limits ──────────────────────────────────────────────────────
#define OSA_BC_MAX       8192   // total bytecode bytes (opcodes + operands)
#define OSA_NUM_CONST    96     // numeric constant pool
#define OSA_STR_CONST    160    // string constant pool (Settings uses ~126)
#define OSA_NAME_CONST   192    // variables, functions and builtin names
#define OSA_STACK_SIZE   48     // operand stack
#define OSA_LOOP_DEPTH   8      // nested while/for/loop tracking

enum OSABytecodeBuildError : uint8_t {
    OSA_BCERR_NONE = 0,
    OSA_BCERR_CODE_FULL,
    OSA_BCERR_NUM_POOL_FULL,
    OSA_BCERR_STR_POOL_FULL,
    OSA_BCERR_NAME_POOL_FULL,
    OSA_BCERR_BAD_PATCH,
    OSA_BCERR_UNSUPPORTED,
};

// Bytecode opcodes — single byte each, operands packed inline (2-byte LE).
enum OSAOp : uint8_t {
    OP_NOP = 0,
    // Stack
    OP_PUSH_NUM,     // 2-byte: numPool[idx]
    OP_PUSH_STR,     // 2-byte: strPool[idx]
    OP_PUSH_NUM0,    // push 0
    OP_PUSH_NUM1,    // push 1
    OP_POP,
    OP_DUP,
    // Variables (operand = name pool idx)
    OP_LOAD_VAR,     // 2-byte name idx
    OP_STORE_VAR,    // 2-byte name idx (assignment — global lookup, may create)
    OP_DECLARE_VAR,  // 2-byte name idx (var X = … — local scope)
    // Arithmetic
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    // Comparison
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    // Logic
    OP_AND, OP_OR, OP_NOT,
    // Control flow (operand = signed 2-byte offset from end of operand)
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_JMP_IF_TRUE,
    // Calls (operand: name idx 2-byte, then argc 1-byte)
    OP_CALL_BUILTIN,
    OP_CALL_USER,
    OP_RET,           // return from user fn (pop = return value, 0 if none)
    OP_RET_VOID,
    // Loops
    OP_LOOP_TICK,     // top of main loop — yield + checkExitGesture
    // Lifecycle
    OP_EXIT,          // set exitFlag, halt
    OP_HALT,          // end of bytecode marker
};

// Compiled script — replaces line[]+parsing on every execLine.
struct OSABytecode {
    uint8_t  code[OSA_BC_MAX];
    int      codeLen = 0;

    double   numPool[OSA_NUM_CONST];
    int      numPoolLen = 0;

    String   strPool[OSA_STR_CONST];
    int      strPoolLen = 0;

    String   namePool[OSA_NAME_CONST];
    int      namePoolLen = 0;

    int      setupEnd  = 0;    // pc where setup section ends, loop body begins
    int      loopStart = -1;   // pc of OP_LOOP_TICK (top of main loop)
    int      loopEnd   = -1;   // pc just past OP_JMP that closes the loop
    bool     valid     = false;
    uint8_t  buildError = OSA_BCERR_NONE;

    void clear();
};

// Permission bits (granted = bits 0-3, denied = bits 4-7 in Config)
#define OSA_PERM_NOTIFY   0x01   // notify()
#define OSA_PERM_NETWORK  0x02   // http.get / http.post — outbound network access
#define OSA_PERM_SYSTEM   0x04   // setbright, setwallpaper
#define OSA_PERM_OVERLAY  0x08   // overlay.draw — draw on top of the active app
                                  //   (like Android "Draw over other apps")

// Permission descriptor — used by Settings to render only declared toggles.
struct OSAPermDesc {
    uint8_t     bit;
    const char* label;       // shown in Settings row title
    const char* description; // shown in Settings row subtitle
};
extern const OSAPermDesc OSA_PERM_TABLE[];
extern const int         OSA_PERM_TABLE_COUNT;

// ─── Value type ──────────────────────────────────────────────────────────────

struct OSAVal {
    bool   isNum = true;
    double num   = 0;
    String str;

    OSAVal() = default;
    OSAVal(double n)       : isNum(true),  num(n) {}
    OSAVal(const String& s): isNum(false), str(s) {}
    OSAVal(String&& s)     : isNum(false), str(static_cast<String&&>(s)) {}
    OSAVal(bool b)         : isNum(true),  num(b ? 1.0 : 0.0) {}

    bool   truthy()   const;
    String toString() const;
    double toNum()    const;
};

// ─── Runtime ─────────────────────────────────────────────────────────────────

class OSARuntime {
public:
    OSARuntime(TFT_eSPI* tft, XPT2046_Touchscreen* ts);
    ~OSARuntime();

    // Pass by value intentionally: reset() releases every String owned by the
    // runtime, so a caller may safely pass pendingLaunch/loadedScriptPath.
    bool   loadScript(String path); // loads .osa/.osac from SD
    void   runShow();                      // execute setup section
    bool   runUpdate();                    // execute loop body; false = exit
    // Release all script-owned heap (line text, var values, function bodies,
    // any allocated sprite). Call after the app exits — Arduino's String
    // destructor frees char buffers, so the next idle/launch starts clean.
    void   reset();
    bool   hasLoop()  const { return loopStart >= 0 || (bc.valid && bc.loopStart >= 0); }
    bool   hasError() const { return errLine >= 0; }
    String getError() const { return errMsg; }
    String  appName;        // from #app "Name" header
    uint8_t requiredPerms;  // bitmask from #perm header (0 = none declared)
    bool    isException;    // #exception true → script gets full privileged SDK
    // Set by app.launch(path) — host (main.cpp) reads this after the script
    // unwinds and, when non-empty, loads it instead of returning to home.
    String  pendingLaunch;
    // Set by checkOverlayGesture (swipe-down from top). Host opens Control
    // Center instead of going home when this is true on exit.
    bool    wantsOverlay = false;

    // Shared helpers used by Settings to keep perm-key derivation in sync.
    static String  permKeyForName(const String& appName);
    static String  permKeyForPath(const String& scriptPath);
    static String  readAppNameFromFile(const String& path);
    static uint8_t readRequiredPermsFromFile(const String& path);
    // Home-screen shortcut metadata. Scripts opt in with `#isApp true`
    // and can set a tile color with `#appColor "#FF9500"`.
    static bool     readIsAppFromFile(const String& path);
    static uint16_t readIconColorFromFile(const String& path, uint16_t fallback);

    // Allow OSAApp to inject an external sprite (pre-render before animation).
    // All drawing builtins (CV macro) redirect there when non-null.
    void         setActiveSprite(TFT_eSprite* s) { activeSprite = s; }
    TFT_eSprite* getActiveSprite() const          { return activeSprite; }

    // Bytecode compiler — these are exposed (read-only conceptually) so the
    // file-local static compile* helpers in OSARuntime.cpp can touch them
    // without friend boilerplate.
    OSABytecode bc;
    int  bcAddNum(double v);
    int  bcAddStr(const String& s);
    int  bcAddName(const String& s);
    uint8_t bytecodeError() const { return lastCompileError; }
    String*  bcLines()             { return lines; }
    int      bcLineCount() const   { return lineCount; }
    int      bcFindMatchingEnd(int n);
    int      bcFindNextBranch(int n);
    int      bcFuncCount() const   { return funcCount; }
    const String& bcFuncName(int i) const { return funcs[i].name; }
    // Full read access to the function table (.osac serializer needs it).
    int      bcFuncBcStart(int i)    const { return funcs[i].bcStart; }
    int      bcFuncParamCount(int i) const { return funcs[i].paramCount; }
    const String& bcFuncParam(int i, int j) const { return funcs[i].params[j]; }
    // Setters used during .osac deserialization.
    void     bcSetFuncCount(int n)             { funcCount = n; }
    void     bcSetFuncName(int i, const String& n) { funcs[i].name = n; }
    void     bcSetFuncBcStart(int i, int pc)   { funcs[i].bcStart = pc; }
    void     bcSetFuncParamCount(int i, int n) { funcs[i].paramCount = n; }
    void     bcSetFuncParam(int i, int j, const String& p) { funcs[i].params[j] = p; }
    // `#exception true` grants the script the privileged SDK (sys.*, cfg.*,
    // fs.* outside sandbox, ntp.sync, sys.reboot, …). Used by built-in
    // privileged apps that need to mutate global system state.
    static bool     readIsExceptionFromFile(const String& path);

private:
    TFT_eSPI*           tft;
    XPT2046_Touchscreen* ts;
    // Off-screen sprite for flicker-free animations. Allocated on demand by
    // gfx.begin(w, h); when non-null all drawing builtins redirect there.
    TFT_eSprite*        activeSprite = nullptr;
    // Stashed sprite — built once, blitted many times. Detached from
    // activeSprite via gfx.stash so that subsequent drawing builtins target
    // the screen, while gfx.show keeps blitting the stashed buffer.
    TFT_eSprite*        stashSprite  = nullptr;

    // Script storage
    String lines[OSA_MAX_LINES];
    int    lineCount = 0;
    String loadedScriptPath;
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
        int    bcStart = -1;   // pc where bytecode body starts (set by compile)
        String params[8];
        int    paramCount = 0;
    };
    Func funcs[OSA_MAX_FUNCS];
    int  funcCount = 0;

    // Call stack
    struct Frame { int retVarCount; };
    Frame callStack[OSA_STACK_MAX];
    int   stackDepth = 0;

    // VM operand stack (bc itself is public — compiler helpers in the .cpp
    // need to emit into it).
    OSAVal      vmStack[OSA_STACK_SIZE];
    int         vmSp = 0;
    bool        vmHalted = false;
    // Per-call frame for OP_CALL_USER. Records where to resume + the var
    // scope size to restore so locals declared by the callee go away on ret.
    struct VMFrame { int returnPc; int savedVarCount; };
    VMFrame     vmCallStack[OSA_STACK_MAX];
    int         vmCallDepth = 0;
    OSAVal*     directBuiltinArgs = nullptr;
    int         directBuiltinArgc = 0;

    // Draw state
    uint16_t drawColor = TFT_WHITE;
    uint16_t txtColor  = TFT_WHITE;
    uint8_t  textFont  = 2;

    // Execution flags
    bool   exitFlag     = false;
    bool   returnFlag   = false;
    bool   breakFlag    = false;
    bool   continueFlag = false;
    OSAVal returnValue;
    int    errLine      = -1;
    String errMsg;
    int    execDepth    = 0;
    uint8_t lastCompileError = OSA_BCERR_NONE;
    uint32_t scriptSliceStarted = 0;
    uint32_t scriptOpsSinceYield = 0;
    uint32_t scriptLoopOpsTotal = 0;
    // Tracks an in-progress swipe from the bottom edge — when the user lifts
    // their finger after dragging up far enough, exitFlag is set and the
    // script unwinds to OSAApp which signals main.cpp to go home.
    int    swipeHomeStartY = -1;

    // Tracks an in-progress swipe from the top edge for checkOverlayGesture.
    int    swipeOverlayStartY = -1;

    // ── Gesture state (refreshed on every touch.*/gesture.* call) ────────────
    int           gestureStartX  = 0;
    int           gestureStartY  = 0;
    int           gestureLastX   = 0;
    int           gestureLastY   = 0;
    unsigned long gestureStartT  = 0;
    bool          gestureActive  = false;
    bool          touchWasDown   = false;
    bool          releasedOneShot = false;
    int           swipeOneShot   = 0;  // 1=up,2=down,3=left,4=right, consumed on read
    uint32_t      touchSampleMs   = UINT32_MAX;
    bool          touchSampleDown = false;
    int           touchSampleX    = -1;
    int           touchSampleY    = -1;
    void sampleTouch();
    void pollGesture();

    // Returns true if the swipe-up-to-home gesture just completed. Called from
    // every blocking widget loop so the user can leave the app from anywhere.
    bool checkExitGesture();
    // Returns true if the swipe-down-from-top gesture just completed.
    bool checkOverlayGesture();

    // ── Bytecode VM ──────────────────────────────────────────────────────────
    // compile(): walks lines[], emits bytecode into `bc` (public).
    // exec(): interprets bc.code[pcStart..pcEnd].
    bool compile();
    bool exec(int pcStart, int pcEnd);
    // Pass by value so temporary String results can be moved onto the VM stack
    // without allocating a second equally large buffer. Lvalues still make the
    // one copy required by OP_LOAD_VAR / OP_DUP.
    bool vmPush(OSAVal v);
    OSAVal vmPop();
    // Binary .osac format — write the compiled state out / read it back.
    bool serializeOsac(const String& dstPath);
    bool loadOsac(const String& srcPath);

    // ── Execution (tree-walker fallback) ─────────────────────────────────────
    void execRange(int from, int to);
    void execLine(int lineNo, int& pc);
    void processIf(int lineNo, int& pc);
    void processWhile(int lineNo, int& pc);
    void processFor(int lineNo, int& pc);
    bool cooperativeTick(uint32_t interval = 128);

    // ── Helpers ──────────────────────────────────────────────────────────────
    int  findMatchingEnd(int lineNo);  // finds 'end' matching block at lineNo
    int  findNextBranch(int from);     // finds elif/else/end at depth 0

    void registerFuncs();              // pre-scan all 'def' blocks
    OSAVal callUser(const String& name, const String& argsStr);

    // ── Variables ────────────────────────────────────────────────────────────
    OSAVal getVar(const String& name) const;
    void   setVar(const String& name, OSAVal val);
    // `var X = Y` — declare in the CURRENT scope. Same-named vars in parent
    // scopes are shadowed, not overwritten. Plain assignment (X = Y) keeps the
    // global-search behaviour of setVar.
    void   declareVar(const String& name, OSAVal val);

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
    String packageAssetPath(const String& rel) const;
    String packageRoot;
};
