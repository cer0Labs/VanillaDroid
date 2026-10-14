#include "app/UiResources.h"

#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace vd {
namespace {

constexpr u64 kStagingBytes = 4ull << 20; // icons ~5 KB, minimap tiles ~45 KB

constexpr u64 kIconTag = 1ull << 62; // | slot

u64 tileKey(u64 mapHash, u32 x, u32 y) {
    return (mapHash ^ (u64{x} << 8 | y) * 0x9E3779B97F4A7C15ull) & ((1ull << 62) - 1);
}

} // namespace

UiResources::~UiResources() {
    m_streamer.stop();
}

bool UiResources::init(Renderer& renderer, const pack::PackReader* textures, const std::filesystem::path& uiPack,
                       const std::filesystem::path& appPack) {
    const Clock::time_point start = Clock::now();
    if (Result<void> opened = m_assets.open(uiPack, appPack); !opened) {
        VD_LOG_WARN("UI packs %s, %s: %s (bake them with: vd_baker bake --out packs --only ui, --only app)",
                    uiPack.string().c_str(), appPack.string().c_str(), opened.error().message.c_str());
        return false;
    }
    const std::span<const u8> atlas = m_assets.atlasBlob();
    const std::span<const u8> fonts = m_assets.fontAtlasBlob();
    u32 atlasPages = 0;
    if (atlas.size() >= sizeof(pack::TextureHeader)) {
        pack::TextureHeader header;
        std::memcpy(&header, atlas.data(), sizeof(header));
        atlasPages = header.layers;
    }
    if (Result<void> set = renderer.setUiTextures(atlas.data(), atlas.size(), fonts.data(), fonts.size()); !set) {
        VD_LOG_ERROR("UI textures: %s", set.error().message.c_str());
        m_assets.close();
        return false;
    }
    m_assets.releaseTextureBlobs(); // phase 22: the renderer has them
    Result<StagingBlock> staging = renderer.createStagingBlock(kStagingBytes);
    if (!staging) {
        VD_LOG_ERROR("UI staging: %s", staging.error().message.c_str());
        return false;
    }
    m_staging = staging.value();
    m_streamer.start(m_staging, "vd-ui");
    m_decodeAstc = !renderer.nativeAstc(); // phase 18 (world)
    m_textures = textures;
    m_ready = true;
    const std::string client = m_assets.hasClientArt() ? uiPack.filename().string() : std::string{};
    const std::string own = m_assets.hasOwnArt() ? appPack.filename().string() : std::string{};
    VD_LOG_INFO("UI: %s%s%s, %u atlas pages, %.1f ms", client.c_str(), !client.empty() && !own.empty() ? " + " : "",
                own.c_str(), atlasPages, secondsBetween(start, Clock::now()) * 1000.0);
    return true;
}

void UiResources::shutdown(Renderer& renderer) {
    m_streamer.stop();
    for (auto& [key, tile] : m_tiles) {
        if (tile.texture.valid()) {
            renderer.destroyTexture(tile.texture);
        }
    }
    m_tiles.clear();
    for (TextureSlot& slot : m_bigTextures) {
        if (slot.texture.valid()) {
            renderer.destroyTexture(slot.texture);
        }
        slot = {};
    }
    m_bigTextureSlots.clear();
    m_bigTextureCount = 0;
    m_iconSlots.clear();
    m_slots = {};
    if (m_staging.valid()) {
        renderer.destroyStagingBlock(m_staging);
    }
    m_assets.close();
    m_ready = false;
}

std::optional<u32> UiResources::icon(u64 texturePathHash) {
    if (!m_ready || !m_textures) {
        return std::nullopt;
    }
    if (const auto found = m_iconSlots.find(texturePathHash); found != m_iconSlots.end()) {
        if (found->second >= kUiIconSlots) {
            return std::nullopt; // not in the pack
        }
        IconSlot& slot = m_slots[found->second];
        slot.lastUse = m_frame;
        return slot.loaded ? std::optional<u32>(found->second) : std::nullopt;
    }
    const std::optional<pack::PackAsset> asset = m_textures->find(texturePathHash);
    if (!asset) {
        m_iconSlots.emplace(texturePathHash, kUiIconSlots);
        return std::nullopt;
    }
    // A free slot, else the one used longest ago (not this frame, not loading).
    u32 chosen = kUiIconSlots;
    for (u32 i = 0; i < kUiIconSlots; ++i) {
        const IconSlot& slot = m_slots[i];
        if (slot.hash == 0) {
            chosen = i;
            break;
        }
        if (!slot.loading && slot.lastUse < m_frame &&
            (chosen == kUiIconSlots || slot.lastUse < m_slots[chosen].lastUse)) {
            chosen = i;
        }
    }
    if (chosen == kUiIconSlots || m_slots[chosen].loading || !m_streamer.request(kIconTag | chosen, *asset)) {
        return std::nullopt; // retried next frame
    }
    IconSlot& slot = m_slots[chosen];
    if (slot.hash != 0) {
        m_iconSlots.erase(slot.hash);
    }
    slot = {texturePathHash, m_frame, false, true};
    m_iconSlots.emplace(texturePathHash, chosen);
    return std::nullopt;
}

std::optional<u32> UiResources::icon(std::string_view texturePath) {
    return icon(assetPathHash(texturePath));
}

