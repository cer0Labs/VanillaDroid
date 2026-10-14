#pragma once

#include "SetupCommon.h"
#include "core/Result.h"
#include "core/Types.h"
#include "pack/ReleaseManifest.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (updates, ADR-0005 §7): VanillaDroid Setup updating itself and fetching the new APK. The latest
// release's manifest.json and its signature come from GitHub (pack::kReleaseManifestUrl) with the system's
// curl, as the ADB platform-tools do; nothing is used unless the signature verifies against the key built
// into the program and each file's SHA-256 is the manifest's (pack/ReleaseManifest.h). A newer Setup is
// unpacked next to the installed one and takes its place: on Windows a running program cannot be overwritten
// but can be renamed, so every file it replaces becomes "<name>.old" first (deleted on a later start) and all
// is put back if a step fails; on macOS the whole .app is swapped. Then the new program runs again with the
// same arguments (relaunch). The new APK goes to <data folder>/apk/, where `serve` and `adb install` find it
// (newestApk). Plain functions with Operation progress, for the console (vdsetup update, status) and the
// window; errors are already in the player's language.
namespace vd::setup {

inline constexpr const char* kUpdateCacheFile = "update.json";
inline constexpr const char* kUpdateFolder =
    "update"; // in the data folder: the manifest, its signature, the zip
inline constexpr i64 kUpdateCheckSeconds = 6 * 3600; // `status` (and the window) check at most this often...
inline constexpr i64 kUpdateRetrySeconds = 3600;     // ...and wait this long after a failed check
inline constexpr const char* kStagingPrefix = ".vdsetup-update-";
inline constexpr const char* kOldSuffix = ".old";

// The manifest's URL: `override` (vdsetup --manifest-url) or $VD_UPDATE_URL in development builds, else the
// latest release's (the only one a distribution build knows).
std::string updateManifestUrl(std::string_view override);
// Its signature's: the same URL + ".sig".
std::string signatureUrlOf(std::string_view manifestUrl);

// Seconds since 1970 (the cache's clock).
i64 unixNow();

// What the last check found, in <data folder>/update.json: `status` reads it instead of asking every time.
struct UpdateCache {
    i64 checkedAt = 0;   // the last check that worked (0: none)
    i64 attemptedAt = 0; // the last try, worked or not
    std::string manifestUrl;
    std::string version; // the release's
    u32 versionCode = 0;
    std::string notes;
    bool tooNew = false; // a manifest format this program cannot read

