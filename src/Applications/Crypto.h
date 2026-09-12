#ifndef CRYPTO_H
#define CRYPTO_H
#include <Arduino.h>

// Authenticated encryption for secrets that live on the removable SD card
// (Wi-Fi credentials, the lockscreen passcode, crypto.* from privileged
// scripts).
//
// Values are AES-256-GCM sealed with a key that never leaves the device:
// SHA-256 over a fixed domain tag, the factory Wi-Fi MAC and a 32-byte random
// secret that is generated on first boot and kept in NVS (internal flash).
// A copy of the SD card, or of this public firmware, is therefore not enough
// to read the stored values. Format: "v2:" + base64(nonce[12] || ciphertext
// || tag[16]); a fresh random nonce is drawn for every encrypt().
//
// Earlier releases obfuscated the same values with a fixed XOR key compiled
// into the firmware. decrypt() still understands that format so that an
// existing card upgrades in place, and main.cpp re-seals any legacy value it
// finds at boot. The legacy path is decrypt-only and can be deleted once no
// pre-1.2 cards remain.
namespace Crypto {
    // Loads (or on first boot creates) the device secret and derives the key.
    // Needs only NVS, but call it while an RF stack (Wi-Fi or Bluetooth) is
    // running the first time so the secret comes from the true hardware RNG.
    void begin();

    // True once a device secret exists in NVS (i.e. begin() will not draw one).
    bool hasDeviceSecret();

    // Returns true when the per-device key is available.
    bool ready();

    String encrypt(const String& plain);
    // Returns an empty String when the value was tampered with or sealed by
    // another device.
    String decrypt(const String& stored);

    // True for values already in the v2 authenticated format.
    bool isCurrent(const String& stored);
}

#endif
