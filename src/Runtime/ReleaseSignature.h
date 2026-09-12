#pragma once

#include <Arduino.h>
#include <mbedtls/sha256.h>

// ECDSA P-256 verification against the release public key compiled into
// OpenOS (OpenOSReleaseKeys.h). Shared by the firmware OTA manifest and the
// OpenStore catalog so both feeds are trusted by the same offline key.
namespace ReleaseSignature {

// Streams the bytes that a signature covers into SHA-256 without copying the
// document — catalogs are up to 24 KB and the heap is tight during HTTPS.
class Hasher {
public:
    Hasher();
    ~Hasher();
    bool update(const uint8_t* data, size_t length);
    bool update(const char* text);
    bool update(const String& text, size_t offset, size_t length);
    bool finish(uint8_t digest[32]);
private:
    mbedtls_sha256_context context;
    bool ok;
};

// `signatureBase64` is the DER ECDSA signature as written by the release
// tools. Returns false with a user-facing reason in `why`.
bool verifyDigest(const uint8_t digest[32], const String& signatureBase64,
                  String& why);

// Convenience for small payloads that are already in memory.
bool verify(const uint8_t* payload, size_t length,
            const String& signatureBase64, String& why);

// True when the string is plausibly a base64 DER P-256 signature: length
// and character checks only, no decoding.
bool plausibleSignature(const String& signatureBase64);

} // namespace ReleaseSignature
