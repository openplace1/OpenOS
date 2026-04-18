#include "OSARuntime.h"
#include "../Config.h"
#include <SD.h>
#include <WiFi.h>
#include <math.h>

extern bool isSdReady;
extern int  sysTheme;
extern int  sysBrightness;

// Forward-declare notification push
namespace _osa_notify { void push(const char* msg); }

// ═══════════════════════════════════════════════════════════════════════════════
// OSAVal helpers
// ═══════════════════════════════════════════════════════════════════════════════

static String numToStr(double v) {
    if (v == (long long)v && fabs(v) < 1e15) {
        return String((long long)v);
    }
    char buf[32]; dtostrf(v, 0, 6, buf);
    String s(buf); s.trim();
    if (s.indexOf('.') >= 0) {
        while (s.endsWith("0")) s.remove(s.length() - 1);
        if (s.endsWith(".")) s.remove(s.length() - 1);
    }
    return s;
}

bool   OSAVal::truthy()   const { return isNum ? (num != 0.0) : (str.length() > 0); }
String OSAVal::toString() const { return isNum ? numToStr(num) : str; }
double OSAVal::toNum()    const { return isNum ? num : str.toDouble(); }

// ═══════════════════════════════════════════════════════════════════════════════
// Constructor / reset
// ═══════════════════════════════════════════════════════════════════════════════

OSARuntime::OSARuntime(TFT_eSPI* t, XPT2046_Touchscreen* ts_)
    : tft(t), ts(ts_) {}