TextureHandle UiResources::minimapTile(std::string_view map, u32 x, u32 y) {
    if (!m_ready || !m_textures) {
        return {};
    }
    if (m_minimapMap != map) {
        m_minimapMap = std::string(map);
        m_minimap = m_assets.minimap(map);
    }
    const u64 key = tileKey(assetPathHash(map), x, y);
    if (const auto found = m_tiles.find(key); found != m_tiles.end()) {
        found->second.lastUse = m_frame;
        return found->second.texture;
    }
    Tile& tile = m_tiles[key];
    tile.lastUse = m_frame;
    const auto entry = std::lower_bound(m_minimap.begin(), m_minimap.end(), (y << 8) | x,
                                        [](const pack::UiMinimapTile& t, u32 k) { return (u32{t.y} << 8 | t.x) < k; });
    std::optional<pack::PackAsset> asset;
    if (entry != m_minimap.end() && entry->x == x && entry->y == y) {
        asset = m_textures->find(entry->textureHash);
    }
    if (!asset) {
        tile.missing = true;
        return {};
    }
    if (m_streamer.request(kTileTag | key, *asset, m_decodeAstc)) {
        tile.loading = true;
        ++m_loadingTiles;
    } else {
        m_tiles.erase(key); // asked again next frame
    }
    return {};
}

TextureHandle UiResources::texture(u64 pathHash) {
    if (!m_ready || !m_textures || pathHash == 0) {
        return {};
    }
    if (const auto found = m_bigTextureSlots.find(pathHash); found != m_bigTextureSlots.end()) {
        TextureSlot& slot = m_bigTextures[found->second];
        slot.lastUse = m_frame;
        return slot.texture;
    }
    u32 free = kMaxTextures;
    for (u32 i = 0; i < kMaxTextures && free == kMaxTextures; ++i) {
        free = m_bigTextures[i].hash == 0 ? i : free;
    }
    if (free == kMaxTextures) {
        return {}; // full: textures go after kTextureKeepFrames unused
    }
    TextureSlot& slot = m_bigTextures[free];
    slot = {pathHash, {}, m_frame, false, false};
    m_bigTextureSlots.emplace(pathHash, free);
    ++m_bigTextureCount;
    const std::optional<pack::PackAsset> asset = m_textures ? m_textures->find(pathHash) : std::nullopt;
    if (!asset) {
        slot.missing = true;
    } else if (m_streamer.request(kTextureTag | free, *asset, m_decodeAstc)) { // phase 18 (world): desktop decode off-thread
        slot.loading = true;
    } else { // the queue is full: asked again next frame
        m_bigTextureSlots.erase(pathHash);
        slot = {};
        --m_bigTextureCount;
    }
    return {};
}

void UiResources::update(Renderer& renderer) {
    VD_PROFILE_ZONE("UiResources::update");
    if (!m_ready) {
        return;
    }
    ++m_frame;
    m_streamer.update(renderer.completedFrames());
    AssetStreamer::Done done;
    while (m_streamer.poll(done)) {
        u64 usedIn = 0;
        if (done.tag & kTileTag) {
            const u64 key = done.tag & ~kTileTag;
            --m_loadingTiles;
            if (const auto found = m_tiles.find(key); found != m_tiles.end()) {
                found->second.loading = false;
                if (done.ok) {
                    Result<TextureHandle> texture =
                        renderer.createTexture(m_staging, done.stagingOffset, done.data, done.size);
                    if (texture) {
                        found->second.texture = texture.value();
                        usedIn = renderer.frameNumber() + 1;
                    } else {
                        found->second.missing = true;
                    }
                }
            }
        } else if ((done.tag & kTextureTag) && (done.tag & 0xFFFF) < kMaxTextures) {
            TextureSlot& slot = m_bigTextures[done.tag & 0xFFFF];
            slot.loading = false;
            Result<TextureHandle> texture = done.ok ? renderer.createTexture(m_staging, done.stagingOffset, done.data,
                                                                             done.size)
                                                    : Result<TextureHandle>(Error{"read failed"});
            if (texture && slot.hash != 0) {
                slot.texture = texture.value();
                usedIn = renderer.frameNumber() + 1;
            } else {
                slot.missing = true;
            }
        } else if ((done.tag & kIconTag) && (done.tag & 0xFFFF) < kUiIconSlots) {
            const u32 index = static_cast<u32>(done.tag & 0xFFFF);
            IconSlot& slot = m_slots[index];
            slot.loading = false;
            if (done.ok && renderer.setUiIcon(index, m_staging, done.stagingOffset, done.data, done.size)) {
                slot.loaded = true;
                usedIn = renderer.frameNumber() + 1;
            } else {
                VD_LOG_WARN("UI icon %016llx: not a 64x64 icon", static_cast<unsigned long long>(slot.hash));
                m_iconSlots[slot.hash] = kUiIconSlots; // never asked again
                slot = {};
            }
        }
        m_streamer.release(done, usedIn);
    }
    // Textures nobody asked for during kTextureKeepFrames go (freed once the GPU is done with them).
    for (u32 i = 0; i < kMaxTextures; ++i) {
        TextureSlot& slot = m_bigTextures[i];
        if (slot.hash != 0 && !slot.loading && slot.lastUse + kTextureKeepFrames < m_frame) {
            if (slot.texture.valid()) {
                renderer.destroyTexture(slot.texture);
            }
            m_bigTextureSlots.erase(slot.hash);
            slot = {};
            --m_bigTextureCount;
        }
    }
    // Minimap tiles nobody drew for a while go (the texture is freed once the GPU is done with it).
    if (m_tiles.size() > kMaxTiles) {
        for (auto it = m_tiles.begin(); it != m_tiles.end() && m_tiles.size() > kMaxTiles;) {
            if (!it->second.loading && it->second.lastUse + 120 < m_frame) {
                if (it->second.texture.valid()) {
                    renderer.destroyTexture(it->second.texture);
                }
                it = m_tiles.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace vd
