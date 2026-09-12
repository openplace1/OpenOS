#include "Crypto.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace Crypto {
namespace {

static constexpr const char* PREF_NAMESPACE = "openos-sec";
static constexpr const char* PREF_SECRET_KEY = "secret";
static constexpr const char* PREF_NONCE_KEY = "nonce";
static constexpr const char* KEY_DOMAIN = "OpenOS-Crypto-v2";
static constexpr const char* V2_PREFIX = "v2:";
static constexpr size_t SECRET_LEN = 32;
static constexpr size_t KEY_LEN = 32;
static constexpr size_t NONCE_LEN = 12;
static constexpr size_t TAG_LEN = 16;
static constexpr size_t MAX_PLAIN_LEN = 1024;

static uint8_t s_key[KEY_LEN];
static bool s_ready = false;
// Persistent counter that forms the first four nonce bytes. GCM must never
// reuse a nonce under one key, and esp_random() is only guaranteed to be
// unpredictable while an RF stack runs; the counter keeps nonces unique even
// for values sealed before Wi-Fi comes up (boot-time migration).
static uint32_t s_nonceCounter = 0;

// Kept only so cards written by OpenOS <= 1.1 can be read once and re-sealed.
static const char LEGACY_XOR_KEY[] = "0p3nOS!k3y$";

static void fillRandom(uint8_t* buffer, size_t length) {
    while (length >= 4) {
        uint32_t word = esp_random();
        memcpy(buffer, &word, 4);
        buffer += 4;
        length -= 4;
    }
    if (length > 0) {
        uint32_t word = esp_random();
        memcpy(buffer, &word, length);
    }
}

static bool nextNonce(uint8_t nonce[NONCE_LEN]) {
    Preferences preferences;
    if (!preferences.begin(PREF_NAMESPACE, false)) return false;
    uint32_t counter = preferences.getUInt(PREF_NONCE_KEY, 0);
    if (counter < s_nonceCounter) counter = s_nonceCounter;
    ++counter;
    bool ok = preferences.putUInt(PREF_NONCE_KEY, counter) == sizeof(counter);
    preferences.end();
    if (!ok) return false;
    s_nonceCounter = counter;
    nonce[0] = (uint8_t)(counter >> 24);
    nonce[1] = (uint8_t)(counter >> 16);
    nonce[2] = (uint8_t)(counter >> 8);
    nonce[3] = (uint8_t)counter;
    fillRandom(nonce + 4, NONCE_LEN - 4);
    return true;
}

static bool loadOrCreateSecret(uint8_t secret[SECRET_LEN]) {
    Preferences preferences;
    if (!preferences.begin(PREF_NAMESPACE, false)) return false;
    s_nonceCounter = preferences.getUInt(PREF_NONCE_KEY, 0);
    size_t stored = preferences.getBytesLength(PREF_SECRET_KEY);
    bool ok = false;
    if (stored == SECRET_LEN) {
        ok = preferences.getBytes(PREF_SECRET_KEY, secret, SECRET_LEN) == SECRET_LEN;
    } else {
        // First boot. esp_random() is a true hardware source only while an RF
        // stack runs; begin() is therefore called after Wi-Fi was brought up
        // (main.cpp starts the STA interface for this draw if it is disabled).
        fillRandom(secret, SECRET_LEN);
        ok = preferences.putBytes(PREF_SECRET_KEY, secret, SECRET_LEN) == SECRET_LEN;
        if (ok) Serial.println("[CRYPTO] generated device secret");
    }
    preferences.end();
    return ok;
}

static bool base64Encode(const uint8_t* input, size_t length, String& output) {
    size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, input, length);
    if (needed == 0 || !output.reserve(needed)) return false;
    char* buffer = (char*)malloc(needed);
    if (!buffer) return false;
    size_t written = 0;
    bool ok = mbedtls_base64_encode((unsigned char*)buffer, needed, &written,
                                    input, length) == 0;
    if (ok) ok = output.concat(buffer, written);
    free(buffer);
    return ok;
}

static String legacyDecrypt(const String& hex) {
    int keyLength = strlen(LEGACY_XOR_KEY);
    String out;
    int pairs = hex.length() / 2;
    if (!out.reserve(pairs)) return String();
    for (int i = 0; i < pairs; i++) {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        if (!isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1]))
            return String();
        uint8_t value = (uint8_t)strtoul(pair, nullptr, 16);
        out += (char)(value ^ (uint8_t)LEGACY_XOR_KEY[i % keyLength]);
    }
    return out;
}

} // namespace

