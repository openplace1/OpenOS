// Host-side regression tests for the portable parsers in src/Runtime.
//
// Build and run with any C++11 compiler: see README.md in this directory
// (run.sh / run.bat wrap the one-line g++ invocation).
//
// No test framework: each CHECK prints its location on failure and the
// process exit code is the number of failed checks.

#include "CatalogSignature.h"
#include "OpenOSTrustAnchors.h"
#include "OtaManifest.h"
#include "UrlUtil.h"

#include <stdio.h>
#include <string.h>

#include "manifest_fixture.inc"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) do { \
    ++g_checks; \
    if (!(condition)) { \
        ++g_failures; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

#define CHECK_EQ_STR(actual, expected) do { \
    ++g_checks; \
    String a_ = (actual); \
    if (!(a_ == (expected))) { \
        ++g_failures; \
        printf("FAIL %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, \
               String(expected).c_str(), a_.c_str()); \
    } \
} while (0)

static OtaManifest::Expectations officialExpectations() {
    OtaManifest::Expectations expected;
    expected.target = INFO_EXPECTED_TARGET;
    expected.partitionScheme = INFO_EXPECTED_PARTITION;
    expected.keyId = INFO_EXPECTED_KEY_ID;
    expected.officialFeed = true;
    expected.slotBytes = 0x1F0000;
    expected.infoUrl = "https://raw.githubusercontent.com/openplace1/OpenStore/main/update/info.json";
    return expected;
}

static bool parseOk(const char* json, OtaManifest::Manifest& manifest) {
    String error;
    return OtaManifest::parse(String(json), manifest, error);
}

static String withField(const char* field, const char* replacement) {
    // Replaces the whole `"field": value,` line of the published manifest.
    String json(INFO_JSON);
    String needle = String("  \"") + field + "\": ";
    int start = json.indexOf(needle.c_str());
    if (start < 0) return json;
    int end = json.indexOf('\n', start);
    String result = json.substring(0, start);
    result += replacement;
    result += json.substring(end);
    return result;
}

static void testPublishedManifestParsesAndCanonicalises() {
    OtaManifest::Manifest manifest;
    String error;
    CHECK(OtaManifest::parse(String(INFO_JSON), manifest, error));
    CHECK_EQ_STR(error, "");
    CHECK(manifest.schema == 1);
    CHECK_EQ_STR(manifest.product, "openos");
    CHECK_EQ_STR(manifest.name, INFO_EXPECTED_NAME);
    CHECK(manifest.versionCode == INFO_EXPECTED_VERSION_CODE);
    CHECK(manifest.size == INFO_EXPECTED_SIZE);
    CHECK_EQ_STR(manifest.firmware, INFO_EXPECTED_FIRMWARE);
    // UTF-8 passes through untouched (the description holds Polish letters:
    // "\xc5\x82" is the UTF-8 encoding of the "l with stroke").
    CHECK(manifest.description.indexOf("\xc5\x82") >= 0);

    String payload;
    CHECK(OtaManifest::canonicalPayload(manifest, payload));
    CHECK_EQ_STR(payload, INFO_CANONICAL);

    OtaManifest::Expectations expected = officialExpectations();
    CHECK(OtaManifest::validateFields(manifest, expected, error));
    CHECK_EQ_STR(manifest.resolvedFirmwareUrl,
                 String("https://raw.githubusercontent.com/openplace1/OpenStore/main/update/") +
                 INFO_EXPECTED_FIRMWARE);
}

static void testParserRejectsWhatTheDeviceMustNotAccept() {
    OtaManifest::Manifest manifest;
    String error;
    struct Case { const char* label; String json; const char* expectedError; };
    const Case cases[] = {
        { "duplicate field", withField("schema", "  \"schema\": 1, \"schema\": 1,"),
          "Duplicate OTA manifest field: schema" },
        { "unknown field", withField("channel", "  \"channel\": \"stable\", \"extra\": 1,"),
          "Unknown OTA manifest field: extra" },
        { "\\u escape", withField("name", "  \"name\": \"Open\\u004fS\","),
          "Invalid string OTA field: name" },
        { "missing field", withField("publishedAt", ""),
          "OTA manifest is missing required fields" },
        { "leading zero", withField("versionCode", "  \"versionCode\": 02,"),
          "Invalid numeric OTA field: versionCode" },
        { "32-bit overflow", withField("size", "  \"size\": 4294967296,"),
          "Invalid numeric OTA field: size" },
        { "string where number expected", withField("size", "  \"size\": \"2007104\","),
          "Invalid numeric OTA field: size" },
        { "trailing data", String(INFO_JSON) + "x", "OTA manifest has trailing data" },
        { "not an object", String("[1]"), "OTA manifest must be a JSON object" },
    };
    for (const Case& c : cases) {
        manifest = OtaManifest::Manifest();
        error = "";
        bool ok = OtaManifest::parse(c.json, manifest, error);
        if (ok) printf("  (%s) unexpectedly parsed\n", c.label);
        CHECK(!ok);
        CHECK_EQ_STR(error, c.expectedError);
    }

    // A trailing comma is rejected before the closing brace.
    String trailing = String(INFO_JSON);
    int lastQuote = trailing.lastIndexOf('"');
    trailing = trailing.substring(0, lastQuote + 1) + ",\n}\n";
    CHECK(!parseOk(trailing.c_str(), manifest));
}

static void testFieldValidation() {
    OtaManifest::Manifest manifest;
    String error;
    CHECK(parseOk(INFO_JSON, manifest));

    OtaManifest::Expectations expected = officialExpectations();
    OtaManifest::Manifest copy = manifest;
    expected.target = "other-board";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA release targets different hardware or partitions");

    expected = officialExpectations();
    copy = manifest;
    copy.channel = "beta";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "The official OTA feed only accepts stable releases");
    expected.officialFeed = false;
    expected.infoUrl = "https://example.invalid/feed/info.json?x=1";
    CHECK(OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(copy.resolvedFirmwareUrl,
                 String("https://example.invalid/feed/") + INFO_EXPECTED_FIRMWARE);

    expected = officialExpectations();
    copy = manifest;
    copy.size = 0x1F0000 + 1;
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA manifest version or firmware size is invalid");

    copy = manifest;
    copy.sha256 = "ABCDEF";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA manifest SHA-256 is invalid");

    copy = manifest;
    copy.signature = "not base64!";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA manifest signature is invalid");

    copy = manifest;
    copy.name = "bad\rname";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA manifest text fields are invalid");

    copy = manifest;
    copy.keyId = "openos-release-1999-01";
    CHECK(!OtaManifest::validateFields(copy, expected, error));
    CHECK_EQ_STR(error, "OTA manifest uses an unknown release key");
}

static void testFirmwareUrlResolution() {
    String resolved;
    const String info = "https://host.example/dir/info.json";
    CHECK(OtaManifest::resolveFirmwareUrl(info, "fw.bin", resolved));
    CHECK_EQ_STR(resolved, "https://host.example/dir/fw.bin");
    CHECK(OtaManifest::resolveFirmwareUrl(info + "?openos=1", "sub/fw.bin", resolved));
    CHECK_EQ_STR(resolved, "https://host.example/dir/sub/fw.bin");
    CHECK(OtaManifest::resolveFirmwareUrl(info, "https://cdn.example/x/fw.bin", resolved));
    CHECK_EQ_STR(resolved, "https://cdn.example/x/fw.bin");
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "../fw.bin", resolved));
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "/fw.bin", resolved));
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "a\\b.bin", resolved));
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "fw.bin?x", resolved));
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "http://cdn.example/fw.bin", resolved));
    CHECK(!OtaManifest::resolveFirmwareUrl(info, "https://user@cdn.example/fw.bin", resolved));
    CHECK(!OtaManifest::validHttpsUrl("https://host.example/with space"));
    CHECK(!OtaManifest::validHttpsUrl("https:///nohost"));
}

