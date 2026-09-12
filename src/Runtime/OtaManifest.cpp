#include "OtaManifest.h"

namespace OtaManifest {
namespace {

static void skipWs(const String& json, int& position) {
    while (position < (int)json.length()) {
        char c = json[position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++position;
    }
}

static bool parseJsonString(const String& json, int& position, String& value) {
    if (position >= (int)json.length() || json[position] != '"') return false;
    ++position;
    value = "";
    while (position < (int)json.length()) {
        uint8_t c = (uint8_t)json[position++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') {
            if (!value.concat((char)c)) return false;
            continue;
        }
        if (position >= (int)json.length()) return false;
        char escaped = json[position++];
        char decoded = 0;
        if (escaped == '"' || escaped == '\\' || escaped == '/') decoded = escaped;
        else if (escaped == 'b') decoded = '\b';
        else if (escaped == 'f') decoded = '\f';
        else if (escaped == 'n') decoded = '\n';
        else if (escaped == 'r') decoded = '\r';
        else if (escaped == 't') decoded = '\t';
        else return false; // release builder writes UTF-8 directly, never \u escapes
        if (!value.concat(decoded)) return false;
    }
    return false;
}

static bool parseJsonUInt(const String& json, int& position, uint32_t& value) {
    if (position >= (int)json.length() || json[position] < '0' || json[position] > '9')
        return false;
    uint64_t parsed = 0;
    int start = position;
    if (json[position] == '0') {
        ++position;
        if (position < (int)json.length() && json[position] >= '0' && json[position] <= '9')
            return false;
    } else {
        while (position < (int)json.length() &&
               json[position] >= '0' && json[position] <= '9') {
            parsed = parsed * 10U + (uint8_t)(json[position] - '0');
            if (parsed > UINT32_MAX) return false;
            ++position;
        }
    }
    if (position == start) return false;
    value = (uint32_t)parsed;
    return true;
}

enum ManifestField : uint8_t {
    MF_SCHEMA = 0, MF_PRODUCT, MF_CHANNEL, MF_TARGET, MF_PARTITION,
    MF_NAME, MF_VERSION, MF_VERSION_CODE, MF_MIN_UPDATER, MF_TYPE,
    MF_DESCRIPTION, MF_PUBLISHED, MF_FIRMWARE, MF_SIZE, MF_SHA256,
    MF_KEY_ID, MF_SIGNATURE, MF_COUNT
};

static int manifestField(const String& key) {
    if (key == "schema") return MF_SCHEMA;
    if (key == "product") return MF_PRODUCT;
    if (key == "channel") return MF_CHANNEL;
    if (key == "target") return MF_TARGET;
    if (key == "partitionScheme") return MF_PARTITION;
    if (key == "name") return MF_NAME;
    if (key == "version") return MF_VERSION;
    if (key == "versionCode") return MF_VERSION_CODE;
    if (key == "minUpdaterVersionCode") return MF_MIN_UPDATER;
    if (key == "releaseType") return MF_TYPE;
    if (key == "description") return MF_DESCRIPTION;
    if (key == "publishedAt") return MF_PUBLISHED;
    if (key == "firmware") return MF_FIRMWARE;
    if (key == "size") return MF_SIZE;
    if (key == "sha256") return MF_SHA256;
    if (key == "keyId") return MF_KEY_ID;
    if (key == "signature") return MF_SIGNATURE;
    return -1;
}

static bool numericField(int field) {
    return field == MF_SCHEMA || field == MF_VERSION_CODE ||
           field == MF_MIN_UPDATER || field == MF_SIZE;
}

static bool assignStringField(Manifest& manifest, int field, String value) {
    switch (field) {
        case MF_PRODUCT: manifest.product = static_cast<String&&>(value); break;
        case MF_CHANNEL: manifest.channel = static_cast<String&&>(value); break;
        case MF_TARGET: manifest.target = static_cast<String&&>(value); break;
        case MF_PARTITION: manifest.partitionScheme = static_cast<String&&>(value); break;
        case MF_NAME: manifest.name = static_cast<String&&>(value); break;
        case MF_VERSION: manifest.version = static_cast<String&&>(value); break;
        case MF_TYPE: manifest.type = static_cast<String&&>(value); break;
        case MF_DESCRIPTION: manifest.description = static_cast<String&&>(value); break;
        case MF_PUBLISHED: manifest.published = static_cast<String&&>(value); break;
        case MF_FIRMWARE: manifest.firmware = static_cast<String&&>(value); break;
        case MF_SHA256: manifest.sha256 = static_cast<String&&>(value); break;
        case MF_KEY_ID: manifest.keyId = static_cast<String&&>(value); break;
        case MF_SIGNATURE: manifest.signature = static_cast<String&&>(value); break;
        default: return false;
    }
    return true;
}

static bool assignNumberField(Manifest& manifest, int field, uint32_t value) {
    if (field == MF_SCHEMA) manifest.schema = value;
    else if (field == MF_VERSION_CODE) manifest.versionCode = value;
    else if (field == MF_MIN_UPDATER) manifest.minUpdaterVersionCode = value;
    else if (field == MF_SIZE) manifest.size = value;
    else return false;
    return true;
}

static bool hasControl(const String& value, bool allowLineBreaks = false) {
    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];
        if (c < 0x20 && !(allowLineBreaks && (c == '\n' || c == '\t')))
            return true;
    }
    return false;
}

static bool validLowerSha256(const String& value) {
    if (value.length() != 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool appendCanonical(String& output, const char* key, const String& value) {
    return output.concat(key) && output.concat(':') &&
           output.concat(String((unsigned long)value.length())) &&
           output.concat(':') && output.concat(value) && output.concat('\n');
}

static bool reject(String& error, const String& message) {
    error = message;
    return false;
}

} // namespace

bool validHttpsUrl(const String& url) {
    if (!url.startsWith("https://") || url.length() < 10 ||
        url.length() > URL_MAX_BYTES) return false;
    int hostStart = 8;
    int hostEnd = url.indexOf('/', hostStart);
    if (hostEnd < 0) hostEnd = url.length();
    int at = url.indexOf('@', hostStart);
    if (hostEnd <= hostStart || (at >= 0 && at < hostEnd)) return false;
    for (size_t i = 0; i < url.length(); ++i)
        if ((uint8_t)url[i] <= 0x20 || url[i] == '\\') return false;
    return true;
}

bool validRelativeFirmwarePath(const String& path) {
    if (path.length() < 1 || path.length() > 160 || path[0] == '/' ||
        path[0] == '\\' || path.indexOf('\\') >= 0 ||
        path.indexOf(':') >= 0 || path.indexOf('?') >= 0 ||
        path.indexOf('#') >= 0) return false;
    int start = 0;
    while (start <= (int)path.length()) {
        int slash = path.indexOf('/', start);
        if (slash < 0) slash = path.length();
        String part = path.substring(start, slash);
        if (part.length() == 0 || part == "." || part == "..") return false;
        start = slash + 1;
        if (slash == (int)path.length()) break;
    }
    for (size_t i = 0; i < path.length(); ++i)
        if ((uint8_t)path[i] <= 0x20) return false;
    return true;
}

bool resolveFirmwareUrl(const String& infoUrl, const String& firmware,
                        String& resolved) {
    if (firmware.startsWith("https://")) {
        if (!validHttpsUrl(firmware)) return false;
        resolved = firmware;
        return true;
    }
    if (!validRelativeFirmwarePath(firmware)) return false;
    String base = infoUrl;
    int suffix = base.indexOf('?');
    if (suffix < 0) suffix = base.indexOf('#');
    if (suffix >= 0) base.remove(suffix);
    int slash = base.lastIndexOf('/');
    if (slash < 8) return false;
    resolved = base.substring(0, slash + 1) + firmware;
    return validHttpsUrl(resolved);
}

bool parse(const String& json, Manifest& manifest, String& error) {
    int position = 0;
    skipWs(json, position);
    if (position >= (int)json.length() || json[position++] != '{')
        return reject(error, "OTA manifest must be a JSON object");
    uint32_t seen = 0;
    skipWs(json, position);
    while (position < (int)json.length() && json[position] != '}') {
        String key;
        if (!parseJsonString(json, position, key))
            return reject(error, "OTA manifest contains an invalid key");
        int field = manifestField(key);
        if (field < 0) return reject(error, String("Unknown OTA manifest field: ") + key);
        uint32_t bit = 1UL << field;
        if (seen & bit) return reject(error, String("Duplicate OTA manifest field: ") + key);
        seen |= bit;
        skipWs(json, position);
        if (position >= (int)json.length() || json[position++] != ':')
            return reject(error, "OTA manifest is missing ':'");
        skipWs(json, position);
        if (numericField(field)) {
            uint32_t number = 0;
            if (!parseJsonUInt(json, position, number) ||
                !assignNumberField(manifest, field, number))
                return reject(error, String("Invalid numeric OTA field: ") + key);
        } else {
            String value;
            if (!parseJsonString(json, position, value) ||
                !assignStringField(manifest, field, static_cast<String&&>(value)))
                return reject(error, String("Invalid string OTA field: ") + key);
        }
        skipWs(json, position);
        if (position < (int)json.length() && json[position] == ',') {
            ++position;
            skipWs(json, position);
            if (position < (int)json.length() && json[position] == '}')
                return reject(error, "OTA manifest has a trailing comma");
            continue;
        }
        break;
    }
    if (position >= (int)json.length() || json[position++] != '}')
        return reject(error, "OTA manifest JSON is incomplete");
    skipWs(json, position);
    if (position != (int)json.length()) return reject(error, "OTA manifest has trailing data");
    const uint32_t required = (1UL << MF_COUNT) - 1UL;
    if (seen != required) return reject(error, "OTA manifest is missing required fields");
    return true;
}

bool canonicalPayload(const Manifest& manifest, String& payload) {
    if (!payload.reserve(512 + manifest.description.length() +
                         manifest.firmware.length())) return false;
    payload = CANONICAL_PREFIX;
    return appendCanonical(payload, "schema", String((unsigned long)manifest.schema)) &&
           appendCanonical(payload, "product", manifest.product) &&
           appendCanonical(payload, "channel", manifest.channel) &&
           appendCanonical(payload, "target", manifest.target) &&
           appendCanonical(payload, "partitionScheme", manifest.partitionScheme) &&
           appendCanonical(payload, "name", manifest.name) &&
           appendCanonical(payload, "version", manifest.version) &&
           appendCanonical(payload, "versionCode", String((unsigned long)manifest.versionCode)) &&
           appendCanonical(payload, "minUpdaterVersionCode",
                           String((unsigned long)manifest.minUpdaterVersionCode)) &&
           appendCanonical(payload, "releaseType", manifest.type) &&
           appendCanonical(payload, "description", manifest.description) &&
           appendCanonical(payload, "publishedAt", manifest.published) &&
           appendCanonical(payload, "firmware", manifest.firmware) &&
           appendCanonical(payload, "size", String((unsigned long)manifest.size)) &&
           appendCanonical(payload, "sha256", manifest.sha256) &&
           appendCanonical(payload, "keyId", manifest.keyId);
}

bool validSignatureEncoding(const String& signatureBase64) {
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

bool validateFields(Manifest& manifest, const Expectations& expected, String& error) {
    if (manifest.schema != 1 || manifest.product != "openos")
        return reject(error, "Unsupported OTA manifest schema or product");
    if (manifest.target != expected.target ||
        manifest.partitionScheme != expected.partitionScheme)
        return reject(error, "OTA release targets different hardware or partitions");
    if (manifest.keyId != expected.keyId)
        return reject(error, "OTA manifest uses an unknown release key");
    if (manifest.channel != "stable" && manifest.channel != "beta" &&
        manifest.channel != "dev")
        return reject(error, "OTA manifest has an invalid channel");
    if (expected.officialFeed && manifest.channel != "stable")
        return reject(error, "The official OTA feed only accepts stable releases");
    if (manifest.type != "major" && manifest.type != "minor" &&
        manifest.type != "patch" && manifest.type != "security")
        return reject(error, "OTA manifest has an invalid release type");
    if (manifest.name.length() < 1 || manifest.name.length() > 80 ||
        manifest.version.length() < 1 || manifest.version.length() > 24 ||
        manifest.published.length() < 1 || manifest.published.length() > 40 ||
        manifest.description.length() > DESCRIPTION_MAX_BYTES ||
        hasControl(manifest.name) || hasControl(manifest.version) ||
        hasControl(manifest.published) || hasControl(manifest.description, true))
        return reject(error, "OTA manifest text fields are invalid");
    if (manifest.versionCode < 1 || manifest.minUpdaterVersionCode < 1 ||
        manifest.size < 4096 || manifest.size > expected.slotBytes)
        return reject(error, "OTA manifest version or firmware size is invalid");
    if (!validLowerSha256(manifest.sha256))
        return reject(error, "OTA manifest SHA-256 is invalid");
    if (!validSignatureEncoding(manifest.signature))
        return reject(error, "OTA manifest signature is invalid");
    if (!resolveFirmwareUrl(expected.infoUrl, manifest.firmware,
                            manifest.resolvedFirmwareUrl))
        return reject(error, "OTA firmware URL is invalid");
    return true;
}

bool digestMatches(const uint8_t digest[32], const String& expectedHex) {
    static const char hex[] = "0123456789abcdef";
    if (expectedHex.length() != 64) return false;
    for (int i = 0; i < 32; ++i) {
        if (expectedHex[i * 2] != hex[digest[i] >> 4] ||
            expectedHex[i * 2 + 1] != hex[digest[i] & 0x0F]) return false;
    }
    return true;
}

} // namespace OtaManifest
