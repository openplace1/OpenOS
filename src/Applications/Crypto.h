#ifndef CRYPTO_H
#define CRYPTO_H
#include <Arduino.h>

// Simple XOR obfuscation — prevents casual file reading on SD card.
// Not cryptographically secure; key is compiled into firmware.
namespace Crypto {
    static const char KEY[] = "0p3nOS!k3y$";

    static inline String encrypt(const String& input) {
        int kLen = strlen(KEY);
        String out = "";
        for (int i = 0; i < (int)input.length(); i++) {
            uint8_t c = (uint8_t)input[i] ^ (uint8_t)KEY[i % kLen];
            char hex[3]; sprintf(hex, "%02X", c);
            out += hex;
        }
        return out;
    }

    static inline String decrypt(const String& hex) {
        int kLen = strlen(KEY);
        String out = "";
        int pairs = hex.length() / 2;
        for (int i = 0; i < pairs; i++) {
            String bs = hex.substring(i * 2, i * 2 + 2);
            uint8_t b = (uint8_t)strtoul(bs.c_str(), nullptr, 16);
            out += (char)(b ^ (uint8_t)KEY[i % kLen]);
        }
        return out;
    }
}

#endif
