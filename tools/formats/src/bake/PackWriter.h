#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackFormat.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::bake {

// Streams asset blobs into a VDPK file as they are produced (thread-safe add), then writes the
// sorted entry table and name table in finish(). Two paths hashing to the same value abort the bake.
class PackWriter {
public:
    PackWriter() = default;
    ~PackWriter();
    PackWriter(const PackWriter&) = delete;
    PackWriter& operator=(const PackWriter&) = delete;

    Result<void> create(const std::filesystem::path& path, u32 bakerVersion);
    // compress: store as LZ4 (high compression) when that saves space; readers get the raw bytes
    // back through PackReader::read().
    Result<void> add(std::string_view path, pack::AssetType type, std::span<const u8> data, bool compress = false);
    Result<void> finish();
    // Phase 21 (languages): the language of the client data the pack holds ("esES"; PackHeader::locale). Before
    // finish().
    void setLocale(std::string_view locale);

    u32 entryCount() const { return static_cast<u32>(m_entries.size()); }
    u64 bytesWritten() const { return m_offset; }
    u64 rawBytes() const { return m_rawBytes; }

private:
    Result<void> writeAt(u64 offset, const void* data, usize size);

    std::FILE* m_file = nullptr;
    std::filesystem::path m_path;
    u32 m_bakerVersion = 0;
    char m_locale[4] = {};
    u64 m_offset = 0;
    u64 m_rawBytes = 0; // sum of uncompressed asset sizes
    std::vector<pack::PackEntry> m_entries;
    std::string m_names;
    std::unordered_map<u64, std::string> m_pathsByHash;
    std::mutex m_mutex;
};

} // namespace vd::bake
