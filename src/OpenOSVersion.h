#pragma once

namespace OpenOSBuild {

static constexpr const char* VERSION_NAME = "1.2.11";
static constexpr int VERSION_CODE = 14;
static constexpr int OSA_SDK_VERSION = 3;
static constexpr int OTA_UPDATER_VERSION_CODE = 1;
static constexpr const char* OTA_TARGET = "denky32-wroom32";
static constexpr const char* OTA_PARTITION_SCHEME = "openos-dual-v1";
static constexpr const char* OTA_IMAGE_MARKER =
    "OPENOS-OTA-IMAGE-V1|target=denky32-wroom32|partition=openos-dual-v1|"
    "version=1.2.11|versionCode=14|";

} // namespace OpenOSBuild
