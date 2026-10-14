#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

// Phase 22 (ADR-0005 §4): what VanillaDroid Setup remembers between runs, in setup.json in the player's data
// folder (Windows %LOCALAPPDATA%\VanillaDroid, macOS ~/Library/Application Support/VanillaDroid, elsewhere
// $XDG_DATA_HOME/VanillaDroid or ~/.local/share/VanillaDroid; $VD_SETUP_DIR or --data-dir instead, for
// tests). Paths are stored as UTF-8.
namespace vd::setup {

inline constexpr const char* kDistAppId = "com.cer0labs.vanilladroid"; // the published APK (dist)
inline constexpr const char* kDevAppId = "dev.cer.vanilladroid";       // the development builds
inline constexpr u16 kDefaultPort = 47800;
inline constexpr const char* kSettingsFile = "setup.json";

struct Settings {
    std::string language;               // the tool's language ("enUS", "esES"); empty: the system's
    std::filesystem::path wow;          // the WoW 1.12.1 folder the packs are baked from
    std::filesystem::path localeClient; // optional: a client of the same build in another language
    std::vector<std::string>
        includeArchives;         // archives that are not official the player chose to bake anyway
    std::filesystem::path packs; // empty: <data folder>/packs
    u16 port = kDefaultPort;     // the local server's first port
    std::string pairingKey;      // 128 bits in hex; made on the first `serve`
    std::filesystem::path adb;   // empty: found (our platform-tools, the Android SDK, PATH)
    std::filesystem::path apk;   // empty: the APK next to the program
    std::string appId = kDistAppId;
    std::string serial;   // the adb device chosen with --serial
    std::string lastBake; // what the latest bake read (bakeFingerprint): another input re-bakes

    // A missing file gives the defaults; a damaged one is an error.
    static Result<Settings> load(const std::filesystem::path& file);
    // Written to a temporary name, then renamed; creates the folder.
    Result<void> save(const std::filesystem::path& file) const;
};

// The data folder: `overrideDir` when set, else $VD_SETUP_DIR, else the system's place (see above).
std::filesystem::path dataDir(const std::filesystem::path& overrideDir = {});
// Where the packs are baked to and served from.
std::filesystem::path packsDir(const Settings& settings, const std::filesystem::path& dataDir);

// A new pairing key: 16 random bytes in lowercase hex (empty if the system has no random source).
std::string newPairingKey();
// A `serve` session's 6-digit code ("042917"), every value equally likely (empty without a random source). It
// only unlocks the key through GET /pair (PhoneServer.h): 6 digits cannot carry 128 bits.
std::string newPairingCode();

} // namespace vd::setup
