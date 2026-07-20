#pragma once

#include <Arduino.h>

struct OPKManifest {
    int schema = 0;
    String id;
    String name;
    String version;
    int versionCode = 0;
    String entry;
    String scope;
    bool isApp = true;
};

namespace PackageManager {

static constexpr const char* DEFAULT_CATALOG_URL =
    "https://raw.githubusercontent.com/openplace1/OpenStore/main/store/catalog.json";
static constexpr const char* DEFAULT_SYSTEM_PACKAGE_PREFIX =
    "https://raw.githubusercontent.com/openplace1/OpenStore/main/store/packages/";

// Removes interrupted staging files/directories. Safe to call on every boot.
void begin();

// Store network operations. Packages are downloaded directly to SD and never
// held as one in-memory String.
String fetchCatalog();
bool refreshCatalog();
void clearCatalog();
// Sources are persisted in /user/config.ini. This lets the Store live in a
// separate repository/CDN without rebuilding the kernel.
String catalogSourceUrl();
bool setCatalogSourceUrl(const String& url);
String systemPackageSourcePrefix();
bool setSystemPackageSourcePrefix(const String& prefix);
int catalogCount();
String catalogId(int index);
String catalogName(int index);
String catalogVersion(int index);
int catalogVersionCode(int index);
String catalogScope(int index);
String catalogSummary(int index);
String catalogDeveloper(int index);
String catalogDescription(int index);
uint16_t catalogColor(int index);
String catalogUrl(int index);
String catalogSha256(int index);
// Fast, allocation-light helpers used by the OpenStore list renderer.
// `systemTab=false` selects user apps, true selects system apps.
int catalogVisibleCount(bool systemTab);
int catalogVisibleIndex(bool systemTab, int slot);
int catalogItemState(int index);       // 0=GET, 1=UPDATE, 2=INSTALLED, 3=NEWER
bool catalogCanUninstall(int index);
bool installFromUrl(const String& url, const String& expectedSha256,
                    const String& expectedId, const String& expectedScope);

// Installed package queries.
bool readManifest(const String& packageDir, OPKManifest& out);
String installedEntry(const String& id, bool systemPackage);
int installedVersionCode(const String& id);
String installedVersion(const String& id);
bool removeUserPackage(const String& id);

// Only fixed, official system package IDs may receive privileged OSA access.
bool isOfficialSystemId(const String& id);
bool isTrustedSystemEntry(const String& path);
String resolveSystemEntry(const String& id, const String& legacyPath);

String lastError();
bool restartRequired();
void clearRestartRequired();

} // namespace PackageManager
