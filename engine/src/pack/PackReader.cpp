#include "pack/PackReader.h"

#include "core/Hash.h"

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace vd::pack {

PackReader::PackReader() = default;
PackReader::~PackReader() = default;
PackReader::PackReader(PackReader&&) noexcept = default;
PackReader& PackReader::operator=(PackReader&&) noexcept = default;

Result<void> PackReader::open(const std::filesystem::path& path) {
    close();
    VD_TRY(m_file.open(path));
    m_path = path;
    // Every rejection must unmap the file (otherwise it stays locked on Windows).
    auto fail = [&](const std::string& reason) {
        close();
        return Error{reason + ": " + path.string()};
    };
    const usize fileSize = m_file.size();
    if (fileSize < sizeof(PackHeader)) {
        return fail("Pack too small");
    }
    const PackHeader& h = header();
    if (std::memcmp(h.magic, kPackMagic, 4) != 0) {
        return fail("Not a VDPK pack");
    }
    if (h.version != kPackVersion) {
        return fail("Pack version " + std::to_string(h.version) + " != supported " + std::to_string(kPackVersion) +
                    " (re-bake)");
    }
    if (h.clientBuild != kClientBuild) {
        return fail("Pack built from client build " + std::to_string(h.clientBuild));
    }
    const u64 entriesEnd = h.entriesOffset + u64{h.entryCount} * sizeof(PackEntry);
    if (h.entriesOffset % alignof(PackEntry) != 0 || entriesEnd > fileSize ||
        h.namesOffset + h.namesSize > fileSize) {
        return fail("Corrupt pack tables");
    }
    m_entries = reinterpret_cast<const PackEntry*>(m_file.data() + h.entriesOffset);
    m_entryCount = h.entryCount;
    m_names = h.namesSize ? reinterpret_cast<const char*>(m_file.data() + h.namesOffset) : nullptr;
    m_namesSize = h.namesSize;
    for (u32 i = 0; i < m_entryCount; ++i) {
        if (m_entries[i].offset + m_entries[i].storedSize > fileSize) {
            return fail("Corrupt pack entry " + std::to_string(i));
        }
    }
    return {};
}

void PackReader::close() {
    m_overlay.reset();
    m_path.clear();
    m_file.close();
    m_entries = nullptr;
    m_entryCount = 0;
    m_names = nullptr;
    m_namesSize = 0;
}

Result<void> PackReader::openOverlay(std::string_view locale) {
    m_overlay.reset();
    if (m_path.empty() || locale.empty()) {
        return {};
    }
    std::filesystem::path path = m_path;
    path.replace_extension(); // "db.vdpk" -> "db" -> "db.esES.vdpk"
    path += "." + std::string(locale) + ".vdpk";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return {};
    }
    auto overlay = std::make_unique<PackReader>();
    VD_TRY(overlay->open(path));
    if (overlay->locale().empty()) {
        return Error{"Not a language overlay (no locale in its header, re-bake): " + path.string()};
    }
    m_overlay = std::move(overlay);
    return {};
}

std::string_view PackReader::locale() const {
    if (!m_file.data()) {
        return {};
    }
    const char* code = header().locale;
    return {code, ::strnlen(code, sizeof(header().locale))};
}

PackAsset PackReader::entry(u32 index) const {
    if (m_overlay) {
        if (std::optional<PackAsset> replaced = m_overlay->find(m_entries[index].pathHash)) {
            return *replaced;
        }
    }
    return baseEntry(index);
}

PackAsset PackReader::baseEntry(u32 index) const {
    const PackEntry& e = m_entries[index];
    PackAsset asset;
    asset.data = m_file.data() + e.offset;
    asset.size = e.storedSize;
    asset.rawSize = e.rawSize;
    asset.type = static_cast<AssetType>(e.type);
    asset.compression = static_cast<Compression>(e.compression);
    if (m_names && e.nameOffset < m_namesSize) {
        const char* name = m_names + e.nameOffset;
        asset.name = std::string_view(name, ::strnlen(name, static_cast<usize>(m_namesSize - e.nameOffset)));
    }
    return asset;
}

std::optional<PackAsset> PackReader::find(u64 pathHash) const {
    if (m_overlay) {
        if (std::optional<PackAsset> replaced = m_overlay->find(pathHash)) {
            return replaced;
        }
    }
    return findBase(pathHash);
}

std::optional<PackAsset> PackReader::findBase(u64 pathHash) const {
    const PackEntry* end = m_entries + m_entryCount;
    const PackEntry* it =
        std::lower_bound(m_entries, end, pathHash, [](const PackEntry& e, u64 hash) { return e.pathHash < hash; });
    if (it == end || it->pathHash != pathHash) {
        return std::nullopt;
    }
    return baseEntry(static_cast<u32>(it - m_entries));
}

Result<void> PackReader::read(const PackAsset& asset, u8* dst, usize dstSize) {
    if (dstSize < asset.rawSize) {
        return Error{"Destination too small for asset"};
    }
    switch (asset.compression) {
    case Compression::None:
        std::memcpy(dst, asset.data, asset.size);
        return {};
    case Compression::Lz4: {
        const int size = LZ4_decompress_safe(reinterpret_cast<const char*>(asset.data), reinterpret_cast<char*>(dst),
                                             static_cast<int>(asset.size), static_cast<int>(asset.rawSize));
        if (size < 0 || static_cast<u32>(size) != asset.rawSize) {
            return Error{"Corrupt LZ4 asset"};
        }
        return {};
    }
    }
    return Error{"Unknown compression " + std::to_string(static_cast<u32>(asset.compression))};
}

std::optional<PackAsset> PackReader::find(std::string_view path) const {
    return find(assetPathHash(path));
}

std::optional<PackAsset> PackReader::findBase(std::string_view path) const {
    return findBase(assetPathHash(path));
}

} // namespace vd::pack
