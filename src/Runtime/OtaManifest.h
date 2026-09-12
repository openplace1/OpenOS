#pragma once

#include "PortableString.h"

#include <stddef.h>
#include <stdint.h>

// Pure parsing and validation of the signed OTA manifest (update/info.json).
// No networking, flash or mbedTLS here, so the same code runs in the host
// test harness under test/host. FirmwareUpdate.cpp owns the transport, the
// persisted source URL and the signature/hash verification.
namespace OtaManifest {

static constexpr size_t MAX_BYTES = 4096;
static constexpr size_t DESCRIPTION_MAX_BYTES = 1200;
static constexpr size_t URL_MAX_BYTES = 2048;
static constexpr const char* CANONICAL_PREFIX = "OPENOS-OTA-V1\n";

struct Manifest {
    uint32_t schema = 0;
    String product;
    String channel;
    String target;
    String partitionScheme;
    String name;
    String version;
    uint32_t versionCode = 0;
    uint32_t minUpdaterVersionCode = 0;
    String type;
    String description;
    String published;
    String firmware;
    uint32_t size = 0;
    String sha256;
    String keyId;
    String signature;
    String resolvedFirmwareUrl;
};

// What the running firmware expects the manifest to declare.
struct Expectations {
    const char* target = "";
    const char* partitionScheme = "";
    const char* keyId = "";
    // The official feed accepts only the stable channel.
    bool officialFeed = true;
    // Size of one OTA app slot; the image must fit.
    uint32_t slotBytes = 0;
    // The info.json URL the manifest came from, used to resolve `firmware`.
    String infoUrl;
};

bool validHttpsUrl(const String& url);
bool validRelativeFirmwarePath(const String& path);
// Resolves the `firmware` field (absolute https URL or a path relative to the
// manifest's directory) into `resolved`.
bool resolveFirmwareUrl(const String& infoUrl, const String& firmware,
                        String& resolved);

// Strict flat-object parser: exactly the documented fields, each once, no
// \u escapes, no trailing data. `error` receives a user-facing reason.
bool parse(const String& json, Manifest& manifest, String& error);

// Field-level checks and firmware URL resolution; the signature itself is
// verified by the caller over canonicalPayload().
bool validateFields(Manifest& manifest, const Expectations& expected, String& error);

// Length-prefixed text the release signature covers. Must match
// build_update.py::canonical_payload byte for byte.
bool canonicalPayload(const Manifest& manifest, String& payload);

// Base64 length/charset check for a DER P-256 signature (no decoding).
bool validSignatureEncoding(const String& signatureBase64);

// Compares a binary SHA-256 with its 64-character lowercase hex form.
bool digestMatches(const uint8_t digest[32], const String& expectedHex);

} // namespace OtaManifest