static void testCatalogSignatureFraming() {
    const String unsigned_ = "{\"schema\":1,\"apps\":[{\"id\":\"a.b\",\"name\":\"q\\\"x\"}],\"keyId\":\"k\"}";
    const String signature = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    String signed_ = unsigned_.substring(0, unsigned_.length() - 1) +
                     ",\"signature\":\"" + signature + "\"}\n";
    size_t signedLength = 0;
    String extracted, error;
    CHECK(CatalogSignature::split(signed_, signedLength, extracted, error));
    CHECK_EQ_STR(extracted, signature);
    // The hashed text is document[0:signedLength] + "}" == the unsigned object.
    CHECK_EQ_STR(signed_.substring(0, signedLength) + "}", unsigned_);
    CHECK(CatalogSignature::split(signed_ + "\r\n  \n", signedLength, extracted, error));

    CHECK(!CatalogSignature::split(unsigned_ + "\n", signedLength, extracted, error));
    CHECK_EQ_STR(error, "Store catalog is not signed");
    String spaced = signed_;
    spaced = spaced.substring(0, spaced.length() - 2) + " }\n";
    CHECK(!CatalogSignature::split(spaced, signedLength, extracted, error));
    CHECK_EQ_STR(error, "Store catalog signature field is malformed");
    String notLast = signed_.substring(0, signed_.length() - 2) + ",\"x\":1}\n";
    CHECK(!CatalogSignature::split(notLast, signedLength, extracted, error));
    CHECK(!CatalogSignature::split(String(""), signedLength, extracted, error));
}

