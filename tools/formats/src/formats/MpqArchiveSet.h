#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::formats {

// Read-only view over the 1.12 client's Data/*.MPQ archives with the client's override order:
// patch-N.MPQ (highest N first) > patch-2.MPQ > patch.MPQ > base archives. In vanilla, patches
// replace whole files, so the first archive that contains a file wins. 0-byte entries (patch
// "deletion" markers) are skipped in favour of lower-priority archives.
//
// Paths may use '/' or '\' and any case. Reads are serialized (StormLib handles are not thread-safe);
// decoding and encoding work happens outside the lock.
class MpqArchiveSet {
public:
    MpqArchiveSet() = default;
    ~MpqArchiveSet();
    MpqArchiveSet(const MpqArchiveSet&) = delete;
    MpqArchiveSet& operator=(const MpqArchiveSet&) = delete;

    // Phase 22: `skipArchives` are file names (any case) left out: archives that are not official (a private
    // server's patch-3.MPQ...), which VanillaDroid Setup does not bake unless the player includes them.
    Result<void> open(const std::filesystem::path& dataDir, std::span<const std::string> skipArchives = {});
    void close();

    bool contains(std::string_view path) const;
    Result<std::vector<u8>> read(std::string_view path) const;

    // Unique file names (as spelled in the highest-priority archive) matching a StormLib wildcard
    // mask such as "*.blp" or "DBFilesClient\\*.dbc". Uses the archives' internal listfiles.
    // `onlyArchive` (e.g. "backup.MPQ") restricts the listing to one archive, for diagnostics.
    std::vector<std::string> list(const char* mask = "*", std::string_view onlyArchive = {}) const;

    // Archive file names, highest priority first.
    std::vector<std::string> archiveNames() const;

private:
    struct Archive {
        std::string name;
        void* handle = nullptr;
    };
    std::vector<Archive> m_archives;
    mutable std::mutex m_mutex;
};

// MPQ spelling: backslashes. Used for StormLib lookups.
std::string toMpqPath(std::string_view path);

// Phase 22: a client's Data folder whatever its case ("Data", "data": case-sensitive file systems), else
// <clientDir>/Data.
std::filesystem::path findDataDir(const std::filesystem::path& clientDir);

} // namespace vd::formats
