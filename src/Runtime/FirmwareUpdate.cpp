#include "FirmwareUpdate.h"

#include "OpenOSReleaseKeys.h"
#include "../OpenOSVersion.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>

extern bool sysBTEnabled;
extern bool osaSuspendBluetoothForMemory(const char* reason);

namespace FirmwareUpdate {
namespace {

static constexpr size_t MANIFEST_MAX_BYTES = 4096;
static constexpr size_t DESCRIPTION_MAX_BYTES = 1200;
static constexpr size_t URL_MAX_BYTES = 2048;
static constexpr uint32_t OTA_SLOT_BYTES = 0x1F0000;
static constexpr uint32_t BOOT_PROBATION_MS = 8000;
static constexpr const char* PREF_NAMESPACE = "openos-ota";
static constexpr const char* PREF_SOURCE_KEY = "info_url";

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

static Manifest s_manifest;
static bool s_manifestReady = false;
static bool s_updateAvailable = false;
static String s_error;
static bool s_bootPending = false;
static uint32_t s_bootProbationStarted = 0;

static bool fail(const String& message) {
    s_error = message;
    Serial.printf("[OTA] %s\n", message.c_str());
    return false;
}

static void clearManifest() {
    s_manifest = Manifest();
    s_manifestReady = false;
    s_updateAvailable = false;
}

static bool validHttpsUrl(const String& url) {
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

static bool validRelativeFirmwarePath(const String& path) {
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

static bool resolveFirmwareUrl(const String& infoUrl, const String& firmware,
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

class BoundedStringStream : public Stream {
public:
    explicit BoundedStringStream(size_t maximum) : limit(maximum) {}

    bool reserve(size_t requested) {
        if (requested > limit) requested = limit;
        return requested == 0 || data.reserve(requested);
    }
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* source, size_t count) override {
        if (overflow || count > limit - data.length()) {
            overflow = true;
            setWriteError();
            return 0;
        }
        if (count > 0 && !data.concat((const char*)source, count)) {
            oom = true;
            setWriteError();
            return 0;
        }
        return count;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    bool tooLarge() const { return overflow; }
    bool outOfMemory() const { return oom; }
    String take() { return static_cast<String&&>(data); }

private:
    String data;
    size_t limit;
    bool overflow = false;
    bool oom = false;
};

static void prepareHttpsMemory(const char* operation) {
    if (sysBTEnabled) osaSuspendBluetoothForMemory(operation);
    Serial.printf("[OTA] %s free=%u maxBlock=%u\n", operation,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool beginHttps(HTTPClient& http, WiFiClientSecure& client,
                       const String& url) {
    if (!validHttpsUrl(url)) return fail("OTA source must be a valid HTTPS URL");
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    // Authenticity does not depend on this transport setting: info.json is
    // ECDSA-signed by the release key embedded below, and its signed SHA-256
    // authenticates every firmware byte. TLS still protects passive privacy.
    client.setInsecure();
    return http.begin(client, url) || fail("Could not open OTA HTTPS connection");
}

static bool downloadManifest(String& document) {
    prepareHttpsMemory("manifest HTTPS");
    String requestUrl = sourceUrl();
    if (requestUrl.startsWith("https://raw.githubusercontent.com/")) {
        requestUrl += requestUrl.indexOf('?') >= 0 ? '&' : '?';
        requestUrl += "openos=";
        requestUrl += millis();
    }

    HTTPClient http;
    WiFiClientSecure client;
    if (!beginHttps(http, client, requestUrl)) return false;
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        String message = status < 0
            ? String("OTA manifest HTTPS failed: ") + HTTPClient::errorToString(status)
            : String("OTA manifest HTTP error ") + status;
        http.end();
        return fail(message);
    }
    int declared = http.getSize();
    if (declared > (int)MANIFEST_MAX_BYTES) {
        http.end();
        return fail("OTA manifest exceeds 4 KB");
    }
    BoundedStringStream output(MANIFEST_MAX_BYTES);
    if (!output.reserve(declared > 0 ? (size_t)declared : 768)) {
        http.end();
        return fail("Not enough RAM for OTA manifest");
    }
    int received = http.writeToStream(&output);
    http.end();
    if (output.tooLarge()) return fail("OTA manifest exceeds 4 KB");
    if (output.outOfMemory()) return fail("Not enough RAM for OTA manifest");
    if (received < 0) return fail("Could not read OTA manifest");
    document = output.take();
    return document.length() > 0 || fail("OTA manifest is empty");
}

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

static bool parseManifestJson(const String& json, Manifest& manifest) {
    int position = 0;
    skipWs(json, position);
    if (position >= (int)json.length() || json[position++] != '{')
        return fail("OTA manifest must be a JSON object");
    uint32_t seen = 0;
    skipWs(json, position);
    while (position < (int)json.length() && json[position] != '}') {
        String key;
        if (!parseJsonString(json, position, key))
            return fail("OTA manifest contains an invalid key");
        int field = manifestField(key);
        if (field < 0) return fail(String("Unknown OTA manifest field: ") + key);
        uint32_t bit = 1UL << field;
        if (seen & bit) return fail(String("Duplicate OTA manifest field: ") + key);
        seen |= bit;
        skipWs(json, position);
        if (position >= (int)json.length() || json[position++] != ':')
            return fail("OTA manifest is missing ':'");
        skipWs(json, position);
        if (numericField(field)) {
            uint32_t number = 0;
            if (!parseJsonUInt(json, position, number) ||
                !assignNumberField(manifest, field, number))
                return fail(String("Invalid numeric OTA field: ") + key);
        } else {
            String value;
            if (!parseJsonString(json, position, value) ||
                !assignStringField(manifest, field, static_cast<String&&>(value)))
                return fail(String("Invalid string OTA field: ") + key);
        }
        skipWs(json, position);
        if (position < (int)json.length() && json[position] == ',') {
            ++position;
            skipWs(json, position);
            if (position < (int)json.length() && json[position] == '}')
                return fail("OTA manifest has a trailing comma");
            continue;
        }
        break;
    }
    if (position >= (int)json.length() || json[position++] != '}')
        return fail("OTA manifest JSON is incomplete");
    skipWs(json, position);
    if (position != (int)json.length()) return fail("OTA manifest has trailing data");
    const uint32_t required = (1UL << MF_COUNT) - 1UL;
    if (seen != required) return fail("OTA manifest is missing required fields");
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

static bool canonicalPayload(const Manifest& manifest, String& payload) {
    if (!payload.reserve(512 + manifest.description.length() +
                         manifest.firmware.length())) return false;
    payload = "OPENOS-OTA-V1\n";
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

static bool verifyManifestSignature(const Manifest& manifest) {
    String payload;
    if (!canonicalPayload(manifest, payload))
        return fail("Not enough RAM to verify OTA signature");

    uint8_t digest[32];
    if (mbedtls_sha256_ret((const uint8_t*)payload.c_str(), payload.length(),
                           digest, 0) != 0)
        return fail("Could not hash OTA manifest");

    uint8_t signature[80];
    size_t signatureLength = 0;
    int result = mbedtls_base64_decode(signature, sizeof(signature),
                                       &signatureLength,
                                       (const uint8_t*)manifest.signature.c_str(),
                                       manifest.signature.length());
    if (result != 0 || signatureLength < 64 || signatureLength > sizeof(signature))
        return fail("OTA manifest signature encoding is invalid");

    mbedtls_ecdsa_context context;
    mbedtls_ecdsa_init(&context);
    result = mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0)
        result = mbedtls_ecp_point_read_binary(
            &context.grp, &context.Q, OpenOSReleaseKeys::P256_PUBLIC_KEY,
            sizeof(OpenOSReleaseKeys::P256_PUBLIC_KEY));
    if (result == 0)
        result = mbedtls_ecdsa_read_signature(&context, digest, sizeof(digest),
                                              signature, signatureLength);
    mbedtls_ecdsa_free(&context);
    return result == 0 || fail("OTA manifest signature is not trusted");
}

static bool validateManifest(Manifest& manifest) {
    if (manifest.schema != 1 || manifest.product != "openos")
        return fail("Unsupported OTA manifest schema or product");
    if (manifest.target != OpenOSBuild::OTA_TARGET ||
        manifest.partitionScheme != OpenOSBuild::OTA_PARTITION_SCHEME)
        return fail("OTA release targets different hardware or partitions");
    if (manifest.keyId != OpenOSReleaseKeys::KEY_ID)
        return fail("OTA manifest uses an unknown release key");
    if (manifest.channel != "stable" && manifest.channel != "beta" &&
        manifest.channel != "dev")
        return fail("OTA manifest has an invalid channel");
    if (sourceUrl() == DEFAULT_INFO_URL && manifest.channel != "stable")
        return fail("The official OTA feed only accepts stable releases");
    if (manifest.type != "major" && manifest.type != "minor" &&
        manifest.type != "patch" && manifest.type != "security")
        return fail("OTA manifest has an invalid release type");
    if (manifest.name.length() < 1 || manifest.name.length() > 80 ||
        manifest.version.length() < 1 || manifest.version.length() > 24 ||
        manifest.published.length() < 1 || manifest.published.length() > 40 ||
        manifest.description.length() > DESCRIPTION_MAX_BYTES ||
        hasControl(manifest.name) || hasControl(manifest.version) ||
        hasControl(manifest.published) || hasControl(manifest.description, true))
        return fail("OTA manifest text fields are invalid");
    if (manifest.versionCode < 1 || manifest.minUpdaterVersionCode < 1 ||
        manifest.size < 4096 || manifest.size > OTA_SLOT_BYTES)
        return fail("OTA manifest version or firmware size is invalid");
    if (!validLowerSha256(manifest.sha256))
        return fail("OTA manifest SHA-256 is invalid");
    if (manifest.signature.length() < 80 || manifest.signature.length() > 120 ||
        hasControl(manifest.signature))
        return fail("OTA manifest signature is invalid");
    if (!resolveFirmwareUrl(sourceUrl(), manifest.firmware,
                            manifest.resolvedFirmwareUrl))
        return fail("OTA firmware URL is invalid");
    return verifyManifestSignature(manifest);
}

static bool digestMatches(const uint8_t digest[32], const String& expected) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        if (expected[i * 2] != hex[digest[i] >> 4] ||
            expected[i * 2 + 1] != hex[digest[i] & 0x0F]) return false;
    }
    return true;
}

} // namespace

void begin() {
    s_error = "";
    clearManifest();
    Serial.printf("[OTA] %s\n", OpenOSBuild::OTA_IMAGE_MARKER);
}

String sourceUrl() {
    Preferences preferences;
    if (!preferences.begin(PREF_NAMESPACE, true)) return String(DEFAULT_INFO_URL);
    String configured = preferences.getString(PREF_SOURCE_KEY, "");
    preferences.end();
    configured.trim();
    return configured.length() > 0 ? configured : String(DEFAULT_INFO_URL);
}

bool setSourceUrl(const String& requested) {
    s_error = "";
    String source = requested;
    source.trim();
    if (source.length() > 0 && !validHttpsUrl(source))
        return fail("OTA source must be a valid HTTPS info.json URL");
    Preferences preferences;
    if (!preferences.begin(PREF_NAMESPACE, false))
        return fail("Could not open OTA source settings");
    bool ok = source.length() == 0
        ? preferences.remove(PREF_SOURCE_KEY)
        : preferences.putString(PREF_SOURCE_KEY, source) == source.length();
    preferences.end();
    if (!ok && source.length() > 0) return fail("Could not save OTA source");
    clearManifest();
    return true;
}

int check() {
    s_error = "";
    clearManifest();
    if (WiFi.status() != WL_CONNECTED) {
        fail("Wi-Fi is not connected");
        return -1;
    }
    if (ESP.getFreeHeap() < 30U * 1024U ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16U * 1024U) {
        fail("Not enough RAM to check for updates");
        return -1;
    }
    String document;
    if (!downloadManifest(document)) return -1;
    Manifest candidate;
    if (!parseManifestJson(document, candidate) || !validateManifest(candidate))
        return -1;
    if (candidate.minUpdaterVersionCode >
        (uint32_t)OpenOSBuild::OTA_UPDATER_VERSION_CODE) {
        fail("This update requires a newer OTA updater");
        return -1;
    }
    s_manifest = static_cast<Manifest&&>(candidate);
    s_manifestReady = true;
    s_updateAvailable = s_manifest.versionCode > (uint32_t)OpenOSBuild::VERSION_CODE;
    return s_updateAvailable ? 1 : 0;
}

bool available() { return s_manifestReady && s_updateAvailable; }

bool supported() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!running || !next || running == next) return false;
    bool runningSlot = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
                       running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1;
    bool nextSlot = next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
                    next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1;
    return runningSlot && nextSlot && running->size == OTA_SLOT_BYTES &&
           next->size == OTA_SLOT_BYTES;
}

String remoteName() { return s_manifestReady ? s_manifest.name : String(); }
String remoteVersion() { return s_manifestReady ? s_manifest.version : String(); }
int remoteVersionCode() { return s_manifestReady ? (int)s_manifest.versionCode : 0; }
String releaseChannel() { return s_manifestReady ? s_manifest.channel : String(); }
String releaseType() { return s_manifestReady ? s_manifest.type : String(); }
String releaseDescription() {
    return s_manifestReady ? s_manifest.description : String();
}
String publishedAt() { return s_manifestReady ? s_manifest.published : String(); }
size_t downloadSize() { return s_manifestReady ? (size_t)s_manifest.size : 0; }

bool install(ProgressCallback progress, void* context) {
    s_error = "";
    if (!s_manifestReady || !s_updateAvailable)
        return fail("Check for a newer signed update first");
    if (!supported())
        return fail("USB migration to the dual-slot partition layout is required");
    if (WiFi.status() != WL_CONNECTED) return fail("Wi-Fi is not connected");
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next || s_manifest.size > next->size)
        return fail("Firmware does not fit the inactive OTA slot");

