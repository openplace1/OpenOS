#include "ReleaseSignature.h"

#include "OpenOSReleaseKeys.h"

#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <string.h>

namespace ReleaseSignature {

Hasher::Hasher() {
    mbedtls_sha256_init(&context);
    ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
}

Hasher::~Hasher() {
    mbedtls_sha256_free(&context);
}

bool Hasher::update(const uint8_t* data, size_t length) {
    if (ok && length > 0)
        ok = mbedtls_sha256_update_ret(&context, data, length) == 0;
    return ok;
}

bool Hasher::update(const char* text) {
    return update((const uint8_t*)text, strlen(text));
}

bool Hasher::update(const String& text, size_t offset, size_t length) {
    if (offset > text.length() || length > text.length() - offset) {
        ok = false;
        return false;
    }
    return update((const uint8_t*)text.c_str() + offset, length);
}

bool Hasher::finish(uint8_t digest[32]) {
    if (ok) ok = mbedtls_sha256_finish_ret(&context, digest) == 0;
    return ok;
}

bool plausibleSignature(const String& signatureBase64) {
    // DER-encoded P-256 signatures are 70-72 bytes: 96 base64 characters.
    if (signatureBase64.length() < 80 || signatureBase64.length() > 120)
        return false;
    for (size_t i = 0; i < signatureBase64.length(); ++i) {
        char c = signatureBase64[i];
        bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        if (!valid) return false;
    }
    return true;
}

bool verifyDigest(const uint8_t digest[32], const String& signatureBase64,
                  String& why) {
    if (!plausibleSignature(signatureBase64)) {
        why = "signature encoding is invalid";
        return false;
    }
    uint8_t signature[80];
    size_t signatureLength = 0;
    int result = mbedtls_base64_decode(signature, sizeof(signature),
                                       &signatureLength,
                                       (const uint8_t*)signatureBase64.c_str(),
                                       signatureBase64.length());
    if (result != 0 || signatureLength < 64 || signatureLength > sizeof(signature)) {
        why = "signature encoding is invalid";
        return false;
    }

    mbedtls_ecdsa_context context;
    mbedtls_ecdsa_init(&context);
    result = mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0)
        result = mbedtls_ecp_point_read_binary(
            &context.grp, &context.Q, OpenOSReleaseKeys::P256_PUBLIC_KEY,
            sizeof(OpenOSReleaseKeys::P256_PUBLIC_KEY));
    if (result == 0)
        result = mbedtls_ecdsa_read_signature(&context, digest, 32,
                                              signature, signatureLength);
    mbedtls_ecdsa_free(&context);
    if (result != 0) {
        why = "signature is not trusted";
        return false;
    }
    return true;
}

bool verify(const uint8_t* payload, size_t length,
            const String& signatureBase64, String& why) {
    Hasher hasher;
    uint8_t digest[32];
    if (!hasher.update(payload, length) || !hasher.finish(digest)) {
        why = "could not hash signed payload";
        return false;
    }
    return verifyDigest(digest, signatureBase64, why);
}

} // namespace ReleaseSignature