void begin() {
    s_ready = false;
    uint8_t secret[SECRET_LEN];
    if (!loadOrCreateSecret(secret)) {
        Serial.println("[CRYPTO] device secret unavailable; encryption disabled");
        return;
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0 &&
              mbedtls_sha256_update_ret(&sha, (const uint8_t*)KEY_DOMAIN,
                                        strlen(KEY_DOMAIN)) == 0 &&
              mbedtls_sha256_update_ret(&sha, mac, sizeof(mac)) == 0 &&
              mbedtls_sha256_update_ret(&sha, secret, SECRET_LEN) == 0 &&
              mbedtls_sha256_finish_ret(&sha, s_key) == 0;
    mbedtls_sha256_free(&sha);
    memset(secret, 0, sizeof(secret));
    s_ready = ok;
    if (!ok) Serial.println("[CRYPTO] key derivation failed");
    else Serial.printf("[CRYPTO] device key ready, %u values sealed so far\n",
                       (unsigned)s_nonceCounter);
}

bool ready() { return s_ready; }

bool hasDeviceSecret() {
    Preferences preferences;
    if (!preferences.begin(PREF_NAMESPACE, true)) return false;
    bool present = preferences.getBytesLength(PREF_SECRET_KEY) == SECRET_LEN;
    preferences.end();
    return present;
}

bool isCurrent(const String& stored) {
    return stored.startsWith(V2_PREFIX);
}

String encrypt(const String& plain) {
    if (!s_ready || plain.length() > MAX_PLAIN_LEN) return String();
    const size_t length = plain.length();
    const size_t sealedLength = NONCE_LEN + length + TAG_LEN;
    uint8_t* sealed = (uint8_t*)malloc(sealedLength);
    if (!sealed) return String();
    uint8_t* nonce = sealed;
    uint8_t* ciphertext = sealed + NONCE_LEN;
    uint8_t* tag = ciphertext + length;
    if (!nextNonce(nonce)) {
        free(sealed);
        return String();
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key,
                                 KEY_LEN * 8) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, length,
                                        nonce, NONCE_LEN, nullptr, 0,
                                        (const uint8_t*)plain.c_str(),
                                        ciphertext, TAG_LEN, tag) == 0;
    mbedtls_gcm_free(&gcm);

    String output;
    if (ok) {
        output = V2_PREFIX;
        ok = base64Encode(sealed, sealedLength, output);
    }
    memset(sealed, 0, sealedLength);
    free(sealed);
    return ok ? output : String();
}

String decrypt(const String& stored) {
    if (stored.length() == 0) return String();
    if (!isCurrent(stored)) return legacyDecrypt(stored);
    if (!s_ready) return String();

    const char* encoded = stored.c_str() + strlen(V2_PREFIX);
    const size_t encodedLength = stored.length() - strlen(V2_PREFIX);
    size_t sealedLength = 0;
    int result = mbedtls_base64_decode(nullptr, 0, &sealedLength,
                                       (const uint8_t*)encoded, encodedLength);
    if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL ||
        sealedLength < NONCE_LEN + TAG_LEN ||
        sealedLength > NONCE_LEN + TAG_LEN + MAX_PLAIN_LEN)
        return String();
    uint8_t* sealed = (uint8_t*)malloc(sealedLength);
    if (!sealed) return String();
    size_t decoded = 0;
    if (mbedtls_base64_decode(sealed, sealedLength, &decoded,
                              (const uint8_t*)encoded, encodedLength) != 0 ||
        decoded != sealedLength) {
        free(sealed);
        return String();
    }

    const size_t length = sealedLength - NONCE_LEN - TAG_LEN;
    const uint8_t* nonce = sealed;
    const uint8_t* ciphertext = sealed + NONCE_LEN;
    const uint8_t* tag = ciphertext + length;
    uint8_t* plain = (uint8_t*)malloc(length + 1);
    if (!plain) {
        free(sealed);
        return String();
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key,
                                 KEY_LEN * 8) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, length, nonce, NONCE_LEN,
                                       nullptr, 0, tag, TAG_LEN,
                                       ciphertext, plain) == 0;
    mbedtls_gcm_free(&gcm);
    free(sealed);

    String output;
    if (ok) {
        plain[length] = 0;
        // Stored values are text; a NUL inside means the input was not ours.
        ok = strlen((const char*)plain) == length && output.reserve(length) &&
             output.concat((const char*)plain, length);
    }
    memset(plain, 0, length + 1);
    free(plain);
    return ok ? output : String();
}

} // namespace Crypto
