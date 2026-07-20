#include "PackageManager.h"
#include "../Config.h"

#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp32/rom/miniz.h>
#include <mbedtls/sha256.h>
#include <new>

extern bool isSdReady;
extern bool sysBTEnabled;
extern bool osaSuspendBluetoothForMemory(const char* reason);

namespace PackageManager {
namespace {

static constexpr size_t OPK_MAX_PACKAGE_BYTES = 8U * 1024U * 1024U;
static constexpr size_t OPK_MAX_TOTAL_BYTES   = 8U * 1024U * 1024U;
static constexpr size_t OPK_MAX_FILE_BYTES    = 2U * 1024U * 1024U;
static constexpr size_t OPK_MAX_MANIFEST      = 8192;
static constexpr int    OPK_MAX_ENTRIES       = 64;
static constexpr size_t CATALOG_MAX_BYTES     = 24U * 1024U;
static constexpr int    CATALOG_MAX_ENTRIES   = 64;

static const char* DOWNLOAD_PATH = "/system/.openstore-download.opk";
static const char* MANIFEST_TMP  = "/system/.openstore-manifest.tmp";
static const char* CATALOG_CONFIG_KEY = "store_catalog_url";
static const char* SYSTEM_PREFIX_CONFIG_KEY = "store_system_prefix";

static String s_error;
static bool s_restartRequired = false;
static String s_catalog;
static int s_catalogCount = 0;
static int s_catalogItemStart[CATALOG_MAX_ENTRIES];
static int s_catalogItemEnd[CATALOG_MAX_ENTRIES];
static bool s_catalogIndexReady = false;
static uint32_t s_installedIdHash[CATALOG_MAX_ENTRIES];
static int s_installedCode[CATALOG_MAX_ENTRIES];
static bool s_installedCacheReady = false;

static void releaseCatalogStorage() {
    s_catalog.~String();
    new (&s_catalog) String();
    s_catalogCount = 0;
    s_catalogIndexReady = false;
    s_installedCacheReady = false;
}

static bool fail(const String& message) {
    s_error = message;
    return false;
}

static void removeFileIfPresent(const char* path) {
    if (SD.exists(path)) SD.remove(path);
}

static bool validHttpsSource(const String& url, bool prefix) {
    if (!url.startsWith("https://") || url.length() < 9 || url.length() > 2048 ||
        url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0 ||
        url.indexOf(' ') >= 0) return false;
    if (prefix && (url.indexOf('?') >= 0 || url.indexOf('#') >= 0)) return false;
    return true;
}

static bool normalizeSystemPrefix(String& prefix) {
    prefix.trim();
    if (!validHttpsSource(prefix, true)) return false;
    if (!prefix.endsWith("/")) {
        if (prefix.length() >= 2048 || !prefix.concat('/')) return false;
    }
    return true;
}

static bool readExact(File& file, void* destination, size_t length) {
    return length == 0 || file.read((uint8_t*)destination, length) == (int)length;
}

static bool readU16(File& file, uint16_t& value) {
    uint8_t b[2];
    if (!readExact(file, b, sizeof(b))) return false;
    value = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

static bool readU32(File& file, uint32_t& value) {
    uint8_t b[4];
    if (!readExact(file, b, sizeof(b))) return false;
    value = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
            ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static String baseName(const String& path) {
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

static bool validId(const String& id) {
    if (id.length() < 1 || id.length() > 48) return false;
    for (size_t i = 0; i < id.length(); ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

static uint32_t packageIdHash(const String& id) {
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < id.length(); ++i) {
        hash ^= (uint8_t)id[i];
        hash *= 16777619UL;
    }
    return hash;
}

static bool safeRelativePath(const String& path, bool allowDirectory = true) {
    if (path.length() < 1 || path.length() > 160 || path[0] == '/' ||
        path[0] == '\\' || path.indexOf('\\') >= 0 || path.indexOf(':') >= 0)
        return false;
    if (!allowDirectory && path.endsWith("/")) return false;

    int start = 0;
    while (start <= (int)path.length()) {
        int slash = path.indexOf('/', start);
        if (slash < 0) slash = path.length();
        String part = path.substring(start, slash);
        if (part == ".." || part == "." || (part.length() == 0 && slash < (int)path.length()))
            return false;
        start = slash + 1;
        if (slash == (int)path.length()) break;
    }
    return true;
}

static bool makeParents(const String& path, bool includeLast) {
    int end = includeLast ? path.length() : path.lastIndexOf('/');
    if (end < 1) return true;
    for (int i = 1; i <= end; ++i) {
        if (i != end && path[i] != '/') continue;
        String part = path.substring(0, i);
        if (part.length() > 0 && !SD.exists(part.c_str()) && !SD.mkdir(part.c_str()))
            return false;
    }
    if (includeLast && !SD.exists(path.c_str()) && !SD.mkdir(path.c_str()))
        return false;
    return true;
}

static void removeTree(const String& path) {
    File item = SD.open(path);
    if (!item) return;
    if (!item.isDirectory()) {
        item.close();
        SD.remove(path.c_str());
        return;
    }
    File child = item.openNextFile();
    while (child) {
        String childPath = path;
        if (!childPath.endsWith("/")) childPath += '/';
        childPath += baseName(child.name());
        bool directory = child.isDirectory();
        child.close();
        if (directory) removeTree(childPath);
        else SD.remove(childPath.c_str());
        child = item.openNextFile();
        yield();
    }
    item.close();
    SD.rmdir(path.c_str());
}

static bool parseManifest(const String& json, OPKManifest& manifest);

// Small, allocation-conscious JSON navigator used for the online catalog.
// The complete catalog stays in one bounded String; getters allocate only the
// final field requested by OpenStore instead of copying the whole document
// through the OSA operand stack on every json.get().
struct JsonRange {
    int start = -1;
    int end = -1;
    JsonRange() = default;
    JsonRange(int first, int last) : start(first), end(last) {}
    bool valid() const { return start >= 0 && end > start; }
};

static int catalogSkipWs(const String& json, int position) {
    while (position < (int)json.length() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) position++;
    return position;
}

static int catalogSkipString(const String& json, int position) {
    if (position < 0 || position >= (int)json.length() || json[position] != '"') return -1;
    for (position++; position < (int)json.length(); ++position) {
        uint8_t c = (uint8_t)json[position];
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (++position >= (int)json.length()) return -1;
            char escaped = json[position];
            if (escaped == 'u') {
                if (position + 4 >= (int)json.length()) return -1;
                for (int i = 1; i <= 4; ++i)
                    if (!isxdigit((unsigned char)json[position + i])) return -1;
                position += 4;
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                       escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') return -1;
        } else if (c == '"') return position + 1;
    }
    return -1;
}

static int catalogSkipValue(const String& json, int position) {
    position = catalogSkipWs(json, position);
    if (position >= (int)json.length()) return -1;
    char first = json[position];
    if (first == '"') return catalogSkipString(json, position);
    if (first == '{' || first == '[') {
        char expected[24];
        int depth = 0;
        expected[depth++] = first == '{' ? '}' : ']';
        for (position++; position < (int)json.length(); ++position) {
            char c = json[position];
            if (c == '"') {
                int end = catalogSkipString(json, position);
                if (end < 0) return -1;
                position = end - 1;
                continue;
            }
            if (c == '{' || c == '[') {
                if (depth >= (int)(sizeof(expected) / sizeof(expected[0]))) return -1;
                expected[depth++] = c == '{' ? '}' : ']';
            } else if (c == '}' || c == ']') {
                if (depth <= 0 || expected[depth - 1] != c) return -1;
                if (--depth == 0) return position + 1;
            }
        }
        return -1;
    }
    int start = position;
    while (position < (int)json.length()) {
        char c = json[position];
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
            c == '\r' || c == '\n') break;
        position++;
    }
    return position > start ? position : -1;
}

static String catalogDecodeString(const String& json, JsonRange range, size_t maximum) {
    if (!range.valid() || json[range.start] != '"' || json[range.end - 1] != '"') return "";
    String output;
    size_t hint = min(maximum, (size_t)(range.end - range.start - 2));
    if (hint > 0 && !output.reserve(hint)) return "";
    for (int i = range.start + 1; i < range.end - 1; ++i) {
        if (json[i] != '\\') {
            if (output.length() + 1 > maximum || !output.concat(json[i])) return "";
            continue;
        }

        uint32_t codepoint = 0;
        {
            if (++i >= range.end - 1) return "";
            char escaped = json[i];
            if (escaped == 'n') codepoint = '\n';
            else if (escaped == 'r') codepoint = '\r';
            else if (escaped == 't') codepoint = '\t';
            else if (escaped == 'b') codepoint = '\b';
            else if (escaped == 'f') codepoint = '\f';
            else if (escaped == '"' || escaped == '\\' || escaped == '/') codepoint = escaped;
            else if (escaped == 'u') {
                if (i + 4 >= range.end) return "";
                codepoint = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = json[++i];
                    codepoint = (codepoint << 4) |
                                (h >= '0' && h <= '9' ? h - '0' :
                                 h >= 'a' && h <= 'f' ? h - 'a' + 10 : h - 'A' + 10);
                }
                // Catalog text does not need UTF-16 surrogate composition;
                // reject lone surrogate halves instead of emitting invalid UTF-8.
                if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return "";
            } else return "";
        }
        char utf8[4]; int bytes = 0;
        if (codepoint <= 0x7F) utf8[bytes++] = (char)codepoint;
        else if (codepoint <= 0x7FF) {
            utf8[bytes++] = (char)(0xC0 | (codepoint >> 6));
            utf8[bytes++] = (char)(0x80 | (codepoint & 0x3F));
        } else {
            utf8[bytes++] = (char)(0xE0 | (codepoint >> 12));
            utf8[bytes++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            utf8[bytes++] = (char)(0x80 | (codepoint & 0x3F));
        }
        if (output.length() + bytes > maximum ||
            !output.concat(utf8, (unsigned int)bytes)) return "";
    }
    return output;
}

static JsonRange catalogObjectField(const String& json, JsonRange object,
                                    const String& wanted) {
    if (!object.valid()) return {};
    int position = catalogSkipWs(json, object.start);
    if (position >= object.end || json[position++] != '{') return {};
    while (position < object.end) {
        position = catalogSkipWs(json, position);
        if (position < object.end && json[position] == '}') return {};
        int keyStart = position;
        int keyEnd = catalogSkipString(json, position);
        if (keyEnd < 0 || keyEnd > object.end) return {};
        String key = catalogDecodeString(json, JsonRange(keyStart, keyEnd), 48);
        position = catalogSkipWs(json, keyEnd);
        if (position >= object.end || json[position++] != ':') return {};
        position = catalogSkipWs(json, position);
        int valueEnd = catalogSkipValue(json, position);
        if (valueEnd < 0 || valueEnd > object.end) return {};
        if (key == wanted) return JsonRange(position, valueEnd);
        position = catalogSkipWs(json, valueEnd);
        if (position < object.end && json[position] == ',') { position++; continue; }
        if (position < object.end && json[position] == '}') return {};
        return {};
    }
    return {};
}

static JsonRange catalogArrayElement(const String& json, JsonRange array, int wanted) {
    if (wanted < 0 || !array.valid()) return {};
    int position = catalogSkipWs(json, array.start);
    if (position >= array.end || json[position++] != '[') return {};
    int index = 0;
    while (position < array.end) {
        position = catalogSkipWs(json, position);
        if (position < array.end && json[position] == ']') return {};
        int end = catalogSkipValue(json, position);
        if (end < 0 || end > array.end) return {};
        if (index++ == wanted) return JsonRange(position, end);
        position = catalogSkipWs(json, end);
        if (position < array.end && json[position] == ',') { position++; continue; }
        return {};
    }
    return {};
}

static bool catalogRangeInt(const String& json, JsonRange range, int& output) {
    if (!range.valid()) return false;
    int position = range.start;
    bool negative = false;
    if (json[position] == '-') { negative = true; position++; }
    if (position >= range.end || !isdigit((unsigned char)json[position])) return false;
    int64_t result = 0;
    while (position < range.end && isdigit((unsigned char)json[position])) {
        result = result * 10 + json[position++] - '0';
        if (result > 2147483647LL) return false;
    }
    if (catalogSkipWs(json, position) != range.end) return false;
    output = negative ? -(int)result : (int)result;
    return true;
}

static JsonRange catalogRootRange(const String& json) {
    int start = catalogSkipWs(json, 0);
    int end = catalogSkipValue(json, start);
    if (end < 0 || catalogSkipWs(json, end) != (int)json.length()) return {};
    return JsonRange(start, end);
}

// Unlike catalogObjectField(), this scans the entire object and rejects a
// duplicate key. Manifests are security-sensitive metadata, so accepting the
// first of two conflicting `entry`/`scope` fields would be ambiguous.
static JsonRange uniqueObjectField(const String& json, JsonRange object,
                                   const char* wanted) {
    if (!object.valid()) return {};
    int position = catalogSkipWs(json, object.start);
    if (position >= object.end || json[position++] != '{') return {};
    JsonRange found;
    while (position < object.end) {
        position = catalogSkipWs(json, position);
        if (position < object.end && json[position] == '}')
            return found;
        int keyStart = position;
        int keyEnd = catalogSkipString(json, position);
        if (keyEnd < 0 || keyEnd > object.end) return {};
        String key = catalogDecodeString(json, JsonRange(keyStart, keyEnd), 48);
        if (key.length() == 0) return {};
        position = catalogSkipWs(json, keyEnd);
        if (position >= object.end || json[position++] != ':') return {};
        position = catalogSkipWs(json, position);
        int valueEnd = catalogSkipValue(json, position);
        if (valueEnd < 0 || valueEnd > object.end) return {};
        if (key == wanted) {
            if (found.valid()) return {};
            found = JsonRange(position, valueEnd);
        }
        position = catalogSkipWs(json, valueEnd);
        if (position < object.end && json[position] == ',') {
            position++;
            continue;
        }
        if (position < object.end && json[position] == '}') return found;
        return {};
    }
    return {};
}

static String manifestString(const String& json, JsonRange root,
                             const char* key, size_t maximum) {
    return catalogDecodeString(json, uniqueObjectField(json, root, key), maximum);
}

static bool parseManifest(const String& json, OPKManifest& manifest) {
    manifest = OPKManifest();
    JsonRange root = catalogRootRange(json);
    if (!root.valid() || json[root.start] != '{')
        return fail("Invalid OPK manifest JSON");

    if (!catalogRangeInt(json, uniqueObjectField(json, root, "schema"),
                         manifest.schema) || manifest.schema != 1)
        return fail("Unsupported OPK manifest schema");
    manifest.id = manifestString(json, root, "id", 48);
    if (!validId(manifest.id)) return fail("Invalid OPK package id");
    manifest.name = manifestString(json, root, "name", 48);
    if (manifest.name.length() < 1) return fail("Invalid OPK app name");
    manifest.version = manifestString(json, root, "version", 24);
    if (manifest.version.length() < 1) return fail("Invalid OPK version");
    if (!catalogRangeInt(json, uniqueObjectField(json, root, "versionCode"),
                         manifest.versionCode) || manifest.versionCode < 1)
        return fail("Invalid OPK versionCode");
    manifest.entry = manifestString(json, root, "entry", 160);
    if (!safeRelativePath(manifest.entry, false))
        return fail("Invalid OPK entry path");
    String entryLower = manifest.entry;
    entryLower.toLowerCase();
    if (!entryLower.endsWith(".osa") && !entryLower.endsWith(".osac"))
        return fail("OPK entry must be .osa or .osac");
    manifest.scope = manifestString(json, root, "scope", 8);
    if (manifest.scope != "user" && manifest.scope != "system")
        return fail("Invalid OPK package scope");

    JsonRange appFlag = uniqueObjectField(json, root, "isApp");
    manifest.isApp = true;
    if (appFlag.valid()) {
        int start = catalogSkipWs(json, appFlag.start);
        int end = appFlag.end;
        while (end > start && (json[end - 1] == ' ' || json[end - 1] == '\t' ||
               json[end - 1] == '\r' || json[end - 1] == '\n')) end--;
        if (end - start == 4 && json.startsWith("true", start)) manifest.isApp = true;
        else if (end - start == 5 && json.startsWith("false", start)) manifest.isApp = false;
        else return fail("Invalid OPK isApp flag");
    }
    return true;
}

static JsonRange catalogAppsRange() {
    JsonRange root = catalogRootRange(s_catalog);
    return catalogObjectField(s_catalog, root, "apps");
}

static JsonRange catalogItemRange(int index) {
    if (s_catalogIndexReady && index >= 0 && index < s_catalogCount)
        return JsonRange(s_catalogItemStart[index], s_catalogItemEnd[index]);
    return catalogArrayElement(s_catalog, catalogAppsRange(), index);
}

static String catalogStringField(int index, const char* field, size_t maximum) {
    JsonRange value = catalogObjectField(s_catalog, catalogItemRange(index), field);
    return catalogDecodeString(s_catalog, value, maximum);
}

static bool catalogStringFieldEquals(int index, const char* field,
                                     const char* expected) {
    JsonRange value = catalogObjectField(s_catalog, catalogItemRange(index), field);
    if (!value.valid() || s_catalog[value.start] != '"' ||
        s_catalog[value.end - 1] != '"') return false;
    size_t wanted = strlen(expected);
    if ((size_t)(value.end - value.start) == wanted + 2) {
        bool exact = true;
        for (size_t i = 0; i < wanted; ++i) {
            if (s_catalog[value.start + 1 + (int)i] != expected[i]) {
                exact = false;
                break;
            }
        }
        if (exact) return true;
    }
    // Escaped JSON spellings are legal; they are rare, so only that path pays
    // for a decoded temporary String.
    return catalogDecodeString(s_catalog, value, 16) == expected;
}

static bool validCatalogColor(const String& color) {
    if (color.length() != 7 || color[0] != '#') return false;
    for (int i = 1; i < 7; ++i)
        if (!isxdigit((unsigned char)color[i])) return false;
    return true;
}

static int countCatalogArray(const String& json, JsonRange array) {
    if (!array.valid()) return -1;
    int position = catalogSkipWs(json, array.start);
    if (position >= array.end || json[position++] != '[') return -1;
    int count = 0;
    while (position < array.end) {
        position = catalogSkipWs(json, position);
        if (position < array.end && json[position] == ']') return count;
        int end = catalogSkipValue(json, position);
        if (end < 0 || end > array.end || ++count > 64) return -1;
        position = catalogSkipWs(json, end);
        if (position < array.end && json[position] == ',') { position++; continue; }
        if (position < array.end && json[position] == ']') return count;
        return -1;
    }
    return -1;
}

static bool buildCatalogIndex() {
    s_catalogIndexReady = false;
    JsonRange apps = catalogAppsRange();
    if (!apps.valid() || s_catalogCount < 0 ||
        s_catalogCount > CATALOG_MAX_ENTRIES) return false;
    int position = catalogSkipWs(s_catalog, apps.start);
    if (position >= apps.end || s_catalog[position++] != '[') return false;
    for (int i = 0; i < s_catalogCount; ++i) {
        position = catalogSkipWs(s_catalog, position);
        int end = catalogSkipValue(s_catalog, position);
        if (end < 0 || end > apps.end) return false;
        s_catalogItemStart[i] = position;
        s_catalogItemEnd[i] = end;
        position = catalogSkipWs(s_catalog, end);
        if (i + 1 < s_catalogCount) {
            if (position >= apps.end || s_catalog[position] != ',') return false;
            position++;
        }
    }
    s_catalogIndexReady = true;
    return true;
}

static bool validateCatalogDocument(String& document, int& count) {
    String previous = static_cast<String&&>(s_catalog);
    int previousCount = s_catalogCount;
    s_catalog = static_cast<String&&>(document);
    s_catalogCount = 0;

    JsonRange root = catalogRootRange(s_catalog);
    int schema = 0;
    JsonRange apps = catalogObjectField(s_catalog, root, "apps");
    bool valid = root.valid() && s_catalog[root.start] == '{' &&
                 catalogRangeInt(s_catalog, catalogObjectField(s_catalog, root, "schema"), schema) &&
                 schema == 1 && apps.valid() && s_catalog[apps.start] == '[';
    count = valid ? countCatalogArray(s_catalog, apps) : -1;
    valid = valid && count >= 0;

    String ids[64];
    String systemPrefix = systemPackageSourcePrefix();
    bool systemPrefixValid = normalizeSystemPrefix(systemPrefix);
    for (int i = 0; valid && i < count; ++i) {
        JsonRange item = catalogItemRange(i);
        int versionCode = 0;
        String id = catalogStringField(i, "id", 48);
        String name = catalogStringField(i, "name", 48);
        String version = catalogStringField(i, "version", 24);
        String scope = catalogStringField(i, "scope", 8);
        String url = catalogStringField(i, "url", 2048);
        String hash = catalogStringField(i, "sha256", 64);
        JsonRange developerRange = catalogObjectField(s_catalog, item, "developer");
        JsonRange summaryRange = catalogObjectField(s_catalog, item, "summary");
        JsonRange descriptionRange = catalogObjectField(s_catalog, item, "description");
        JsonRange colorRange = catalogObjectField(s_catalog, item, "appColor");
        String developer = developerRange.valid()
                         ? catalogDecodeString(s_catalog, developerRange, 64) : "";
        String summary = summaryRange.valid()
                       ? catalogDecodeString(s_catalog, summaryRange, 240) : "";
        String description = descriptionRange.valid()
                           ? catalogDecodeString(s_catalog, descriptionRange, 10000) : "";
        String color = colorRange.valid()
                     ? catalogDecodeString(s_catalog, colorRange, 7) : "";
        JsonRange versionRange = catalogObjectField(s_catalog, item, "versionCode");
        valid = item.valid() && s_catalog[item.start] == '{' && validId(id) &&
                name.length() > 0 && version.length() > 0 &&
                (scope == "user" || scope == "system") && url.startsWith("https://") &&
                url.indexOf('\r') < 0 && url.indexOf('\n') < 0 &&
                hash.length() == 64 && catalogRangeInt(s_catalog, versionRange, versionCode) &&
                versionCode > 0;
        if (valid && developerRange.valid()) valid = developer.length() > 0;
        if (valid && summaryRange.valid() && summary.length() == 0 &&
            summaryRange.end - summaryRange.start > 2) valid = false;
        if (valid && descriptionRange.valid() && description.length() == 0 &&
            descriptionRange.end - descriptionRange.start > 2) valid = false;
        if (valid && colorRange.valid()) valid = validCatalogColor(color);
        for (size_t k = 0; valid && k < hash.length(); ++k)
            if (!isxdigit((unsigned char)hash[k])) valid = false;
        if (valid && scope == "system")
            valid = systemPrefixValid && isOfficialSystemId(id) &&
                    url.startsWith(systemPrefix);
        if (valid && scope == "user") valid = !id.startsWith("openos.");
        for (int k = 0; valid && k < i; ++k) if (ids[k] == id) valid = false;
        ids[i] = static_cast<String&&>(id);
    }

    document = static_cast<String&&>(s_catalog);
    s_catalog = static_cast<String&&>(previous);
    s_catalogCount = previousCount;
    return valid;
}

static bool readSmallFile(const String& path, size_t maximum, String& output) {
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return fail("Manifest not found");
    }
    size_t length = (size_t)file.size();
    if (length == 0 || length > maximum || !output.reserve(length)) {
        file.close();
        return fail("Manifest is empty or too large");
    }
    output = "";
    char buffer[256];
    while (file.available()) {
        int count = file.read((uint8_t*)buffer, sizeof(buffer));
        if (count <= 0 || !output.concat(buffer, (unsigned int)count)) {
            file.close();
            return fail("Could not read manifest");
        }
    }
    file.close();
    return true;
}

class LimitedStringStream : public Stream {
public:
    explicit LimitedStringStream(size_t limit) : maximum(limit) {}
    bool begin(size_t hint) { return hint == 0 || data.reserve(min(hint, maximum)); }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* src, size_t count) override {
        if (overflow || count > maximum - data.length()) {
            overflow = true; setWriteError(); return 0;
        }
        if (count && !data.concat((const char*)src, (unsigned int)count)) {
            oom = true; setWriteError(); return 0;
        }
        return count;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    String take() { return static_cast<String&&>(data); }
    bool tooLarge() const { return overflow; }
    bool outOfMemory() const { return oom; }
private:
    String data;
    size_t maximum;
    bool overflow = false;
    bool oom = false;
};

class LimitedFileStream : public Stream {
public:
    LimitedFileStream(File& destination, size_t limit) : file(destination), maximum(limit) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* src, size_t count) override {
        if (overflow || count > maximum - written) {
            overflow = true; setWriteError(); return 0;
        }
        size_t n = file.write(src, count);
        written += n;
        if (n != count) setWriteError();
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override { file.flush(); }
    size_t size() const { return written; }
    bool tooLarge() const { return overflow; }
private:
    File& file;
    size_t maximum;
    size_t written = 0;
    bool overflow = false;
};

static bool beginHttp(HTTPClient& http, WiFiClientSecure& client, const String& url) {
    if (!url.startsWith("https://") || url.length() > 2048 ||
        url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0)
        return fail("OpenStore requires a valid HTTPS URL");
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    client.setInsecure();
    return http.begin(client, url) || fail("Could not open HTTPS connection");
}

static void prepareHttpsMemory(const char* operation) {
    // Classic Bluetooth and TLS compete for the same internal RAM on the
    // no-PSRAM ESP32. Keep the user's setting enabled, but suspend the radio
    // until OpenStore is closed; main.cpp restores it after returning Home.
    if (sysBTEnabled) osaSuspendBluetoothForMemory(operation);
    Serial.printf("[STORE] %s free=%u maxBlock=%u\n", operation,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static String httpErrorMessage(const char* resource, int status) {
    String detail = HTTPClient::errorToString(status);
    String message(resource);
    message += " HTTPS failed (";
    message += status;
    message += "): ";
    message += detail.length() > 0 ? detail : String("connection error");
    return message;
}

static bool downloadPackageFile(const String& url) {
    prepareHttpsMemory("package HTTPS");
    removeFileIfPresent(DOWNLOAD_PATH);
    File destination = SD.open(DOWNLOAD_PATH, FILE_WRITE);
    if (!destination) return fail("Could not create package download file");

    // Keep all TLS objects in this helper. Their destructors run before ZIP
    // extraction allocates its 32 KB deflate dictionary.
    HTTPClient http;
    WiFiClientSecure client;
    if (!beginHttp(http, client, url)) {
        destination.close(); SD.remove(DOWNLOAD_PATH); return false;
    }
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        String message = status < 0
            ? httpErrorMessage("Package", status)
            : String("Package HTTP error ") + status;
        Serial.printf("[STORE] %s\n", message.c_str());
        http.end(); destination.close(); SD.remove(DOWNLOAD_PATH);
        return fail(message);
    }
    int declared = http.getSize();
    if (declared > (int)OPK_MAX_PACKAGE_BYTES) {
        http.end(); destination.close(); SD.remove(DOWNLOAD_PATH);
        return fail("OPK download exceeds 8 MB");
    }
    LimitedFileStream sink(destination, OPK_MAX_PACKAGE_BYTES);
    int received = http.writeToStream(&sink);
    sink.flush();
    http.end();
    destination.close();
    if (sink.tooLarge()) { SD.remove(DOWNLOAD_PATH); return fail("OPK download exceeds 8 MB"); }
    if (received < 0 || sink.size() == 0) {
        SD.remove(DOWNLOAD_PATH); return fail("Package download failed");
    }
    return true;
}

static bool sha256File(const String& path, String& hex) {
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return fail("Downloaded package is missing");
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0) != 0) {
        file.close(); mbedtls_sha256_free(&context);
        return fail("SHA-256 initialization failed");
    }
    uint8_t buffer[1024];
    while (file.available()) {
        int n = file.read(buffer, sizeof(buffer));
        if (n <= 0 || mbedtls_sha256_update_ret(&context, buffer, n) != 0) {
            file.close(); mbedtls_sha256_free(&context);
            return fail("SHA-256 read failed");
        }
        yield();
    }
    file.close();
    uint8_t digest[32];
    if (mbedtls_sha256_finish_ret(&context, digest) != 0) {
        mbedtls_sha256_free(&context);
        return fail("SHA-256 finalization failed");
    }
    mbedtls_sha256_free(&context);
    static const char digits[] = "0123456789abcdef";
    hex = "";
    if (!hex.reserve(64)) return fail("Not enough memory for SHA-256");
    for (uint8_t b : digest) {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return true;
}

struct ZipEntry {
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed = 0;
    uint32_t uncompressed = 0;
    String name;
    uint32_t dataOffset = 0;
};

static bool readZipEntry(File& archive, ZipEntry& entry) {
    uint16_t version, modTime, modDate, nameLength, extraLength;
    if (!readU16(archive, version) || !readU16(archive, entry.flags) ||
        !readU16(archive, entry.method) || !readU16(archive, modTime) ||
        !readU16(archive, modDate) || !readU32(archive, entry.crc) ||
        !readU32(archive, entry.compressed) || !readU32(archive, entry.uncompressed) ||
        !readU16(archive, nameLength) || !readU16(archive, extraLength))
        return fail("Truncated OPK ZIP header");
    (void)version; (void)modTime; (void)modDate;
    if (nameLength < 1 || nameLength > 160 || extraLength > 4096)
        return fail("Invalid OPK ZIP header lengths");
    if ((entry.flags & 0x0001) || (entry.flags & 0x0008))
        return fail("Encrypted ZIPs and data descriptors are not supported");
    if (entry.method != 0 && entry.method != 8)
        return fail("OPK uses an unsupported ZIP compression method");
    if (entry.uncompressed > OPK_MAX_FILE_BYTES)
        return fail("OPK file exceeds the 2 MB limit");

    entry.name = "";
    if (!entry.name.reserve(nameLength)) return fail("Not enough memory for ZIP path");
    char pathBuffer[64];
    uint16_t left = nameLength;
    while (left) {
        size_t take = min((size_t)left, sizeof(pathBuffer));
        if (!readExact(archive, pathBuffer, take))
            return fail("Truncated OPK ZIP path");
        // Check raw bytes before converting them into Arduino String. Calling
        // String::indexOf('\0') is incorrect here because the normal trailing
        // terminator can be reported as a match, rejecting every valid path.
        for (size_t i = 0; i < take; ++i) {
            uint8_t c = (uint8_t)pathBuffer[i];
            if (c < 0x20 || c == 0x7F)
                return fail("Unsafe path in OPK archive");
        }
        if (!entry.name.concat(pathBuffer, (unsigned int)take))
            return fail("Not enough memory for ZIP path");
        left -= (uint16_t)take;
    }
    if (!safeRelativePath(entry.name)) return fail("Unsafe path in OPK archive");

    uint32_t afterExtra = (uint32_t)archive.position() + extraLength;
    if (afterExtra < archive.position() || afterExtra > archive.size() ||
        !archive.seek(afterExtra)) return fail("Invalid OPK ZIP extra field");
    entry.dataOffset = archive.position();
    uint32_t dataEnd = entry.dataOffset + entry.compressed;
    if (dataEnd < entry.dataOffset || dataEnd > archive.size())
        return fail("OPK ZIP payload lies outside archive");
    return true;
}

static bool copyStored(File& archive, File& output, const ZipEntry& entry) {
    if (entry.compressed != entry.uncompressed)
        return fail("Invalid stored ZIP entry sizes");
    uint8_t buffer[1024];
    uint32_t left = entry.compressed;
    mz_ulong crc = MZ_CRC32_INIT;
    while (left) {
        size_t take = min((uint32_t)sizeof(buffer), left);
        if (archive.read(buffer, take) != (int)take || output.write(buffer, take) != take)
            return fail("Could not extract OPK file");
        crc = mz_crc32(crc, buffer, take);
        left -= take;
        yield();
    }
    if ((uint32_t)crc != entry.crc) return fail("OPK CRC-32 check failed");
    return true;
}

static bool inflateDeflated(File& archive, File& output, const ZipEntry& entry) {
    uint8_t* dictionary = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
    tinfl_decompressor* inflater = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    if (!dictionary || !inflater) {
        free(dictionary); free(inflater);
        return fail("Not enough RAM to extract compressed OPK");
    }
    tinfl_init(inflater);

    uint8_t input[1024];
    size_t inputPos = 0, inputLength = 0, outputOffset = 0;
    uint32_t compressedLeft = entry.compressed;
    uint32_t totalOutput = 0;
    mz_ulong crc = MZ_CRC32_INIT;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;

    while (true) {
        if (inputPos == inputLength && compressedLeft > 0) {
            size_t want = min((uint32_t)sizeof(input), compressedLeft);
            int got = archive.read(input, want);
            if (got <= 0) { status = TINFL_STATUS_FAILED; break; }
            inputPos = 0;
            inputLength = (size_t)got;
            compressedLeft -= (uint32_t)got;
        }

        size_t availableInput = inputLength - inputPos;
        size_t availableOutput = TINFL_LZ_DICT_SIZE - outputOffset;
        size_t consumed = availableInput;
        size_t produced = availableOutput;
        mz_uint32 flags = compressedLeft > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        status = tinfl_decompress(inflater, input + inputPos, &consumed,
                                  dictionary, dictionary + outputOffset,
                                  &produced, flags);
        inputPos += consumed;

        if (produced > 0) {
            if (produced > entry.uncompressed ||
                totalOutput > entry.uncompressed - produced ||
                output.write(dictionary + outputOffset, produced) != produced) {
                status = TINFL_STATUS_FAILED;
                break;
            }
            crc = mz_crc32(crc, dictionary + outputOffset, produced);
            totalOutput += produced;
            outputOffset = (outputOffset + produced) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (status == TINFL_STATUS_DONE) break;
        if (status < 0 || (consumed == 0 && produced == 0 &&
            compressedLeft == 0 && inputPos == inputLength)) break;
        yield();
    }

    bool valid = status == TINFL_STATUS_DONE && totalOutput == entry.uncompressed &&
                 compressedLeft == 0 && inputPos == inputLength &&
                 (uint32_t)crc == entry.crc;
    free(dictionary);
    free(inflater);
    return valid || fail("Deflated OPK file is corrupt");
}

static bool extractPayload(File& archive, File& output, const ZipEntry& entry) {
    if (!archive.seek(entry.dataOffset)) return fail("Could not seek OPK payload");
    bool ok = entry.method == 0 ? copyStored(archive, output, entry)
                                : inflateDeflated(archive, output, entry);
    output.flush();
    return ok;
}

static bool readArchiveManifest(const String& archivePath, OPKManifest& manifest) {
    File archive = SD.open(archivePath);
    if (!archive) return fail("Could not open downloaded OPK");
    SD.mkdir("/system");
    removeFileIfPresent(MANIFEST_TMP);
    bool found = false;
    int entries = 0;

    while (archive.position() + 4 <= archive.size()) {
        uint32_t signature;
        if (!readU32(archive, signature)) break;
        if (signature == 0x02014b50 || signature == 0x06054b50) break;
        if (signature != 0x04034b50) {
            archive.close(); SD.remove(MANIFEST_TMP);
            return fail("Invalid OPK ZIP structure");
        }
        if (++entries > OPK_MAX_ENTRIES) {
            archive.close(); SD.remove(MANIFEST_TMP);
            return fail("OPK contains too many files");
        }
        ZipEntry entry;
        if (!readZipEntry(archive, entry)) {
            archive.close(); SD.remove(MANIFEST_TMP); return false;
        }
        if (entry.name == "manifest.json") {
            if (found || entry.uncompressed == 0 || entry.uncompressed > OPK_MAX_MANIFEST) {
                archive.close(); SD.remove(MANIFEST_TMP);
                return fail("OPK must contain one small root manifest.json");
            }
            File output = SD.open(MANIFEST_TMP, FILE_WRITE);
            if (!output) { archive.close(); return fail("Could not create manifest temp file"); }
            bool ok = extractPayload(archive, output, entry);
            output.close();
            if (!ok) { archive.close(); SD.remove(MANIFEST_TMP); return false; }
            found = true;
        } else if (!archive.seek(entry.dataOffset + entry.compressed)) {
            archive.close(); SD.remove(MANIFEST_TMP);
            return fail("Could not skip OPK payload");
        }
    }
    archive.close();
    if (!found) { SD.remove(MANIFEST_TMP); return fail("OPK manifest.json is missing"); }

    String json;
    bool ok = readSmallFile(MANIFEST_TMP, OPK_MAX_MANIFEST, json) &&
              parseManifest(json, manifest);
    SD.remove(MANIFEST_TMP);
    return ok;
}

static bool extractArchive(const String& archivePath, const String& stage,
                           const OPKManifest& expected) {
    File archive = SD.open(archivePath);
    if (!archive) return fail("Could not reopen OPK");
    uint32_t totalUncompressed = 0;
    int entries = 0;

    while (archive.position() + 4 <= archive.size()) {
        uint32_t signature;
        if (!readU32(archive, signature)) break;
        if (signature == 0x02014b50 || signature == 0x06054b50) break;
        if (signature != 0x04034b50) {
            archive.close(); return fail("Invalid OPK ZIP structure");
        }
        if (++entries > OPK_MAX_ENTRIES) {
            archive.close(); return fail("OPK contains too many files");
        }
        ZipEntry entry;
        if (!readZipEntry(archive, entry)) { archive.close(); return false; }
        if (totalUncompressed > OPK_MAX_TOTAL_BYTES - entry.uncompressed) {
            archive.close(); return fail("OPK uncompressed size exceeds 8 MB");
        }
        totalUncompressed += entry.uncompressed;

        String outputPath = stage + "/" + entry.name;
        if (entry.name.endsWith("/")) {
            if (entry.compressed != 0 || entry.uncompressed != 0 ||
                !makeParents(outputPath.substring(0, outputPath.length() - 1), true)) {
                archive.close(); return fail("Could not create OPK directory");
            }
            continue;
        }
        if (SD.exists(outputPath.c_str())) {
            archive.close(); return fail("OPK contains a duplicate path");
        }
        if (!makeParents(outputPath, false)) {
            archive.close(); return fail("Could not create OPK parent directory");
        }
        File output = SD.open(outputPath, FILE_WRITE);
        if (!output) { archive.close(); return fail("Could not create OPK output file"); }
        bool ok = extractPayload(archive, output, entry);
        output.close();
        if (!ok) { archive.close(); return false; }
    }
    archive.close();
    if (entries == 0) return fail("OPK archive is empty");

    OPKManifest verified;
    if (!readManifest(stage, verified)) return false;
    if (verified.id != expected.id || verified.entry != expected.entry ||
        verified.scope != expected.scope || verified.versionCode != expected.versionCode)
        return fail("OPK manifest changed during extraction");
    String entryPath = stage + "/" + verified.entry;
    File entry = SD.open(entryPath);
    bool entryOk = entry && !entry.isDirectory();
    if (entry) entry.close();
    return entryOk || fail("OPK entry script is missing");
}

static bool commitArchive(const String& archivePath, const OPKManifest& manifest) {
    bool systemPackage = manifest.scope == "system";
    String root = systemPackage ? "/system/packages" : "/packages";
    if (!makeParents(root, true)) return fail("Could not create package directory");
    String target = root + "/" + manifest.id;
    String stage = root + "/.stage-" + manifest.id;
    String backup = root + "/.backup-" + manifest.id;

    removeTree(stage);
    removeTree(backup);
    if (!SD.mkdir(stage.c_str())) return fail("Could not create OPK staging directory");
    if (!extractArchive(archivePath, stage, manifest)) {
        removeTree(stage);
        return false;
    }

    bool hadPrevious = SD.exists(target.c_str());
    if (hadPrevious && !SD.rename(target.c_str(), backup.c_str())) {
        removeTree(stage);
        return fail("Could not prepare previous package version");
    }
    if (!SD.rename(stage.c_str(), target.c_str())) {
        if (hadPrevious) SD.rename(backup.c_str(), target.c_str());
        removeTree(stage);
        return fail("Could not activate installed package");
    }
    if (hadPrevious) removeTree(backup);
    s_restartRequired = true;
    return true;
}

static void recoverRoot(const String& root) {
    File directory = SD.open(root);
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        return;
    }
    File child = directory.openNextFile();
    while (child) {
        String name = baseName(child.name());
        bool isDirectory = child.isDirectory();
        child.close();
        String full = root + "/" + name;
        if (isDirectory && name.startsWith(".stage-")) {
            removeTree(full);
        } else if (isDirectory && name.startsWith(".backup-")) {
            String id = name.substring(8);
            String target = root + "/" + id;
            if (!SD.exists(target.c_str())) SD.rename(full.c_str(), target.c_str());
            else removeTree(full);
        }
        child = directory.openNextFile();
    }
    directory.close();
}

static bool downloadCatalogDocument(String& document) {
    // Keep TLS/HTTP objects inside this helper so their buffers are gone before
    // JSON validation allocates temporary IDs and before an OPK download starts.
    prepareHttpsMemory("catalog HTTPS");
    String source = catalogSourceUrl();
    // GitHub's raw CDN can briefly serve an older branch snapshot directly
    // after publishing. A harmless query key also prevents that stale cache.
    if (source.startsWith("https://raw.githubusercontent.com/")) {
        source += source.indexOf('?') >= 0 ? '&' : '?';
        source += "openos=";
        source += millis();
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        HTTPClient http;
        WiFiClientSecure client;
        if (!beginHttp(http, client, source)) return false;
        int status = http.GET();
        if (status != HTTP_CODE_OK) {
            String message = status < 0
                ? httpErrorMessage("Catalog", status)
                : String("Catalog HTTP error ") + status;
            Serial.printf("[STORE] catalog attempt %d: %s free=%u maxBlock=%u\n",
                          attempt + 1, message.c_str(),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            http.end();
            if (status < 0 && attempt == 0) {
                delay(350);
                yield();
                continue;
            }
            return fail(message);
        }
        int declared = http.getSize();
        if (declared > (int)CATALOG_MAX_BYTES) {
            http.end();
            return fail("Store catalog exceeds 24 KB");
        }
        LimitedStringStream output(CATALOG_MAX_BYTES);
        if (!output.begin(declared > 0 ? (size_t)declared : 1024)) {
            http.end();
            return fail("Not enough RAM for store catalog");
        }
        int received = http.writeToStream(&output);
        http.end();
        if (output.tooLarge()) return fail("Store catalog exceeds 24 KB");
        if (output.outOfMemory()) return fail("Not enough RAM for store catalog");
        if (received < 0) return fail("Could not read store catalog");
        document = output.take();
        return document.length() > 0 || fail("Store catalog is empty");
    }
    return fail("Catalog HTTPS failed");
}

} // namespace

static void rebuildInstalledVersionCache();

void begin() {
    s_error = "";
    s_restartRequired = false;
    clearCatalog();
    if (!isSdReady) return;
    SD.mkdir("/system");
    SD.mkdir("/packages");
    SD.mkdir("/system/packages");
    removeFileIfPresent(DOWNLOAD_PATH);
    removeFileIfPresent(MANIFEST_TMP);
    recoverRoot("/packages");
    recoverRoot("/system/packages");
}

String catalogSourceUrl() {
    String source = Config::get(CATALOG_CONFIG_KEY, "");
    source.trim();
    return source.length() > 0 ? source : String(DEFAULT_CATALOG_URL);
}

bool setCatalogSourceUrl(const String& requested) {
    s_error = "";
    String source = requested;
    source.trim();
    // Empty restores the firmware default.
    if (source.length() > 0 && !validHttpsSource(source, false))
        return fail("Catalog source must be a valid HTTPS URL");
    Config::set(CATALOG_CONFIG_KEY, source);
    Config::save();
    clearCatalog();
    return true;
}

String systemPackageSourcePrefix() {
    String prefix = Config::get(SYSTEM_PREFIX_CONFIG_KEY, "");
    prefix.trim();
    if (prefix.length() == 0) prefix = DEFAULT_SYSTEM_PACKAGE_PREFIX;
    // Return a normalized valid prefix. Invalid values remain unchanged so
    // catalog validation fails closed instead of silently trusting a fallback.
    String normalized = prefix;
    return normalizeSystemPrefix(normalized) ? normalized : prefix;
}

bool setSystemPackageSourcePrefix(const String& requested) {
    s_error = "";
    String prefix = requested;
    prefix.trim();
    // Empty restores the compiled default source.
    if (prefix.length() > 0 && !normalizeSystemPrefix(prefix))
        return fail("System package source must be an HTTPS directory URL");
    Config::set(SYSTEM_PREFIX_CONFIG_KEY, prefix);
    Config::save();
    clearCatalog();
    return true;
}

String fetchCatalog() {
    if (!refreshCatalog()) return "";
    return s_catalog;
}

bool refreshCatalog() {
    s_error = "";
    if (!isSdReady) return fail("No SD card");
    if (WiFi.status() != WL_CONNECTED) return fail("Wi-Fi is not connected");
    clearCatalog();
    if (sysBTEnabled) osaSuspendBluetoothForMemory("OpenStore catalog");
    if (ESP.getFreeHeap() < CATALOG_MAX_BYTES + 12 * 1024) {
        return fail("Not enough RAM for store catalog");
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16U * 1024U)
        return fail("Not enough contiguous RAM for store HTTPS");
    String document;
    if (!downloadCatalogDocument(document)) return false;
    int count = 0;
    if (!validateCatalogDocument(document, count))
        return fail("Store catalog has an invalid schema or entry");
    s_catalog = static_cast<String&&>(document);
    s_catalogCount = count;
    if (!buildCatalogIndex()) {
        clearCatalog();
        return fail("Could not index store catalog");
    }
    rebuildInstalledVersionCache();
    return true;
}

void clearCatalog() {
    releaseCatalogStorage();
}

int catalogCount() { return s_catalogCount; }
String catalogId(int index)      { return catalogStringField(index, "id", 48); }
String catalogName(int index)    { return catalogStringField(index, "name", 48); }
String catalogVersion(int index) { return catalogStringField(index, "version", 24); }
String catalogScope(int index)   { return catalogStringField(index, "scope", 8); }
String catalogSummary(int index) { return catalogStringField(index, "summary", 240); }
String catalogDeveloper(int index) {
    String value = catalogStringField(index, "developer", 64);
    return value.length() > 0 ? value : String("Unknown developer");
}
String catalogDescription(int index) {
    String value = catalogStringField(index, "description", 10000);
    return value.length() > 0 ? value : catalogSummary(index);
}
uint16_t catalogColor(int index) {
    String value = catalogStringField(index, "appColor", 7);
    if (!validCatalogColor(value)) return 0x0C3F; // #0A84FF in RGB565
    uint32_t rgb = strtoul(value.c_str() + 1, nullptr, 16);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
String catalogUrl(int index)     { return catalogStringField(index, "url", 2048); }
String catalogSha256(int index)  { return catalogStringField(index, "sha256", 64); }

int catalogVisibleCount(bool systemTab) {
    int visible = 0;
    const char* scope = systemTab ? "system" : "user";
    for (int i = 0; i < s_catalogCount; ++i)
        if (catalogStringFieldEquals(i, "scope", scope)) ++visible;
    return visible;
}

int catalogVisibleIndex(bool systemTab, int slot) {
    if (slot < 0) return -1;
    const char* scope = systemTab ? "system" : "user";
    int seen = 0;
    for (int i = 0; i < s_catalogCount; ++i) {
        if (!catalogStringFieldEquals(i, "scope", scope)) continue;
        if (seen++ == slot) return i;
    }
    return -1;
}

int catalogVersionCode(int index) {
    int value = 0;
    JsonRange range = catalogObjectField(s_catalog, catalogItemRange(index), "versionCode");
    return catalogRangeInt(s_catalog, range, value) ? value : 0;
}

bool installFromUrl(const String& url, const String& expectedSha256,
                    const String& expectedId, const String& expectedScope) {
    s_error = "";
    if (!isSdReady) return fail("No SD card");
    if (WiFi.status() != WL_CONNECTED) return fail("Wi-Fi is not connected");
    if (!validId(expectedId)) return fail("Invalid expected package id");
    if (expectedScope != "user" && expectedScope != "system")
        return fail("Catalog package scope is invalid");
    String expectedHash = expectedSha256;
    expectedHash.toLowerCase();
    if (expectedHash.length() != 64) return fail("Catalog SHA-256 is invalid");
    for (char c : expectedHash)
        if (!isxdigit((unsigned char)c)) return fail("Catalog SHA-256 is invalid");

    // Bind installation to the catalog document that OpenStore just fetched;
    // callers cannot mix an ID from one row with a URL/hash from elsewhere.
    bool catalogMatch = false;
    int catalogVersionCodeExpected = 0;
    String catalogDisplayName;
    String catalogDisplayVersion;
    for (int i = 0; i < s_catalogCount && !catalogMatch; ++i) {
        String rowHash = catalogSha256(i); rowHash.toLowerCase();
        catalogMatch = catalogId(i) == expectedId && catalogScope(i) == expectedScope &&
                       catalogUrl(i) == url && rowHash == expectedHash;
        if (catalogMatch) {
            catalogVersionCodeExpected = catalogVersionCode(i);
            catalogDisplayName = catalogName(i);
            catalogDisplayVersion = catalogVersion(i);
        }
    }
    if (!catalogMatch) return fail("Package is not present in the active catalog");

    // The caller has copied the selected item's small fields. Release the
    // potentially 24 KB catalog before TLS and ZIP extraction need RAM.
    clearCatalog();

    SD.mkdir("/system");
    if (!downloadPackageFile(url)) return false;

    String actualHash;
    if (!sha256File(DOWNLOAD_PATH, actualHash)) { SD.remove(DOWNLOAD_PATH); return false; }
    if (actualHash != expectedHash) {
        SD.remove(DOWNLOAD_PATH); return fail("OPK SHA-256 mismatch");
    }

    OPKManifest manifest;
    if (!readArchiveManifest(DOWNLOAD_PATH, manifest)) {
        SD.remove(DOWNLOAD_PATH); return false;
    }
    if (manifest.id != expectedId) {
        SD.remove(DOWNLOAD_PATH); return fail("OPK id does not match catalog");
    }
    if (manifest.scope != expectedScope) {
        SD.remove(DOWNLOAD_PATH); return fail("OPK scope does not match catalog");
    }
    if (manifest.versionCode != catalogVersionCodeExpected) {
        SD.remove(DOWNLOAD_PATH); return fail("OPK versionCode does not match catalog");
    }
    if (manifest.name != catalogDisplayName || manifest.version != catalogDisplayVersion) {
        SD.remove(DOWNLOAD_PATH); return fail("OPK name/version does not match catalog");
    }
    if (manifest.scope == "system") {
        String systemPrefix = systemPackageSourcePrefix();
        if (!isOfficialSystemId(manifest.id) ||
            !normalizeSystemPrefix(systemPrefix) ||
            !url.startsWith(systemPrefix)) {
            SD.remove(DOWNLOAD_PATH);
            return fail("System package is not from an approved OpenOS source");
        }
    } else if (manifest.id.startsWith("openos.")) {
        SD.remove(DOWNLOAD_PATH);
        return fail("The openos.* package namespace is reserved for system apps");
    }

    // A package ID has exactly one scope. This prevents a user package from
    // shadowing version/status queries for a system package (and vice versa).
    String targetDir = manifest.scope == "system"
                     ? "/system/packages/" + manifest.id
                     : "/packages/" + manifest.id;
    String otherDir = manifest.scope == "system"
                    ? "/packages/" + manifest.id
                    : "/system/packages/" + manifest.id;
    if (SD.exists(otherDir.c_str())) {
        SD.remove(DOWNLOAD_PATH);
        return fail("Package id is already installed in a different scope");
    }

    // Never accept a catalog rollback over a newer installed package. Equal
    // versions remain reinstallable so a damaged SD copy can be repaired.
    String previousError = s_error;
    OPKManifest installedManifest;
    bool hasInstalled = readManifest(targetDir, installedManifest) &&
                        installedManifest.id == manifest.id &&
                        installedManifest.scope == manifest.scope;
    s_error = previousError;
    if (hasInstalled && manifest.versionCode < installedManifest.versionCode) {
        SD.remove(DOWNLOAD_PATH);
        return fail("Package downgrade is not allowed");
    }

    bool installed = commitArchive(DOWNLOAD_PATH, manifest);
    SD.remove(DOWNLOAD_PATH);
    return installed;
}

bool readManifest(const String& packageDir, OPKManifest& out) {
    String json;
    if (!readSmallFile(packageDir + "/manifest.json", OPK_MAX_MANIFEST, json)) return false;
    return parseManifest(json, out);
}

static bool readInstalledManifest(const String& directory, OPKManifest& out) {
    String manifestPath = directory + "/manifest.json";
    if (!SD.exists(manifestPath.c_str())) return false;
    return readManifest(directory, out);
}

String installedEntry(const String& id, bool systemPackage) {
    if (!validId(id)) return "";
    String previousError = s_error;
    String root = systemPackage ? "/system/packages/" : "/packages/";
    String directory = root + id;
    String manifestPath = directory + "/manifest.json";
    // Avoid asking SD.open() for paths that do not exist. The ESP32 VFS logs
    // every such probe as an error even though "package not installed" is a
    // normal condition during Home discovery.
    if (!SD.exists(manifestPath.c_str())) {
        s_error = previousError;
        return "";
    }
    OPKManifest manifest;
    if (!readManifest(directory, manifest) || manifest.id != id ||
        (systemPackage ? manifest.scope != "system" : manifest.scope != "user")) {
        s_error = previousError;
        return "";
    }
    String path = directory + "/" + manifest.entry;
    File entry = SD.open(path);
    bool ok = entry && !entry.isDirectory();
    if (entry) entry.close();
    s_error = previousError;
    return ok ? path : String("");
}

static int readInstalledVersionCodeUncached(const String& id) {
    if (!validId(id)) return 0;
    String previousError = s_error;
    OPKManifest manifest;
    String primary = isOfficialSystemId(id) ? "/system/packages/" + id
                                            : "/packages/" + id;
    String secondary = isOfficialSystemId(id) ? "/packages/" + id
                                              : "/system/packages/" + id;
    if (readInstalledManifest(primary, manifest) && manifest.id == id) {
        s_error = previousError;
        return manifest.versionCode;
    }
    if (readInstalledManifest(secondary, manifest) && manifest.id == id) {
        s_error = previousError;
        return manifest.versionCode;
    }
    s_error = previousError;
    return 0;
}

static void rebuildInstalledVersionCache() {
    s_installedCacheReady = false;
    int count = min(s_catalogCount, CATALOG_MAX_ENTRIES);
    for (int i = 0; i < count; ++i) {
        String id = catalogId(i);
        s_installedIdHash[i] = packageIdHash(id);
        s_installedCode[i] = readInstalledVersionCodeUncached(id);
    }
    s_installedCacheReady = true;
}

int installedVersionCode(const String& id) {
    if (!validId(id)) return 0;
    if (s_catalogCount > 0 && !s_installedCacheReady)
        rebuildInstalledVersionCache();
    if (s_installedCacheReady) {
        uint32_t hash = packageIdHash(id);
        int count = min(s_catalogCount, CATALOG_MAX_ENTRIES);
        for (int i = 0; i < count; ++i) {
            if (s_installedIdHash[i] == hash && catalogId(i) == id)
                return s_installedCode[i];
        }
    }
    return readInstalledVersionCodeUncached(id);
}

int catalogItemState(int index) {
    if (index < 0 || index >= s_catalogCount) return 0;
    if (!s_installedCacheReady) rebuildInstalledVersionCache();
    int local = s_installedCode[index];
    int remote = catalogVersionCode(index);
    if (local == 0)
        return catalogStringFieldEquals(index, "scope", "system") ? 1 : 0;
    if (local < remote) return 1;
    if (local == remote) return 2;
    return 3;
}

bool catalogCanUninstall(int index) {
    if (index < 0 || index >= s_catalogCount ||
        !catalogStringFieldEquals(index, "scope", "user")) return false;
    if (!s_installedCacheReady) rebuildInstalledVersionCache();
    return s_installedCode[index] > 0;
}

String installedVersion(const String& id) {
    if (!validId(id)) return "";
    String previousError = s_error;
    OPKManifest manifest;
    String primary = isOfficialSystemId(id) ? "/system/packages/" + id
                                            : "/packages/" + id;
    String secondary = isOfficialSystemId(id) ? "/packages/" + id
                                              : "/system/packages/" + id;
    if (readInstalledManifest(primary, manifest) && manifest.id == id) {
        s_error = previousError;
        return manifest.version;
    }
    if (readInstalledManifest(secondary, manifest) && manifest.id == id) {
        s_error = previousError;
        return manifest.version;
    }
    s_error = previousError;
    return "";
}

bool removeUserPackage(const String& id) {
    s_error = "";
    if (!validId(id)) return fail("Invalid package id");
    if (id.startsWith("openos.")) return fail("System package ids cannot be removed here");
    String path = "/packages/" + id;
    if (!SD.exists(path.c_str())) return fail("Package is not installed");
    removeTree(path);
    if (SD.exists(path.c_str())) return fail("Could not remove package");
    s_installedCacheReady = false;
    s_restartRequired = true;
    return true;
}

bool isOfficialSystemId(const String& id) {
    static const char* allowed[] = {
        "openos.home", "openos.lockscreen", "openos.controlcenter",
        "openos.settings", "openos.files", "openos.clock",
        "openos.calculator", "openos.notes", "openos.compiler",
        "openos.openstore"
    };
    for (const char* candidate : allowed) if (id == candidate) return true;
    return false;
}

bool isTrustedSystemEntry(const String& path) {
    const String prefix = "/system/packages/";
    if (!path.startsWith(prefix)) return false;
    int slash = path.indexOf('/', prefix.length());
    if (slash < 0) return false;
    String id = path.substring(prefix.length(), slash);
    if (!isOfficialSystemId(id)) return false;
    String expected = installedEntry(id, true);
    return expected.length() > 0 && path == expected;
}

String resolveSystemEntry(const String& id, const String& legacyPath) {
    String packaged = installedEntry(id, true);
    return packaged.length() > 0 ? packaged : legacyPath;
}

String lastError() { return s_error; }
bool restartRequired() { return s_restartRequired; }
void clearRestartRequired() { s_restartRequired = false; }

} // namespace PackageManager