    // A check for `url` at `now` would tell nothing new: a recent one worked, or failed a moment ago.
    bool fresh(i64 now, std::string_view url) const;
};
UpdateCache loadUpdateCache(const std::filesystem::path& dataDir); // missing or damaged: empty
Result<void> saveUpdateCache(const std::filesystem::path& dataDir, const UpdateCache& cache);

// Downloads `url` to `file` with the system's curl (into "<file>.part", renamed once whole). Progress
// "download" with `label`; `maxSeconds` 0: no time limit. `httpStatus`, when given, gets the server's HTTP
// status (also when the download failed: 404...; 0 when none came).
Result<void> curlDownload(std::string_view url, const std::filesystem::path& file, const Operation& operation,
                          std::string_view label, u32 maxSeconds, u32* httpStatus = nullptr);

struct UpdateCheck {
    pack::SignedRelease release; // Ok or TooNew (the other outcomes are errors)
    bool newer = false;          // the release's versionCode is above this program's
    // Phase 22 (polish): false when no release is published yet (the manifest's URL answers 404, as GitHub's
    // does before the first release): nothing newer, not an error; `release` is then empty.
    bool published = true;
};
// Downloads the manifest and its signature into <dataDir>/update/, verifies and reads them, and records the
// outcome in the cache (a failure too, so `status` does not ask again at once).
Result<UpdateCheck> checkForUpdate(const std::filesystem::path& dataDir, std::string_view manifestUrl,
                                   const Operation& operation, u32 maxSeconds = 60);

// Phase 22 (polish): what the last check found for `url`, as `vdsetup status` and the window both say it (the
// same keys, VD_SETUP_STATUS_UPDATE_*: the window has its own wording of them): not checked yet, this is the
// latest version (also when no release is published yet), a newer one, or one this program cannot install.
// Info, or Warning when there is something newer.
Message updateStatusMessage(const UpdateCache& cache, std::string_view url);

// A release's file downloaded to `file` and checked (its size and SHA-256 are the manifest's); a file already
// there and right is kept. A wrong download is deleted.
Result<void> downloadReleaseFile(const pack::ReleaseFile& source, const std::filesystem::path& file,
                                 const Operation& operation, std::string_view label);
// The file name at the end of a URL ("https://x/y/VanillaDroid-0.2.0.apk?a=b" -> "VanillaDroid-0.2.0.apk"),
// or `fallback` when it has none safe to use.
std::string fileNameOfUrl(std::string_view url, std::string_view fallback);

// --- Replacing the installed Setup
// ----------------------------------------------------------------------------

// Where this program lives and what an update replaces.
struct InstallLayout {
    std::filesystem::path program; // the running executable
    std::filesystem::path dir;     // the folder whose entries the update's take the place of
    std::filesystem::path bundle;  // macOS: the .app the program runs from (swapped whole); empty elsewhere
};
std::filesystem::path runningProgram();
InstallLayout installLayout(const std::filesystem::path& program);
// An installed Setup (unpacked from a release's zip: vd_baker next to the program, or the program inside its
// .app), not a development build, which an update must never overwrite.
bool isInstalledSetup(const InstallLayout& layout);

// Unpacks a Setup zip into a staging folder in layout.dir (the same volume: every move is a rename) and
// checks that it holds what this program needs to run again: the running program's file and vd_baker
// (Windows), or one .app with the program inside (macOS). Returns the staging folder.
Result<std::filesystem::path> unpackSetup(const std::filesystem::path& zip, const InstallLayout& layout,
                                          std::string_view version, const Operation& operation);
// The same check alone (tests).
Result<void> checkStagedSetup(const std::filesystem::path& staged, const InstallLayout& layout);

// Moves the staged entries into layout.dir (the staged .app to the bundle's place on macOS). Each entry it
// replaces is renamed to "<name>.old" first; if a step fails, what was moved goes back and the old names
// return. The staging folder is removed. Returns how many entries were replaced or added.
Result<u32> replaceInstall(const std::filesystem::path& staged, const InstallLayout& layout);

// Removes what an earlier update left in layout.dir: "<name>.old" entries whose <name> exists, and staging
// folders. What cannot go yet (the replaced program may still be running) stays for a later start.
void removeUpdateLeftovers(const InstallLayout& layout);

// Runs `program` again with `args`. Windows: a new process sharing this one's console and standard handles;
// with `wait` it returns the new one's exit code once it ends (the console stays the player's), otherwise at
// once. Elsewhere this process becomes the program (exec): it returns only on failure.
Result<int> relaunch(const std::filesystem::path& program, const std::vector<std::string>& args, bool wait);

// --- The APK
// --------------------------------------------------------------------------------------------------

// Where `vdsetup update` keeps the APK of a release: <dataDir>/apk/.
std::filesystem::path downloadedApkDir(const std::filesystem::path& dataDir);
// The APKs there (any name), newest first by versionCode.
std::vector<std::filesystem::path> downloadedApks(const std::filesystem::path& dataDir);
// The newest by versionCode among `candidates` (those that cannot be read are skipped; on a tie the first);
// empty when none can be read.
std::filesystem::path newestApk(std::span<const std::filesystem::path> candidates);
// Deletes the downloaded APKs other than `keep`.
void removeOtherApks(const std::filesystem::path& dataDir, const std::filesystem::path& keep);

} // namespace vd::setup
