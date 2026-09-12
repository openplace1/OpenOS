#pragma once

#include "PortableString.h"

#include <stddef.h>

// Framing of the signed OpenStore catalog, shared by the device verifier
// (PackageManager.cpp) and the host tests. build_opk.py emits the compact
// object {"schema":1,"apps":[...],"keyId":"..."}, signs PREFIX followed by
// that exact text, and appends ,"signature":"<base64>" as the last field.
namespace CatalogSignature {

static constexpr const char* PREFIX = "OPENOS-CATALOG-V1\n";
static constexpr const char* MARKER = ",\"signature\":\"";
static constexpr size_t MARKER_LENGTH = 14;

// On success `signedLength` is how many leading bytes of `document` the
// signature covers (they are followed by a closing brace when hashing) and
// `signature` the base64 text. Trailing whitespace after the object is
// ignored. `error` receives a user-facing reason on failure.
inline bool split(const String& document, size_t& signedLength,
                  String& signature, String& error) {
    int end = document.length();
    while (end > 0 && ((uint8_t)document[end - 1] <= 0x20)) --end;
    int position = document.lastIndexOf(MARKER);
    if (position <= 0 || position + (int)MARKER_LENGTH >= end) {
        error = "Store catalog is not signed";
        return false;
    }
    int signatureStart = position + (int)MARKER_LENGTH;
    int signatureEnd = document.indexOf('"', signatureStart);
    if (signatureEnd < 0 || signatureEnd + 2 != end ||
        document[signatureEnd + 1] != '}') {
        error = "Store catalog signature field is malformed";
        return false;
    }
    signature = document.substring(signatureStart, signatureEnd);
    signedLength = (size_t)position;
    return true;
}

} // namespace CatalogSignature
