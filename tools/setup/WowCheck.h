#pragma once

#include "PeVersion.h"
#include "SetupCommon.h"
#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0005 §4): is this folder a WoW 1.12.1 (5875) the packs can be baked from? Its Data folder
// (whatever its case) with the official archives, each whole (an MPQ header whose sizes fit the file: a copy
// cut short is caught without reading 5 GB); WoW.exe's version resource (or a Mac client's Info.plist) when
// there is one; the client's language from its DBC texts (as vd_baker scan-locale), refusing the alphabets
// the fonts cannot show (ADR-0004). Archives that are not official (a private server's patch-3.MPQ or
// patch-A.MPQ, copies) are listed and, unless the player includes them, the baker skips them (vd_baker
// --skip-archives).
namespace vd::setup {

enum class ArchiveKind : u8 {
    Game,       // official, read by the baker
    Launcher,   // official, only the launcher's files (base.MPQ, backup.MPQ): not needed
    Unofficial, // anything else
};

enum class ArchiveState : u8 {
    Ok,
    NotMpq,    // no MPQ header
    Truncated, // its header says it is larger than the file: a copy cut short
    Unreadable,
};

struct ArchiveInfo {
    std::string name; // as spelled on disk
    u64 size = 0;
    ArchiveKind kind = ArchiveKind::Unofficial;
    ArchiveState state = ArchiveState::Ok;
};

// The official 1.12.1 archives, by kind (the same names in every language: the enUS and esES clients
// checked).
std::span<const std::string_view> officialGameArchives();
std::span<const std::string_view> officialLauncherArchives();
ArchiveKind archiveKind(std::string_view fileName);

// The MPQ header of an archive's first bytes against the file's size. `head` holds at least the first 1 KB
// (or the whole file); only an archive at offset 0 or after a user-data block is recognized.
ArchiveState inspectMpqHeader(std::span<const u8> head, u64 fileSize);
ArchiveState inspectMpqFile(const std::filesystem::path& file);

// Languages whose texts the baked fonts can draw (ADR-0004). enGB reads as enUS (the same column of the
// texts).
bool isSupportedLocale(std::string_view locale);

struct ClientCheck {
    std::filesystem::path folder;  // the client's folder (the parent when the player chose its Data folder)
    std::filesystem::path dataDir; // its Data folder, empty when none was found
    std::vector<ArchiveInfo> archives; // every *.MPQ of Data, sorted by name
    std::vector<std::string> missing;  // official game archives that are not there
    bool hasExe = false;
    bool exeVersionRead = false;
    FileVersion exeVersion;
    std::string macVersion;        // CFBundleVersion of a Mac client's .app, when one was found
    std::string locale;            // the client's language ("enUS", "esES"), empty when unknown
    std::vector<Message> messages; // what to tell the player, in order
    bool usable = false;           // nothing fatal: the packs can be baked from it

    // The archives the baker must not read: the unofficial ones not in `include` (names, any case).
    std::vector<std::string> skippedArchives(std::span<const std::string> include) const;
    // The official game archives that are there and whole (what `serve --wow` and `adb send-wow` offer).
    std::vector<ArchiveInfo> gameArchives() const;
    u64 gameBytes() const;
};

// Checks a client folder. `include`: unofficial archives the player chose to bake anyway. Reads only headers,
// the version resource and three DBC tables; never runs anything.
ClientCheck checkClient(const std::filesystem::path& folder, std::span<const std::string> include = {});

// A fingerprint of what a bake reads: the clients' languages and their archives (names and sizes, the skipped
// ones out). Another fingerprint than the last bake's means every pack is baked again (Settings::lastBake).
std::string bakeFingerprint(const ClientCheck& base, const ClientCheck* localeClient,
                            std::span<const std::string> include);

} // namespace vd::setup
