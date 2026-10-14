#include "bake/PackWriter.h"

#include "core/Hash.h"

#include <lz4hc.h>

#include <algorithm>
#include <cstring>

namespace vd::bake {
namespace {

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

PackWriter::~PackWriter() {
    if (m_file) {
        std::fclose(m_file);
    }
}

Result<void> PackWriter::create(const std::filesystem::path& path, u32 bakerVersion) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    m_file = std::fopen(path.string().c_str(), "wb");
    if (!m_file) {
        return Error{"Cannot create " + path.string()};
    }
    m_path = path;
    m_bakerVersion = bakerVersion;
    const pack::PackHeader placeholder{};
    VD_TRY(writeAt(0, &placeholder, sizeof(placeholder)));
    m_offset = sizeof(placeholder);
    return {};
}

void PackWriter::setLocale(std::string_view locale) {
    std::memset(m_locale, 0, sizeof(m_locale));
    std::memcpy(m_locale, locale.data(), std::min(locale.size(), sizeof(m_locale)));
}

Result<void> PackWriter::writeAt(u64 offset, const void* data, usize size) {
#if defined(_WIN32)
    const int seeked = _fseeki64(m_file, static_cast<long long>(offset), SEEK_SET);
#else
    const int seeked = fseeko(m_file, static_cast<off_t>(offset), SEEK_SET);
#endif
    if (seeked != 0 ||
        std::fwrite(data, 1, size, m_file) != size) {
        return Error{"Write failed: " + m_path.string()};
    }
    return {};
}

Result<void> PackWriter::add(std::string_view path, pack::AssetType type, std::span<const u8> data, bool compress) {
    const std::string name = normalizeAssetPath(path);
    const u64 hash = assetPathHash(name);
    if (data.size() > 0xFFFFFFFFu) {
        return Error{"Asset too large: " + name};
    }

    // Compress outside the lock: this is the expensive part and callers add from many threads.
    std::vector<u8> compressed;
    pack::Compression compression = pack::Compression::None;
    std::span<const u8> stored = data;
    if (compress && !data.empty()) {
        compressed.resize(static_cast<usize>(LZ4_compressBound(static_cast<int>(data.size()))));
        const int size = LZ4_compress_HC(reinterpret_cast<const char*>(data.data()),
                                         reinterpret_cast<char*>(compressed.data()), static_cast<int>(data.size()),
                                         static_cast<int>(compressed.size()), LZ4HC_CLEVEL_DEFAULT);
        if (size > 0 && static_cast<usize>(size) < data.size()) {
            compression = pack::Compression::Lz4;
            stored = std::span<const u8>(compressed.data(), static_cast<usize>(size));
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto [it, inserted] = m_pathsByHash.emplace(hash, name); !inserted) {
        return Error{it->second == name ? "Duplicate asset " + name
                                        : "Path hash collision: " + name + " vs " + it->second};
    }
    const u64 offset = alignUp(m_offset, pack::kBlobAlignment);
    VD_TRY(writeAt(offset, stored.data(), stored.size()));
    m_offset = offset + stored.size();
    m_rawBytes += data.size();

    pack::PackEntry entry{};
    entry.pathHash = hash;
    entry.offset = offset;
    entry.storedSize = static_cast<u32>(stored.size());
    entry.rawSize = static_cast<u32>(data.size());
    entry.nameOffset = static_cast<u32>(m_names.size());
    entry.type = static_cast<u16>(type);
    entry.compression = static_cast<u16>(compression);
    m_entries.push_back(entry);
    m_names.append(name).push_back('\0');
    return {};
}

Result<void> PackWriter::finish() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::sort(m_entries.begin(), m_entries.end(),
              [](const pack::PackEntry& a, const pack::PackEntry& b) { return a.pathHash < b.pathHash; });

    pack::PackHeader header{};
    std::memcpy(header.magic, pack::kPackMagic, 4);
    header.version = pack::kPackVersion;
    header.bakerVersion = m_bakerVersion;
    header.clientBuild = pack::kClientBuild;
    header.entriesOffset = alignUp(m_offset, pack::kBlobAlignment);
    header.entryCount = static_cast<u32>(m_entries.size());
    header.namesOffset = header.entriesOffset + m_entries.size() * sizeof(pack::PackEntry);
    header.namesSize = m_names.size();
    std::memcpy(header.locale, m_locale, sizeof(header.locale));

    VD_TRY(writeAt(header.entriesOffset, m_entries.data(), m_entries.size() * sizeof(pack::PackEntry)));
    VD_TRY(writeAt(header.namesOffset, m_names.data(), m_names.size()));
    VD_TRY(writeAt(0, &header, sizeof(header)));
    m_offset = header.namesOffset + header.namesSize;
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    return closed ? Result<void>{} : Result<void>{Error{"Close failed: " + m_path.string()}};
}

} // namespace vd::bake
