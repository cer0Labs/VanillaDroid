#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackFormat.h"
#include "platform/MappedFile.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace vd::pack {

struct PackAsset {
    const u8* data = nullptr; // points into the mapped file: valid while the reader is open
    u32 size = 0;             // stored size
    u32 rawSize = 0;
    AssetType type = AssetType::Dbc;
    Compression compression = Compression::None;
    std::string_view name; // canonical path, empty if the pack has no name table
};

// Memory-mapped VDPK pack. Lookups are a binary search over the sorted entry table; asset data is
// used in place (no copy) until it is uploaded to the GPU or parsed.
//
// Phase 21 (languages): a pack may carry a language overlay, "<name>.<locale>.vdpk" next to it, baked from a client in
// another language (vd_baker bake --only locale): only the assets that language changes (its DBC texts, the world map
// tiles with painted names, the creature voices), under the same paths. find() and entry() return the overlay's asset
// when it has one; findBase() ignores it (the client's own language: what the server matches channel names against).
class PackReader {
public:
    PackReader();
    ~PackReader();
    PackReader(PackReader&&) noexcept;
    PackReader& operator=(PackReader&&) noexcept;

    Result<void> open(const std::filesystem::path& path);
    void close(); // the overlay too

    // "<stem>.<locale>.vdpk" of the open pack, when that file exists (hasOverlay() says whether it did). An error
    // only when the file is there but cannot be used. Main thread, before the pack is shared with other threads.
    Result<void> openOverlay(std::string_view locale);
    bool hasOverlay() const { return m_overlay != nullptr; }
    const PackReader* overlay() const { return m_overlay.get(); }

    std::optional<PackAsset> find(u64 pathHash) const;
    std::optional<PackAsset> find(std::string_view path) const;
    std::optional<PackAsset> findBase(u64 pathHash) const;
    std::optional<PackAsset> findBase(std::string_view path) const;

    // Raw (decompressed) bytes of an asset into dst, which must hold asset.rawSize bytes. Safe to call
    // from any thread: the mapping is read-only.
    static Result<void> read(const PackAsset& asset, u8* dst, usize dstSize);

    // The base pack's entries (an overlay asset under the same path replaces one; assets only the overlay has are
    // reached by find() alone).
    u32 entryCount() const { return m_entryCount; }
    PackAsset entry(u32 index) const;
    u64 entryHash(u32 index) const { return m_entries[index].pathHash; } // entries are sorted by it
    const PackHeader& header() const { return *reinterpret_cast<const PackHeader*>(m_file.data()); }
    // The language of the client the pack was baked from ("esES"), empty when it does not depend on one.
    std::string_view locale() const;
    // The language of what find() returns: the overlay's, else the pack's own.
    std::string_view dataLocale() const { return m_overlay ? m_overlay->locale() : locale(); }

private:
    PackAsset baseEntry(u32 index) const;

    MappedFile m_file;
    std::filesystem::path m_path;
    std::unique_ptr<PackReader> m_overlay;
    const PackEntry* m_entries = nullptr;
    u32 m_entryCount = 0;
    const char* m_names = nullptr;
    u64 m_namesSize = 0;
};

} // namespace vd::pack
