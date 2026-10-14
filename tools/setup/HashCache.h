#pragma once

#include "SetupCommon.h"
#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// Phase 22 (ADR-0005 §5): the SHA-256 of the files that go to the phone (packs, the WoW's archives, the
// APK). 4.5 GB of packs and 5 GB of archives take a while to read, so each digest is kept in <data
// folder>/hashes.json with the file's size and modification time, and taken again only when one of them
// changes.
namespace vd::setup {

// The modification time as the file system's clock counts it (stable on one machine; only compared for
// equality).
i64 fileTimeOf(const std::filesystem::path& file);

class HashCache {
public:
    HashCache() = default;
    explicit HashCache(std::filesystem::path file) : m_file(std::move(file)) {}

    // A missing or damaged file is an empty cache (it only saves time).
    void load();
    Result<void> save() const;

    // The digest kept for `file` when its size and time are still these; empty otherwise.
    std::string find(const std::filesystem::path& file, u64 size, i64 time) const;
    void store(const std::filesystem::path& file, u64 size, i64 time, std::string sha256);

private:
    struct Entry {
        u64 size = 0;
        i64 time = 0;
        std::string sha256;
    };
    static std::string keyOf(const std::filesystem::path& file);

    std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
    bool m_dirty = false;
};

// One file to digest, and its result.
struct HashJob {
    std::filesystem::path file;
    u64 size = 0;
    i64 time = 0;
    std::string sha256; // filled from the cache or computed
};

// The SHA-256 of a file in 1 MB blocks; `onBytes` receives each block's size (any thread). Stops when
// cancelled.
Result<std::string> sha256FileProgress(const std::filesystem::path& file,
                                       const std::function<void(u64)>& onBytes, const CancelToken* cancel);

// Fills every job's sha256: from the cache when the size and time match (stat'ed here), computed otherwise on
// a few threads (reading several files at once keeps a fast disk busy), storing the new ones in the cache.
// Reports "hash" progress by bytes with the text VD_SETUP_HASHING. Errors name the first file that failed.
Result<void> hashFiles(std::span<HashJob> jobs, HashCache& cache, const Operation& operation);

} // namespace vd::setup