void OSARuntime::setError(int line, const String& msg) {
    if (errLine >= 0) return; // keep first error
    errLine = line;
    errMsg  = msg;
    exitFlag = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Script loading
// ═══════════════════════════════════════════════════════════════════════════════

bool OSARuntime::loadScript(const String& path) {
    // Reset state
    lineCount = varCount = funcCount = stackDepth = 0;
    loopStart = loopEnd = -1;
    exitFlag = returnFlag = false;
    errLine = -1; errMsg = "";
    execDepth = 0;
    appName = "";
    drawColor = txtColor = TFT_WHITE;
    textFont = 2;

    if (!isSdReady) { setError(0, "No SD card"); return false; }
    File f = SD.open(path);
    if (!f) { setError(0, "Not found: " + path); return false; }

    while (f.available() && lineCount < OSA_MAX_LINES) {
        String raw = f.readStringUntil('\n');
        raw.trim();
        // Parse #app "Name" header
        if (raw.startsWith("#app ")) {
            String nm = raw.substring(5); nm.trim();
            if (nm.startsWith("\"")) nm = nm.substring(1);
            if (nm.endsWith("\""))   nm = nm.substring(0, nm.length() - 1);
            appName = nm;
        }
        lines[lineCount++] = raw;
    }
    f.close();

    // Find loop section
    for (int i = 0; i < lineCount && loopStart < 0; i++) {
        if (lines[i] == "loop") loopStart = i;
    }
    if (loopStart >= 0) loopEnd = findMatchingEnd(loopStart);

    // Pre-register all user functions
    registerFuncs();

    // Create per-app sandbox directory
    if (isSdReady) {
        SD.mkdir("/apps");
        SD.mkdir(sandboxDir().c_str());
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// run entry points
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::runShow() {
    exitFlag = returnFlag = false;
    execDepth = 0;
    int to = (loopStart >= 0) ? loopStart : lineCount;
    execRange(0, to);
}

bool OSARuntime::runUpdate() {
    if (exitFlag || errLine >= 0) return false;
    if (loopStart < 0) return false;
    returnFlag = false;
    execDepth = 0;
    execRange(loopStart + 1, loopEnd);
    return !(exitFlag || errLine >= 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Block navigation helpers
// ═══════════════════════════════════════════════════════════════════════════════

static bool isBlockOpen(const String& t) {
    return t.startsWith("if ") || t.startsWith("while ") ||
           t.startsWith("for ") || t.startsWith("def ") || t == "loop";
}

int OSARuntime::findMatchingEnd(int lineNo) {
    int depth = 1;
    for (int i = lineNo + 1; i < lineCount; i++) {
        const String& t = lines[i];
        if (isBlockOpen(t)) depth++;
        else if (t == "end") { if (--depth == 0) return i; }
    }
    return lineCount;
}

int OSARuntime::findNextBranch(int from) {
    int depth = 0;
    for (int i = from; i < lineCount; i++) {
        const String& t = lines[i];
        if (isBlockOpen(t)) {
            depth++;
        } else if (t == "end") {
            if (depth == 0) return i;
            depth--;
        } else if (depth == 0 && (t.startsWith("elif ") || t == "else")) {
            return i;
        }
    }
    return lineCount;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Function registration
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::registerFuncs() {
    for (int i = 0; i < lineCount; i++) {
        String t = lines[i]; t.trim();
        if (!t.startsWith("def ")) continue;
        if (funcCount >= OSA_MAX_FUNCS) break;

        int parenO = t.indexOf('(');
        int parenC = t.lastIndexOf(')');
        if (parenO < 0 || parenC < 0) continue;

        Func& fn = funcs[funcCount++];
        fn.name = t.substring(4, parenO); fn.name.trim();
        fn.bodyStart = i + 1;
        fn.bodyEnd   = findMatchingEnd(i);
        fn.paramCount = 0;

        String pStr = t.substring(parenO + 1, parenC); pStr.trim();
        if (pStr.length() > 0) {
            int s = 0;
            for (int k = 0; k <= (int)pStr.length(); k++) {
                if (k == (int)pStr.length() || pStr[k] == ',') {
                    String p = pStr.substring(s, k); p.trim();
                    if (p.length() > 0 && fn.paramCount < 8)
                        fn.params[fn.paramCount++] = p;
                    s = k + 1;
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Execution engine
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::execRange(int from, int to) {
    if (execDepth > 24) { setError(from, "Nesting too deep"); return; }
    execDepth++;
    int pc = from;
    while (pc < to && !exitFlag && !returnFlag && errLine < 0) {
        yield(); // feed WDT
        execLine(pc, pc);
    }
    execDepth--;
}

void OSARuntime::execLine(int lineNo, int& pc) {
    const String& raw = lines[lineNo];

    // Empty / comment / def (already registered, skip body)
    if (raw.length() == 0 || raw[0] == '#') { pc++; return; }
    if (raw.startsWith("def ")) { pc = findMatchingEnd(lineNo) + 1; return; }
    if (raw == "end" || raw == "else" || raw.startsWith("elif ")) { pc++; return; }
    if (raw == "loop") { pc = findMatchingEnd(lineNo) + 1; return; }

    // ── Control flow ─────────────────────────────────────────────────────────

    if (raw.startsWith("if ")) { processIf(lineNo, pc); return; }
    if (raw.startsWith("while ")) { processWhile(lineNo, pc); return; }
    if (raw.startsWith("for ")) { processFor(lineNo, pc); return; }

    // return
    if (raw == "return") {
        returnValue = OSAVal(); returnFlag = true; pc++; return;
    }
    if (raw.startsWith("return ")) {
        String expr = raw.substring(7); expr.trim();
        returnValue = eval(expr); returnFlag = true; pc++; return;
    }

    // ── Variable declaration: var NAME = EXPR ────────────────────────────────
    if (raw.startsWith("var ")) {
        String rest = raw.substring(4); rest.trim();
        int eq = rest.indexOf('=');
        if (eq > 0) {
            String nm = rest.substring(0, eq); nm.trim();
            String ex = rest.substring(eq + 1); ex.trim();
            setVar(nm, eval(ex));
        }
        pc++; return;
    }

    // ── Assignment: NAME = EXPR (careful not to match ==) ────────────────────
    {
        int eq = -1;
        for (int i = 0; i < (int)raw.length(); i++) {
            if (raw[i] == '=') {
                bool prevOk = (i == 0 || (raw[i-1] != '!' && raw[i-1] != '<' &&
                                          raw[i-1] != '>' && raw[i-1] != '='));
                bool nextOk = (i+1 >= (int)raw.length() || raw[i+1] != '=');
                if (prevOk && nextOk) { eq = i; break; }
            }
        }
        if (eq > 0) {
            String nm = raw.substring(0, eq); nm.trim();
            // Validate: must be a plain identifier
            bool valid = (nm.length() > 0);
            for (int i = 0; i < (int)nm.length() && valid; i++)
                if (!isalnum(nm[i]) && nm[i] != '_') valid = false;
            if (valid) {
                String ex = raw.substring(eq + 1); ex.trim();
                setVar(nm, eval(ex));
                pc++; return;
            }
        }
    }

    // ── Anything else: evaluate as expression (function call, etc.) ───────────
    eval(raw);
    pc++;
}

// ── if / elif / else ─────────────────────────────────────────────────────────

void OSARuntime::processIf(int lineNo, int& pc) {
    int curLine = lineNo;
    // Iterative elif chain — no recursion
    while (!exitFlag && !returnFlag && errLine < 0) {
        const String& raw = lines[curLine];

        // Extract condition
        String cond = raw.startsWith("elif ") ? raw.substring(5) : raw.substring(3);
        if (cond.endsWith(" then")) cond = cond.substring(0, cond.length() - 5);
        cond.trim();

        int nextBranch = findNextBranch(curLine + 1);
        int finalEnd   = findMatchingEnd(curLine);

        if (eval(cond).truthy()) {
            execRange(curLine + 1, nextBranch);
            pc = finalEnd + 1;
            return;
        }

        if (nextBranch >= lineCount) { pc = lineCount; return; }
        const String& bt = lines[nextBranch];

        if (bt.startsWith("elif ")) {
            curLine = nextBranch; // iterate rather than recurse
        } else if (bt == "else") {
            int elseEnd = findNextBranch(nextBranch + 1);
            execRange(nextBranch + 1, elseEnd);
            pc = elseEnd + 1;
            return;
        } else {
            pc = nextBranch + 1;
            return;
        }
    }
}

// ── while ────────────────────────────────────────────────────────────────────

void OSARuntime::processWhile(int lineNo, int& pc) {
    const String& raw = lines[lineNo];
    String cond = raw.substring(6); // remove "while "
    if (cond.endsWith(" do")) cond = cond.substring(0, cond.length() - 3);
    cond.trim();

    int matchEnd = findMatchingEnd(lineNo);
    while (eval(cond).truthy() && !exitFlag && !returnFlag && errLine < 0)
        execRange(lineNo + 1, matchEnd);
    pc = matchEnd + 1;
}

// ── for i in start..end do ───────────────────────────────────────────────────

void OSARuntime::processFor(int lineNo, int& pc) {
    const String& raw = lines[lineNo];
    // "for VAR in START..END do"
    String rest = raw.substring(4); rest.trim();
    int inPos = rest.indexOf(" in ");
    if (inPos < 0) { pc++; return; }
    String varNm  = rest.substring(0, inPos); varNm.trim();
    String rangeS = rest.substring(inPos + 4);
    if (rangeS.endsWith(" do")) rangeS = rangeS.substring(0, rangeS.length() - 3);
    rangeS.trim();

    int dotdot = rangeS.indexOf("..");
    if (dotdot < 0) { pc++; return; }
    int startV = (int)eval(rangeS.substring(0, dotdot)).toNum();
    int endV   = (int)eval(rangeS.substring(dotdot + 2)).toNum();

    int matchEnd = findMatchingEnd(lineNo);
    for (int i = startV; i <= endV && !exitFlag && !returnFlag && errLine < 0; i++) {
        setVar(varNm, OSAVal((double)i));
        execRange(lineNo + 1, matchEnd);
    }
    pc = matchEnd + 1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Variable store
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::getVar(const String& name) const {
    for (int i = varCount - 1; i >= 0; i--)
        if (vars[i].name == name) return vars[i].val;
    return OSAVal(); // default 0
}

void OSARuntime::setVar(const String& name, const OSAVal& val) {
    for (int i = varCount - 1; i >= 0; i--) {
        if (vars[i].name == name) { vars[i].val = val; return; }
    }
    if (varCount < OSA_MAX_VARS) {
        vars[varCount].name = name;
        vars[varCount].val  = val;
        varCount++;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// User function call
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::callUser(const String& name, const String& argsStr) {
    // Find function
    int fi = -1;
    for (int i = 0; i < funcCount; i++)
        if (funcs[i].name == name) { fi = i; break; }
    if (fi < 0) { setError(-1, "Undefined: " + name); return OSAVal(); }

    // Parse + evaluate arguments
    String argTokens[6]; int argc = 0;
    splitArgs(argsStr, argTokens, argc);

    if (stackDepth >= OSA_STACK_MAX) { setError(-1, "Stack overflow"); return OSAVal(); }

    // Save scope
    int savedVarCount = varCount;
    callStack[stackDepth++] = { savedVarCount };

    // Bind parameters
    for (int i = 0; i < funcs[fi].paramCount && i < argc; i++)
        setVar(funcs[fi].params[i], eval(argTokens[i]));

    // Execute body
    returnFlag = false;
    returnValue = OSAVal();
    execRange(funcs[fi].bodyStart, funcs[fi].bodyEnd);

    // Restore scope
    stackDepth--;
    varCount = savedVarCount;
    returnFlag = false;
    return returnValue;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Argument splitter (respects strings and nested parens)
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::splitArgs(const String& s, String* out, int& count) {
    count = 0;
    int depth = 0; bool inStr = false; int start = 0;
    for (int i = 0; i <= (int)s.length(); i++) {
        char c = (i < (int)s.length()) ? s[i] : ',';
        if (inStr) {
            if (c == '\\') i++;
            else if (c == '"') inStr = false;
        } else if (c == '"') {
            inStr = true;
        } else if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            depth--;
        } else if (c == ',' && depth == 0) {
            if (count < 6) {
                out[count] = s.substring(start, i);
                out[count].trim();
                count++;
            }
            start = i + 1;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Expression evaluator — recursive descent
// ═══════════════════════════════════════════════════════════════════════════════

void OSARuntime::skipWS(const String& s, int& p) {
    while (p < (int)s.length() && s[p] == ' ') p++;
}

bool OSARuntime::matchKw(const String& s, int& p, const char* kw) {
    int klen = strlen(kw);
    if (p + klen > (int)s.length()) return false;
    if (strncmp(s.c_str() + p, kw, klen) != 0) return false;
    // Keyword must not be followed by alphanumeric
    if (p + klen < (int)s.length() && (isalnum(s[p + klen]) || s[p + klen] == '_'))
        return false;
    p += klen;
    return true;
}

OSAVal OSARuntime::eval(const String& expr) {
    String e = expr; e.trim();
    int p = 0;
    return evalOr(e, p);
}

OSAVal OSARuntime::evalOr(const String& s, int& p) {
    OSAVal left = evalAnd(s, p);
    while (true) {
        skipWS(s, p);
        if (matchKw(s, p, "or")) {
            OSAVal right = evalAnd(s, p);
            left = OSAVal(left.truthy() || right.truthy());
        } else break;
    }
    return left;
}

OSAVal OSARuntime::evalAnd(const String& s, int& p) {
    OSAVal left = evalCompare(s, p);
    while (true) {
        skipWS(s, p);
        if (matchKw(s, p, "and")) {
            OSAVal right = evalCompare(s, p);
            left = OSAVal(left.truthy() && right.truthy());
        } else break;
    }
    return left;
}

OSAVal OSARuntime::evalCompare(const String& s, int& p) {
    OSAVal left = evalAddSub(s, p);
    skipWS(s, p);

    String op;
    if (p + 2 <= (int)s.length()) {
        String two = s.substring(p, p + 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">=")
            { op = two; p += 2; }
    }
    if (op.length() == 0 && p < (int)s.length()) {
        char c = s[p];
        if (c == '<' || c == '>') { op = String(c); p++; }
    }
    if (op.length() == 0) return left;

    skipWS(s, p);
    OSAVal right = evalAddSub(s, p);

    bool result;
    if (!left.isNum || !right.isNum) {
        int cmp = left.toString().compareTo(right.toString());
        if      (op == "==") result = cmp == 0;
        else if (op == "!=") result = cmp != 0;
        else if (op == "<")  result = cmp <  0;
        else if (op == ">")  result = cmp >  0;
        else if (op == "<=") result = cmp <= 0;
        else                 result = cmp >= 0;
    } else {
        double l = left.num, r = right.num;
        if      (op == "==") result = l == r;
        else if (op == "!=") result = l != r;
        else if (op == "<")  result = l <  r;
        else if (op == ">")  result = l >  r;
        else if (op == "<=") result = l <= r;
        else                 result = l >= r;
    }
    return OSAVal(result);
}

OSAVal OSARuntime::evalAddSub(const String& s, int& p) {
    OSAVal left = evalMulDiv(s, p);
    while (true) {
        skipWS(s, p);
        if (p >= (int)s.length()) break;
        char op = s[p];
        if (op != '+' && op != '-') break;
        p++;
        skipWS(s, p);
        OSAVal right = evalMulDiv(s, p);
        if (op == '+') {
            if (!left.isNum || !right.isNum)
                left = OSAVal(left.toString() + right.toString());
            else
                left = OSAVal(left.num + right.num);
        } else {
            left = OSAVal(left.toNum() - right.toNum());
        }
    }
    return left;
}

OSAVal OSARuntime::evalMulDiv(const String& s, int& p) {
    OSAVal left = evalUnary(s, p);
    while (true) {
        skipWS(s, p);
        if (p >= (int)s.length()) break;
        char op = s[p];
        if (op != '*' && op != '/' && op != '%') break;
        p++;
        skipWS(s, p);
        OSAVal right = evalUnary(s, p);
        if (op == '*') left = OSAVal(left.toNum() * right.toNum());
        else if (op == '/') {
            double d = right.toNum();
            left = OSAVal(d != 0 ? left.toNum() / d : 0.0);
        } else {
            left = OSAVal(fmod(left.toNum(), right.toNum()));
        }
    }
    return left;
}

OSAVal OSARuntime::evalUnary(const String& s, int& p) {
    skipWS(s, p);
    if (p < (int)s.length() && s[p] == '-') {
        p++; return OSAVal(-evalUnary(s, p).toNum());
    }
    if (matchKw(s, p, "not")) {
        return OSAVal(!evalUnary(s, p).truthy());
    }
    return evalPrimary(s, p);
}

OSAVal OSARuntime::evalPrimary(const String& s, int& p) {
    skipWS(s, p);
    if (p >= (int)s.length()) return OSAVal();

    // Grouped
    if (s[p] == '(') {
        p++;
        OSAVal v = evalOr(s, p);
        skipWS(s, p);
        if (p < (int)s.length() && s[p] == ')') p++;
        return v;
    }

    // String literal
    if (s[p] == '"') {
        p++;
        String str;
        while (p < (int)s.length() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < (int)s.length()) {
                p++;
                char e = s[p];
                if (e == 'n') str += '\n';
                else if (e == 't') str += '\t';
                else str += e;
            } else {
                str += s[p];
            }
            p++;
        }
        if (p < (int)s.length()) p++; // closing "
        return OSAVal(str);
    }

    // Number literal
    if (isdigit(s[p]) || (s[p] == '.' && p + 1 < (int)s.length() && isdigit(s[p+1]))) {
        double val = 0; bool dot = false; double frac = 0.1;
        while (p < (int)s.length() && (isdigit(s[p]) || (s[p] == '.' && !dot))) {
            if (s[p] == '.') { dot = true; }
            else if (!dot)   { val = val * 10 + (s[p] - '0'); }
            else             { val += (s[p] - '0') * frac; frac *= 0.1; }
            p++;
        }
        return OSAVal(val);
    }

    // Boolean literals
    if (matchKw(s, p, "true"))  return OSAVal(1.0);
    if (matchKw(s, p, "false")) return OSAVal(0.0);

    // Identifier, function call, or method call
    if (isalpha(s[p]) || s[p] == '_') {
        String name;
        while (p < (int)s.length() && (isalnum(s[p]) || s[p] == '_' || s[p] == '.'))
            name += s[p++];

        skipWS(s, p);
        if (p < (int)s.length() && s[p] == '(') {
            // Function / method call — collect arguments respecting nested parens and strings
            p++; // skip (
            int depth = 1, start = p;
            bool inStr = false;
            while (p < (int)s.length() && depth > 0) {
                char c = s[p];
                if (inStr) {
                    if (c == '\\') p++;
                    else if (c == '"') inStr = false;
                } else if (c == '"') { inStr = true; }
                else if (c == '(') depth++;
                else if (c == ')') { if (--depth == 0) break; }
                p++;
            }
            String argsStr = s.substring(start, p);
            if (p < (int)s.length()) p++; // skip )
            return callBuiltin(name, argsStr);
        }

        // Variable lookup
        return getVar(name);
    }

    p++; // consume unknown char
    return OSAVal();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Permissions
// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════
// Sandbox
// ═══════════════════════════════════════════════════════════════════════════════

String OSARuntime::sandboxDir() const {
    String dir = "/apps/";
    for (int i = 0; i < (int)appName.length() && dir.length() < 28; i++) {
        char c = appName[i];
        dir += isalnum(c) ? (char)tolower(c) : '_';
    }
    return dir;
}

String OSARuntime::sandboxPath(const String& rel) const {
    // Strip any path-traversal attempts, then join with sandbox dir
    String safe = rel;
    safe.replace("../", "");
    safe.replace("./",  "");
    safe.replace("..",  "");
    while (safe.startsWith("/")) safe.remove(0, 1);
    return sandboxDir() + "/" + safe;
}

// ─────────────────────────────────────────────────────────────────────────────

String OSARuntime::permKey() const {
    String k = "perm_";
    for (int i = 0; i < (int)appName.length() && k.length() < 22; i++) {
        char c = appName[i];
        k += isalnum(c) ? (char)tolower(c) : '_';
    }
    return k;
}

bool OSARuntime::checkPerm(uint8_t bit, const String& label, const String& detail) {
    String key    = permKey();
    int    stored = Config::getInt(key, 0);
    uint8_t granted = stored & 0x0F;
    uint8_t denied  = (stored >> 4) & 0x0F;

    if (granted & bit) return true;
    if (denied  & bit) return false;

    bool allow = showPermPopup(label, detail);

    if (allow) stored |=  (int)bit;
    else       stored |= ((int)bit << 4);
    Config::setInt(key, stored);
    Config::save();
    return allow;
}

bool OSARuntime::showSystemPopup(const String& title, const String& body1, const String& body2,
                                  const String& leftBtn, const String& rightBtn, bool rightDanger) {
    // Dim overlay — same style as Settings confirm dialogs
    for (int row = 0; row < 320; row += 2)
        tft->drawFastHLine(0, row, 240, tft->color565(0, 0, 0));

    const int cx = 20, cy = 88, cw = 200, cr = 14;
    const int bodyLines = (body2.length() > 0) ? 2 : 1;
    const int ch = 56 + bodyLines * 18 + 52; // title+padding + body + buttons

    tft->fillRoundRect(cx, cy, cw, ch, cr, TFT_WHITE);
    tft->drawRoundRect(cx, cy, cw, ch, cr, tft->color565(220, 220, 222));

    tft->setTextFont(2); tft->setTextSize(1); tft->setTextDatum(MC_DATUM);
    tft->setTextColor(tft->color565(20, 20, 22));
    tft->drawString(title, 120, cy + 24);

    tft->setTextFont(1);
    tft->setTextColor(tft->color565(110, 110, 118));
    tft->drawString(body1, 120, cy + 46);
    if (body2.length() > 0) tft->drawString(body2, 120, cy + 62);

    const int divY = cy + ch - 44;
    tft->drawFastHLine(cx, divY, cw, tft->color565(218, 218, 222));
    tft->drawFastVLine(cx + cw / 2, divY, 44, tft->color565(218, 218, 222));

    tft->setTextFont(2);
    tft->setTextColor(tft->color565(0, 122, 255));
    tft->drawString(leftBtn,  cx + cw / 4,     cy + ch - 22);
    uint16_t rCol = rightDanger ? tft->color565(255, 59, 48) : tft->color565(52, 199, 89);
    tft->setTextColor(rCol);
    tft->drawString(rightBtn, cx + cw * 3 / 4, cy + ch - 22);

    delay(180);
    while (true) {
        yield();
        if (!ts->touched()) continue;
        TS_Point p = ts->getPoint();
        int tx = map(p.x, 300, 3800, 0, 240);
        int ty = map(p.y, 300, 3800, 0, 320);
        if (ty < divY || ty > cy + ch || tx < cx || tx > cx + cw) continue;
        bool right = (tx > cx + cw / 2);
        while (ts->touched()) yield();
        delay(60);
        return right;
    }
}

bool OSARuntime::showPermPopup(const String& label, const String& detail) {
    String title = "\"" + appName + "\" wants to";
    return showSystemPopup(title, label, detail, "Deny", "Allow", false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Built-in functions
// ═══════════════════════════════════════════════════════════════════════════════

OSAVal OSARuntime::callBuiltin(const String& name, const String& argsStr) {
    // Check user functions first — avoids allocating arg arrays on the stack
    for (int i = 0; i < funcCount; i++) {
        if (funcs[i].name == name) return callUser(name, argsStr);
    }

    String argT[6]; int argc = 0;
    splitArgs(argsStr, argT, argc);

    // Evaluate arguments eagerly
    OSAVal a[6];
    for (int i = 0; i < argc; i++) a[i] = eval(argT[i]);

    auto N = [&](int i, double def = 0) -> double {
        return (i < argc) ? a[i].toNum() : def;
    };
    auto S = [&](int i, const String& def = "") -> String {
        return (i < argc) ? a[i].toString() : def;
    };
    auto iN = [&](int i, int def = 0) -> int { return (int)N(i, def); };

    // ── Screen drawing ────────────────────────────────────────────────────────

    if (name == "clear" || name == "cls") {
        uint16_t bg = (sysTheme == 1) ? tft->color565(28, 28, 30) : TFT_WHITE;
        tft->fillScreen(bg);
        return OSAVal();
    }
    if (name == "bg") {
        tft->fillScreen(tft->color565(iN(0), iN(1), iN(2)));
        return OSAVal();
    }
    if (name == "text") {
        tft->setTextFont(textFont); tft->setTextSize(1);
        tft->setTextColor(txtColor); tft->setTextDatum(TL_DATUM);
        tft->drawString(S(2), iN(0), iN(1));
        return OSAVal();
    }
    if (name == "textc") {
        // text centered: textc(cx, y, "msg")
        tft->setTextFont(textFont); tft->setTextSize(1);
        tft->setTextColor(txtColor); tft->setTextDatum(MC_DATUM);
        tft->drawString(S(2), iN(0), iN(1));
        return OSAVal();
    }
    if (name == "rect") {
        tft->fillRect(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "rrect") {
        // rounded rect: rrect(x,y,w,h,r)
        tft->fillRoundRect(iN(0), iN(1), iN(2), iN(3), iN(4), drawColor);
        return OSAVal();
    }
    if (name == "frame") {
        tft->drawRect(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "circle") {
        tft->fillCircle(iN(0), iN(1), iN(2), drawColor);
        return OSAVal();
    }
    if (name == "ring") {
        tft->drawCircle(iN(0), iN(1), iN(2), drawColor);
        return OSAVal();
    }
    if (name == "line") {
        tft->drawLine(iN(0), iN(1), iN(2), iN(3), drawColor);
        return OSAVal();
    }
    if (name == "pixel") {
        tft->drawPixel(iN(0), iN(1), drawColor);
        return OSAVal();
    }
    if (name == "setcolor") {
        drawColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "textcolor") {
        txtColor = tft->color565(iN(0), iN(1), iN(2));
        return OSAVal();
    }
    if (name == "fontsize") {
        int fs = iN(0); textFont = (fs >= 4) ? 4 : (fs >= 2) ? 2 : 1;
        return OSAVal();
    }
    if (name == "screenw") return OSAVal(240.0);
    if (name == "screenh") return OSAVal(320.0);

    // ── Touch ─────────────────────────────────────────────────────────────────

    if (name == "touch.down") return OSAVal(ts->touched() ? 1.0 : 0.0);
    if (name == "touch.x") {
        if (!ts->touched()) return OSAVal(-1.0);
        TS_Point p = ts->getPoint();
        return OSAVal((double)map(p.x, 300, 3800, 0, 240));
    }
    if (name == "touch.y") {
        if (!ts->touched()) return OSAVal(-1.0);
        TS_Point p = ts->getPoint();
        return OSAVal((double)map(p.y, 300, 3800, 0, 320));
    }

    // ── System ────────────────────────────────────────────────────────────────

    if (name == "wait")   { delay(iN(0)); return OSAVal(); }
    if (name == "exit")   { exitFlag = true; return OSAVal(); }
    if (name == "confirm") {
        // confirm(title, body) or confirm(body) → 1 if OK, 0 if Cancel
        String t = (argc >= 2) ? S(0) : appName;
        String b = (argc >= 2) ? S(1) : S(0);
        return OSAVal(showSystemPopup(t, b, "", "Cancel", "OK", false) ? 1.0 : 0.0);
    }
    if (name == "millis") return OSAVal((double)millis());
    if (name == "theme")  return OSAVal((double)sysTheme);
    if (name == "notify") {
        if (!checkPerm(OSA_PERM_NOTIFY, "Send Notifications",
                       "Show system notification banners"))
            return OSAVal();
        extern void osa_notify(const char*);
        osa_notify(S(0).c_str());
        return OSAVal();
    }
    if (name == "setbright") {
        if (!checkPerm(OSA_PERM_SYSTEM, "System Settings",
                       "Change screen brightness"))
            return OSAVal();
        sysBrightness = constrain(iN(0), 10, 255);
        analogWrite(21, sysBrightness); // TFT_BL
        return OSAVal();
    }
    if (name == "setwallpaper") {
        if (!checkPerm(OSA_PERM_SYSTEM, "System Settings",
                       "Change system wallpaper"))
            return OSAVal();
        Config::set("wallpaper_path", S(0));
        Config::save();
        return OSAVal();
    }
    if (name == "wifi.connected") return OSAVal(WiFi.status() == WL_CONNECTED ? 1.0 : 0.0);
    if (name == "wifi.ssid") return OSAVal(WiFi.SSID());
    if (name == "wifi.ip")   return OSAVal(WiFi.localIP().toString());

    // ── Math ─────────────────────────────────────────────────────────────────

    if (name == "abs")    return OSAVal(fabs(N(0)));
    if (name == "min")    return OSAVal(min(N(0), N(1)));
    if (name == "max")    return OSAVal(max(N(0), N(1)));
    if (name == "sqrt")   return OSAVal(sqrt(N(0)));
    if (name == "sin")    return OSAVal(sin(N(0)));
    if (name == "cos")    return OSAVal(cos(N(0)));
    if (name == "floor")  return OSAVal(floor(N(0)));
    if (name == "ceil")   return OSAVal(ceil(N(0)));
    if (name == "int")    return OSAVal((double)(int)N(0));
    if (name == "pow")    return OSAVal(pow(N(0), N(1)));
    if (name == "random") {
        if (argc < 2) return OSAVal((double)random((long)N(0)));
        return OSAVal((double)random((long)N(0), (long)N(1)));
    }
    if (name == "pi") return OSAVal(3.14159265358979);

    // ── String ───────────────────────────────────────────────────────────────

    if (name == "str")       return OSAVal(a[0].toString());
    if (name == "num")       return OSAVal(S(0).toDouble());
    if (name == "len")       return OSAVal((double)S(0).length());
    if (name == "upper")     { String s = S(0); s.toUpperCase(); return OSAVal(s); }
    if (name == "lower")     { String s = S(0); s.toLowerCase(); return OSAVal(s); }
    if (name == "trim")      { String s = S(0); s.trim(); return OSAVal(s); }
    if (name == "substr")    return OSAVal(S(0).substring(iN(1), iN(1) + iN(2)));
    if (name == "replace")   { String s = S(0); s.replace(S(1), S(2)); return OSAVal(s); }
    if (name == "contains")  return OSAVal(S(0).indexOf(S(1)) >= 0 ? 1.0 : 0.0);
    if (name == "startswith")return OSAVal(S(0).startsWith(S(1)) ? 1.0 : 0.0);
    if (name == "endswith")  return OSAVal(S(0).endsWith(S(1)) ? 1.0 : 0.0);
    if (name == "indexof")   return OSAVal((double)S(0).indexOf(S(1)));
    if (name == "char")      { char c = (char)iN(0); return OSAVal(String(c)); }
    if (name == "code")      return OSAVal((double)(unsigned char)S(0)[0]);
    if (name == "split") {
        // split(str, delim) -> returns the Nth part; split(s, ",", n)
        String src = S(0), delim = S(1); int idx = iN(2);
        int count = 0, start = 0;
        for (int i = 0; i <= (int)src.length(); i++) {
            bool atDelim = (i == (int)src.length()) ||
                           src.substring(i, i + delim.length()) == delim;
            if (atDelim) {
                if (count == idx) return OSAVal(src.substring(start, i));
                count++; start = i + delim.length();
            }
        }
        return OSAVal("");
    }

    // ── File I/O (sandboxed to /apps/<appname>/) ──────────────────────────────

    if (name == "fread") {
        if (!isSdReady) return OSAVal("");
        File f = SD.open(sandboxPath(S(0)));
        if (!f) return OSAVal("");
        String c = f.readString(); f.close(); return OSAVal(c);
    }
    if (name == "freadline") {
        if (!isSdReady) return OSAVal("");
        File f = SD.open(sandboxPath(S(0))); if (!f) return OSAVal("");
        int target = iN(1);
        for (int i = 0; i <= target; i++) {
            String line = f.readStringUntil('\n');
            if (i == target) { f.close(); line.trim(); return OSAVal(line); }
        }
        f.close(); return OSAVal("");
    }
    if (name == "fwrite") {
        if (!isSdReady) return OSAVal(0.0);
        String p = sandboxPath(S(0));
        SD.remove(p.c_str());
        File f = SD.open(p, FILE_WRITE); if (!f) return OSAVal(0.0);
        f.print(S(1)); f.close(); return OSAVal(1.0);
    }
    if (name == "fappend") {
        if (!isSdReady) return OSAVal(0.0);
        File f = SD.open(sandboxPath(S(0)), FILE_APPEND); if (!f) return OSAVal(0.0);
        f.println(S(1)); f.close(); return OSAVal(1.0);
    }
    if (name == "fexists") {
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.exists(sandboxPath(S(0)).c_str()) ? 1.0 : 0.0);
    }
    if (name == "fremove") {
        if (!isSdReady) return OSAVal(0.0);
        return OSAVal(SD.remove(sandboxPath(S(0)).c_str()) ? 1.0 : 0.0);
    }

    // Unknown built-in (user funcs were handled at the top)
    setError(-1, "Unknown: " + name);
    return OSAVal();
}