    prepareHttpsMemory("firmware HTTPS");
    if (ESP.getFreeHeap() < 34U * 1024U ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 18U * 1024U)
        return fail("Not enough RAM to install firmware");

    HTTPClient http;
    WiFiClientSecure client;
    if (!beginHttps(http, client, s_manifest.resolvedFirmwareUrl)) return false;
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        String message = status < 0
            ? String("Firmware HTTPS failed: ") + HTTPClient::errorToString(status)
            : String("Firmware HTTP error ") + status;
        http.end();
        return fail(message);
    }
    int declared = http.getSize();
    if (declared < 0 || (uint32_t)declared != s_manifest.size) {
        http.end();
        return fail("Firmware Content-Length does not match signed metadata");
    }

    uint8_t* buffer = (uint8_t*)malloc(4096);
    if (!buffer) {
        http.end();
        return fail("Not enough contiguous RAM for OTA buffer");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool shaReady = mbedtls_sha256_starts_ret(&sha, 0) == 0;
    if (!shaReady) {
        free(buffer);
        http.end();
        mbedtls_sha256_free(&sha);
        return fail("Could not initialize firmware SHA-256");
    }

    if (progress) progress("Preparing", 0, s_manifest.size, context);
    bool updateStarted = Update.begin(s_manifest.size, U_FLASH);
    if (!updateStarted) {
        String message = String("Could not start OTA: ") + Update.errorString();
        free(buffer);
        http.end();
        mbedtls_sha256_free(&sha);
        return fail(message);
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t received = 0;
    uint32_t lastDataAt = millis();
    String transferError;
    while (received < s_manifest.size) {
        int availableBytes = stream ? stream->available() : 0;
        if (availableBytes <= 0) {
            if (!http.connected()) {
                transferError = "Firmware transfer ended early";
                break;
            }
            if ((uint32_t)(millis() - lastDataAt) > 15000U) {
                transferError = "Firmware transfer timed out";
                break;
            }
            delay(2);
            yield();
            continue;
        }
        size_t wanted = min((size_t)availableBytes,
                            min((size_t)4096, (size_t)s_manifest.size - received));
        int count = stream->readBytes(buffer, wanted);
        if (count <= 0) continue;
        lastDataAt = millis();
        if (mbedtls_sha256_update_ret(&sha, buffer, (size_t)count) != 0) {
            transferError = "Could not hash downloaded firmware";
            break;
        }
        size_t written = Update.write(buffer, (size_t)count);
        if (written != (size_t)count) {
            transferError = String("Could not write OTA slot: ") + Update.errorString();
            break;
        }
        received += (size_t)count;
        if (progress) progress("Installing", received, s_manifest.size, context);
        yield();
    }

    uint8_t digest[32];
    bool hashFinished = transferError.length() == 0 &&
        mbedtls_sha256_finish_ret(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    free(buffer);
    http.end();

    if (!hashFinished) {
        Update.abort();
        return fail(transferError.length() > 0 ? transferError
                                               : String("Could not finish firmware SHA-256"));
    }
    if (received != s_manifest.size || !digestMatches(digest, s_manifest.sha256)) {
        Update.abort();
        return fail("Firmware SHA-256 does not match the signed manifest");
    }
    if (progress) progress("Finalizing", s_manifest.size, s_manifest.size, context);
    if (!Update.end(false) || !Update.isFinished()) {
        String message = String("Could not activate OTA image: ") + Update.errorString();
        Update.abort();
        return fail(message);
    }
    return true;
}

bool canRollback() { return Update.canRollBack(); }

bool rollback() {
    s_error = "";
    if (!Update.canRollBack()) return fail("No bootable previous firmware is available");
    return Update.rollBack() || fail(String("Could not select previous firmware: ") +
                                     Update.errorString());
}

String lastError() { return s_error; }

void beginBootValidation() {
    s_bootPending = false;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_bootPending = true;
        s_bootProbationStarted = millis();
        Serial.println("[OTA] new image is in boot probation");
    }
}

void pollBootValidation() {
    if (!s_bootPending ||
        (uint32_t)(millis() - s_bootProbationStarted) < BOOT_PROBATION_MS) return;
    if (ESP.getFreeHeap() < 24U * 1024U) return;
    esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
        s_bootPending = false;
        Serial.println("[OTA] boot probation passed; image marked valid");
    } else {
        Serial.printf("[OTA] could not validate image: %d\n", (int)result);
    }
}

bool pendingBootValidation() { return s_bootPending; }

} // namespace FirmwareUpdate
