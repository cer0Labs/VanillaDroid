#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>

// Phase 22 (ADR-0005 §5): what an APK is, read from its binary AndroidManifest.xml (the zip's entry, stored
// or deflated; Android's binary XML): the package (com.cer0labs.vanilladroid or the development
// dev.cer.vanilladroid), versionCode and versionName. files.json tells the phone the versionCode, and `adb
// install` warns when the APK is not the app the Setup was told about.
namespace vd::setup {

struct ApkInfo {
    std::string package;
    u32 versionCode = 0;
    std::string versionName;
};

Result<ApkInfo> readApkInfo(const std::filesystem::path& apk);
// The manifest of a whole APK in memory, and the binary XML alone (tests).
Result<ApkInfo> readApkInfo(std::span<const u8> apk);
Result<ApkInfo> parseBinaryManifest(std::span<const u8> xml);

} // namespace vd::setup
