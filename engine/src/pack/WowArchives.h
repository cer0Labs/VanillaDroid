#pragma once

#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): is the WoW copied to the phone (<app files>/wow/Data) one
// the packs can be baked from? The same rules as VanillaDroid Setup's check of a client folder
// (tools/setup/WowCheck): the 12 archives of the 1.12.1 client the baker reads, each whole (its MPQ header's
// sizes fit the file: a copy cut short is caught without reading 5 GB). Archives that are not official (a
// private server's patch-3.MPQ) are listed: the bake leaves them out (vd_baker --skip-archives). No StormLib
// here: the game itself checks it.
namespace vd::pack {

// The official game archives of 1.12.1 (the same names in every language: the enUS and esES clients checked).
std::span<const std::string_view> wowGameArchives();
// base.MPQ and backup.MPQ: official, but only the launcher's files (nothing the baker reads).
bool isWowLauncherArchive(std::string_view fileName);
bool isWowGameArchive(std::string_view fileName); // any case

enum class MpqState : u8 {
    Ok,
    NotMpq,    // no MPQ header
    Truncated, // its header says it is larger than the file: a copy cut short
    Unreadable,
};
// The MPQ header of an archive's first bytes against the file's size. `head` holds the file's first 64 KB (or
// the whole file); only an archive at offset 0 or after a user-data block is recognized.
MpqState inspectMpqHeader(std::span<const u8> head, u64 fileSize);
MpqState inspectMpqFile(const std::filesystem::path& file);

// A client's Data folder whatever its case (<clientDir>/Data, /data, /DATA); empty when there is none.
std::filesystem::path findWowDataDir(const std::filesystem::path& clientDir);

struct WowDataCheck {
    std::vector<std::string> missing;    // game archives that are not there
    std::vector<std::string> damaged;    // there, but not an MPQ or cut short (as spelled on disk)
    std::vector<std::string> unofficial; // other *.MPQ: left out of the bake (as spelled on disk)
    u32 archives = 0;                    // *.MPQ files found, of any kind
    u64 gameBytes = 0;                   // the game archives that are whole
    bool complete() const { return archives > 0 && missing.empty() && damaged.empty(); }
};
// Reads the headers of the archives in `dataDir` (12 x 64 KB): not for the game thread.
WowDataCheck checkWowData(const std::filesystem::path& dataDir);

} // namespace vd::pack
