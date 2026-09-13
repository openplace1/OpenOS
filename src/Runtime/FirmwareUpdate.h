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
// True once check() has fetched and validated a manifest in this session,
// whatever the outcome. The cached result outlives the script that asked for
// it, so Settings can render the right state after leaving and coming back.
bool checked();
bool supported();

String remoteName();
String remoteVersion();
int remoteVersionCode();
String releaseChannel();
String releaseType();
String releaseDescription();
String publishedAt();
size_t downloadSize();

// Records the release cached by a successful check() for installation on the
// next boot, and does not download anything itself. No URL, hash or target
// can be supplied by OSA.
//
// The download runs from setup(), before any application is loaded, because
// a 2 MB TLS transfer needs a large contiguous block and a device that has
// been running for a while cannot offer one: the Wi-Fi stack keeps buffers it
// grew during earlier transfers, so the heap has plenty free but nothing big
// enough left in one piece. At boot it does.
bool stageForRestart();

// True when a staged release is waiting; the caller restarts to install it.
bool staged();

// Downloads, verifies and activates a staged release. Clears the record
// first, so a failure costs one attempt and never loops the device. Returns
// false with lastError() set; the caller carries on booting.
bool installStaged(ProgressCallback progress = nullptr, void* context = nullptr);

// Identity of the staged release, for the progress screen.
String stagedName();

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
