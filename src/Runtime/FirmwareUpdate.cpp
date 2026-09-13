#include "FirmwareUpdate.h"

#include "HeapReserve.h"
#include "OpenOSReleaseKeys.h"
#include "OtaManifest.h"
#include "ReleaseSignature.h"
#include "SecureHttp.h"
#include "../OpenOSVersion.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

namespace FirmwareUpdate {
namespace {

static constexpr size_t MANIFEST_MAX_BYTES = OtaManifest::MAX_BYTES;
static constexpr uint32_t OTA_SLOT_BYTES = 0x1F0000;
static constexpr uint32_t BOOT_PROBATION_MS = 8000;
static constexpr const char* PREF_NAMESPACE = "openos-ota";
static constexpr const char* PREF_SOURCE_KEY = "info_url";

using OtaManifest::Manifest;
using OtaManifest::validHttpsUrl;

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

static bool downloadManifest(String& document) {
    SecureHttp::prepareMemory("OTA", "manifest HTTPS");
    SecureHttp::RadioAwake awake;
    String why;
    if (!SecureHttp::memoryAvailable(why)) return fail(why);
    String requestUrl = sourceUrl();
    if (!validHttpsUrl(requestUrl)) return fail("OTA source must be a valid HTTPS URL");
    if (requestUrl.startsWith("https://raw.githubusercontent.com/")) {
        requestUrl += requestUrl.indexOf('?') >= 0 ? '&' : '?';
        requestUrl += "openos=";
        requestUrl += millis();
    }

    HTTPClient http;
    WiFiClientSecure client;
    SecureHttp::Request request;
    request.attempts = 3;
    int status = SecureHttp::get(http, client, requestUrl, request, why);
    if (status != HTTP_CODE_OK) {
        String message = status < 0
            ? String("OTA manifest download failed: ") + why
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

static bool verifyManifestSignature(const Manifest& manifest) {
    String payload;
    if (!OtaManifest::canonicalPayload(manifest, payload))
        return fail("Not enough RAM to verify OTA signature");
    String why;
    if (!ReleaseSignature::verify((const uint8_t*)payload.c_str(),
                                  payload.length(), manifest.signature, why))
        return fail(String("OTA manifest ") + why);
    return true;
}

static bool validateManifest(Manifest& manifest) {
    OtaManifest::Expectations expected;
    expected.target = OpenOSBuild::OTA_TARGET;
    expected.partitionScheme = OpenOSBuild::OTA_PARTITION_SCHEME;
    expected.keyId = OpenOSReleaseKeys::KEY_ID;
    expected.infoUrl = sourceUrl();
    expected.officialFeed = expected.infoUrl == DEFAULT_INFO_URL;
    expected.slotBytes = OTA_SLOT_BYTES;
    String error;
    if (!OtaManifest::validateFields(manifest, expected, error)) return fail(error);
    return verifyManifestSignature(manifest);
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
    // Heap is probed inside downloadManifest, after Bluetooth was suspended,
    // so the figure reflects what the TLS handshake will actually get.
    String document;
    bool downloaded = downloadManifest(document);
    HeapReserve::reclaim();
    if (!downloaded) return -1;
    Manifest candidate;
    String error;
    if (!OtaManifest::parse(document, candidate, error)) {
        fail(error);
        return -1;
    }
    if (!validateManifest(candidate)) return -1;
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

bool checked() { return s_manifestReady; }

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

    SecureHttp::prepareMemory("OTA", "firmware HTTPS");
    SecureHttp::RadioAwake awake;
    String why;
    if (!SecureHttp::memoryAvailable(why)) return fail(why);
    // The TLS session stays open for the whole 2 MB stream while the 4 KB
    // transfer buffer and Update's own sector buffer are live next to it.
    if (ESP.getFreeHeap() < SecureHttp::TLS_MIN_FREE_BYTES + 10U * 1024U)
        return fail(String("Not enough RAM to install firmware (free ") +
                    (unsigned)(ESP.getFreeHeap() / 1024U) + " KB, need " +
                    (unsigned)(SecureHttp::TLS_MIN_FREE_BYTES / 1024U + 10U) +
                    " KB)");
    if (!validHttpsUrl(s_manifest.resolvedFirmwareUrl))
        return fail("OTA firmware URL is invalid");

    // Both 4 KB buffers are claimed before the TLS session exists. Update's
    // own sector buffer used to be allocated after the handshake, when the
    // record buffers had taken the large region, and the Arduino library
    // reports that failure as a begin() returning false with no error set.
    uint8_t* buffer = (uint8_t*)malloc(4096);
    if (!buffer) return fail("Not enough contiguous RAM for the OTA buffer");
    if (progress) progress("Preparing", 0, s_manifest.size, context);
    if (!Update.begin(s_manifest.size, U_FLASH)) {
        String detail = Update.errorString();
        // UPDATE_ERROR_OK here means begin() bailed out without recording a
        // reason; the only such path is its own 4 KB allocation failing.
        if (detail == "No Error")
            detail = String("not enough contiguous RAM for the flash writer (free ") +
                     (unsigned)(ESP.getFreeHeap() / 1024U) + " KB)";
        free(buffer);
        return fail(String("Could not start OTA: ") + detail);
    }

    HTTPClient http;
    WiFiClientSecure client;
    SecureHttp::Request request;
    request.attempts = 2;
    request.readTimeoutMs = 20000;
    int status = SecureHttp::get(http, client, s_manifest.resolvedFirmwareUrl,
                                 request, why);
    if (status != HTTP_CODE_OK) {
        String message = status < 0
            ? String("Firmware download failed: ") + why
            : String("Firmware HTTP error ") + status;
        http.end();
        free(buffer);
        Update.abort();
        return fail(message);
    }
    int declared = http.getSize();
    if (declared < 0 || (uint32_t)declared != s_manifest.size) {
        http.end();
        free(buffer);
        Update.abort();
        return fail("Firmware Content-Length does not match signed metadata");
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts_ret(&sha, 0) != 0) {
        free(buffer);
        http.end();
        mbedtls_sha256_free(&sha);
        Update.abort();
        return fail("Could not initialize firmware SHA-256");
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
        HeapReserve::reclaim();
        return fail(transferError.length() > 0 ? transferError
                                               : String("Could not finish firmware SHA-256"));
    }
    if (received != s_manifest.size ||
        !OtaManifest::digestMatches(digest, s_manifest.sha256)) {
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