static void testUrlAndAnchors() {
    CHECK_EQ_STR(UrlUtil::hostFromHttpsUrl("https://raw.githubusercontent.com/a/b?c"),
                 "raw.githubusercontent.com");
    CHECK_EQ_STR(UrlUtil::hostFromHttpsUrl("https://user:pw@host.example:8443/x"), "host.example");
    CHECK_EQ_STR(UrlUtil::hostFromHttpsUrl("https://host.example"), "host.example");
    CHECK_EQ_STR(UrlUtil::hostFromHttpsUrl("http://host.example/"), "");

    CHECK(OpenOSTrustAnchors::forHost("raw.githubusercontent.com") != nullptr);
    CHECK(OpenOSTrustAnchors::forHost("objects.githubusercontent.com") != nullptr);
    CHECK(OpenOSTrustAnchors::forHost("RAW.GitHubUserContent.com") != nullptr);
    CHECK(OpenOSTrustAnchors::forHost("githubusercontent.com") != nullptr);
    CHECK(OpenOSTrustAnchors::forHost("evilgithubusercontent.com") == nullptr);
    CHECK(OpenOSTrustAnchors::forHost("github.com") == nullptr);
    CHECK(OpenOSTrustAnchors::forHost("") == nullptr);
    CHECK(OpenOSTrustAnchors::forHost(nullptr) == nullptr);
    CHECK(strstr(OpenOSTrustAnchors::ISRG_ROOT_X1, "-----BEGIN CERTIFICATE-----") ==
          OpenOSTrustAnchors::ISRG_ROOT_X1);
}

static void testDigestMatches() {
    uint8_t digest[32];
    memset(digest, 0, sizeof(digest));
    digest[0] = 0xab;
    digest[31] = 0x01;
    String hex = "ab";
    for (int i = 1; i < 31; ++i) hex += "00";
    hex += "01";
    CHECK(OtaManifest::digestMatches(digest, hex));
    CHECK(!OtaManifest::digestMatches(digest, hex.substring(0, 63)));
    String upper = "AB" + hex.substring(2);
    CHECK(!OtaManifest::digestMatches(digest, upper));
}

int main() {
    testPublishedManifestParsesAndCanonicalises();
    testParserRejectsWhatTheDeviceMustNotAccept();
    testFieldValidation();
    testFirmwareUrlResolution();
    testCatalogSignatureFraming();
    testUrlAndAnchors();
    testDigestMatches();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
