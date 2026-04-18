#ifndef THEME_H
#define THEME_H
#include <Arduino.h>

extern int sysTheme;

namespace Theme {
    static inline uint16_t c(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    }
    static inline bool dark()     { return sysTheme == 1; }
    static inline uint16_t bg()      { return dark() ? c(18,18,20)    : c(240,240,245); }
    static inline uint16_t surface() { return dark() ? c(30,30,35)    : 0xFFFF; }
    static inline uint16_t header()  { return dark() ? c(28,28,32)    : c(248,248,248); }
    static inline uint16_t divider() { return dark() ? c(55,55,60)    : c(200,200,200); }
    static inline uint16_t divider2(){ return dark() ? c(45,45,50)    : c(220,220,220); }
    static inline uint16_t text()    { return dark() ? 0xFFFF         : 0x0000; }
    static inline uint16_t subtext() { return dark() ? c(160,160,165) : c(100,100,100); }
    static inline uint16_t hint()    { return dark() ? c(110,110,115) : c(150,150,150); }
    static inline uint16_t toggleOff(){ return dark() ? c(75,75,80)   : c(220,220,220); }
    static inline uint16_t kbdBg()   { return dark() ? c(50,50,55)   : c(209,213,219); }
    static inline uint16_t kbdKey()  { return dark() ? c(65,68,72)   : 0xFFFF; }
    static inline uint16_t kbdSpec() { return dark() ? c(40,42,46)   : c(170,175,185); }
    static inline uint16_t kbdShadow(){ return dark() ? c(25,25,28)  : c(136,138,141); }
    static inline uint16_t kbdText() { return dark() ? 0xFFFF        : 0x0000; }
}

#endif
