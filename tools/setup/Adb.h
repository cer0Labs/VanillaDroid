#pragma once

#include "SetupCommon.h"
#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0005 §4): the ADB way, for players who prefer a cable or wireless debugging to the local
// server. Google's platform-tools are downloaded only when the player accepts their terms (never shipped with
// the Setup). Files go to the app's own external folder (/sdcard/Android/data/<appId>/files), which exists
// once the app ran: packs and manifest.json to packs/, the WoW's official archives to wow/Data/. Each file is
// pushed to <name>.part, checked with the phone's sha256sum and renamed, so a cut cable never leaves half a
// pack under its real name; files whose digest on the phone already matches are skipped.
namespace vd::setup {

struct AdbDevice {
    std::string serial;
    std::string state; // "device", "unauthorized", "offline"...
    std::string model; // "SM_F946B" (from -l)
};

// `adb devices -l` output.
std::vector<AdbDevice> parseAdbDevices(std::string_view output);
// `sha256sum` output ("<hex>  <path>" lines) -> file name (the path's last part) -> digest.
std::map<std::string, std::string> parseSha256sum(std::string_view output);
// `stat -c '%s %n'` output -> file name -> size.
std::map<std::string, u64> parseStatSizes(std::string_view output);
// A word for the device's shell, single-quoted: it's -> 'it'\''s'.
std::string shellQuote(std::string_view text);
// /sdcard/Android/data/<appId>/files
std::string remoteFilesDir(std::string_view appId);

// Google's platform-tools for this system, and the terms the player accepts before downloading them.
std::string_view platformToolsUrl();
std::string_view platformToolsTermsUrl();

// adb: `explicitPath` (--adb or the settings), else our platform-tools in the data folder, else the Android
// SDK's (ANDROID_HOME, ANDROID_SDK_ROOT, the usual install folders), else the PATH. Empty when there is none.
std::filesystem::path findAdb(const std::filesystem::path& explicitPath,
                              const std::filesystem::path& dataDir);

// Downloads and unpacks platform-tools into <dataDir>/platform-tools with the system's curl and tar (Windows
// 10+), ditto (macOS) or unzip; returns adb's path.
Result<std::filesystem::path> installPlatformTools(const std::filesystem::path& dataDir,
                                                   const Operation& operation);

// What went wrong in adb's words, mapped to one of our texts when we know it
// (INSTALL_FAILED_UPDATE_INCOMPATIBLE...).
Message explainAdbFailure(std::string_view output);

class Adb {
public:
    Adb(std::filesystem::path program, std::string serial)
        : m_program(std::move(program)), m_serial(std::move(serial)) {}

    const std::filesystem::path& program() const { return m_program; }
    const std::string& serial() const { return m_serial; }
    void setSerial(std::string serial) { m_serial = std::move(serial); }

    // Runs adb with these arguments (with -s <serial> when one is chosen). The output is returned when it
    // exits with 0; otherwise an error holding the output.
    Result<std::string> run(const std::vector<std::string>& args, const CancelToken* cancel = nullptr) const;
    // `adb shell <command>`: the output whatever the exit code (a missing file in a list is not an error
    // here).
    Result<std::string> shell(std::string_view command, const CancelToken* cancel = nullptr) const;

    Result<std::vector<AdbDevice>> devices() const;
    // The one device to use: the chosen serial, or the only one connected. Errors are keys of our texts.
    Result<AdbDevice> chooseDevice() const;
    Result<std::string> pair(std::string_view hostPort, std::string_view code) const;
    Result<std::string> connect(std::string_view hostPort) const;
    Result<bool> isInstalled(std::string_view appId) const;
    Result<void> install(const std::filesystem::path& apk, const Operation& operation) const;

private:
    std::filesystem::path m_program;
    std::string m_serial;
};

struct PushItem {
    std::filesystem::path local;
    std::string name; // the file name on the phone
    u64 size = 0;
    std::string sha256;
};

struct PushOutcome {
    u32 pushed = 0;
    u32 skipped = 0; // already on the phone
    u64 bytes = 0;   // pushed
};

// Pushes the items (in order: put manifest.json last) to `remoteDir`, skipping those the phone already has.
// Progress "push" by bytes, with a poll of the file's size on the phone while a big one goes.
Result<PushOutcome> pushFiles(const Adb& adb, const std::string& remoteDir, std::span<const PushItem> items,
                              const Operation& operation);

} // namespace vd::setup
