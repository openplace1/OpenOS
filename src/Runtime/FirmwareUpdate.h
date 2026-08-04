#pragma once

#include <Arduino.h>

namespace FirmwareUpdate {

using ProgressCallback = void (*)(const char* phase, size_t completed,
                                  size_t total, void* context);

static constexpr const char* DEFAULT_INFO_URL =
    "https://raw.githubusercontent.com/openplace1/OpenStore/main/update/info.json";

// Clears transient state. The configured source is persisted separately in
// NVS, so it remains available even when the removable SD card is absent.
void begin();

// Source configuration. An empty value restores DEFAULT_INFO_URL.
String sourceUrl();
bool setSourceUrl(const String& requested);

// Returns 1 when a newer signed release is available, 0 when the installed
// version is current/newer, and -1 on network, schema or signature failure.
int check();
bool available();
bool supported();

String remoteName();
String remoteVersion();
int remoteVersionCode();
String releaseChannel();
String releaseType();
String releaseDescription();
String publishedAt();
size_t downloadSize();

// Installs only the release cached by a successful check(). No URL, hash or
// target can be supplied by OSA. On success the next OTA slot is bootable but
// the caller decides when to restart.
bool install(ProgressCallback progress = nullptr, void* context = nullptr);

bool canRollback();
bool rollback();
String lastError();

// Arduino's default core validates a new image before setup(). OpenOS delays
// that decision until display/touch/application startup has completed and the
// main loop has stayed alive for a short probation window.
void beginBootValidation();
void pollBootValidation();
bool pendingBootValidation();

} // namespace FirmwareUpdate
