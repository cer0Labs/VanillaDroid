#pragma once

#include "core/Types.h"
#include "pack/PackReader.h"
#include "render/Renderer.h"
#include "ui/UiAssets.h"
#include "world/AssetStreamer.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace vd {

// What the touch UI draws with (phase 7): ui.vdpk (atlas, fonts, minimap tables; phase 22: and app.vdpk, our art and
// font, either alone) and, streamed on demand from
// textures.vdpk by the "vd-ui" IO thread, icons (into the renderer's icon cache, least recently used slot first) and
// minimap tiles (textures). Game thread only.
class UiResources {
public:
    ~UiResources();

    bool init(Renderer& renderer, const pack::PackReader* textures, const std::filesystem::path& uiPack,
              const std::filesystem::path& appPack);
    void shutdown(Renderer& renderer);
    bool ready() const { return m_ready; }

    const ui::Assets& assets() const { return m_assets; }

    // Cache slot of an icon (its path in textures.vdpk, e.g. "Interface\\Icons\\INV_Misc_QuestionMark.blp"); asked
    // for when missing, nullopt until it has arrived. Using a slot keeps it from being replaced this frame.
    std::optional<u32> icon(u64 texturePathHash);
    std::optional<u32> icon(std::string_view texturePath);
    // Texture of a minimap tile of `map` (its directory, as in Map.dbc) or nothing (no tile there, or still loading).
    TextureHandle minimapTile(std::string_view map, u32 x, u32 y);
    // Phase 8: any texture of textures.vdpk bigger than an icon (world map tiles and overlays, the taxi map, loading
    // screens), streamed on demand into a texture of its own; invalid until it has arrived (or when the pack lacks
    // it). A texture not asked for during kTextureKeepFrames is freed (a closed window lets its art go within a
    // second or two); at most kMaxTextures live at once.
    TextureHandle texture(u64 pathHash);
    static constexpr u32 kMaxTextures = 96;
    static constexpr u64 kTextureKeepFrames = 90;
    u32 textureCount() const { return m_bigTextureCount; } // alive or loading

    // Once per frame, before the UI is drawn: finished loads go to the GPU.
    void update(Renderer& renderer);

private:
    struct IconSlot {
        u64 hash = 0;
        u64 lastUse = 0;
        bool loaded = false;
        bool loading = false;
    };
    struct Tile {
        TextureHandle texture;
        u64 lastUse = 0;
        bool loading = false;
        bool missing = false; // no such texture: never asked again
    };
    static constexpr u32 kMaxTiles = 24;
    static constexpr u64 kTileTag = 1ull << 63;
    static constexpr u64 kTextureTag = 1ull << 61; // | texture slot
    struct TextureSlot {
        u64 hash = 0; // 0: free
        TextureHandle texture;
        u64 lastUse = 0;
        bool loading = false;
        bool missing = false; // not in the pack (or not decodable): never asked again while held
    };

    ui::Assets m_assets;
    const pack::PackReader* m_textures = nullptr;
    AssetStreamer m_streamer;
    bool m_decodeAstc = false; // phase 18 (world): big textures decoded on vd-ui (a GPU without ASTC: desktop)
    StagingBlock m_staging;
    bool m_ready = false;
    u64 m_frame = 0;
    std::array<IconSlot, kUiIconSlots> m_slots{};
    std::unordered_map<u64, u32> m_iconSlots; // texture hash -> slot
    std::unordered_map<u64, Tile> m_tiles;    // (map hash ^ tile) -> texture
    std::string m_minimapMap;                 // the map the tile table below belongs to
    std::span<const pack::UiMinimapTile> m_minimap;
    u32 m_loadingTiles = 0;
    std::array<TextureSlot, kMaxTextures> m_bigTextures{};
    std::unordered_map<u64, u32> m_bigTextureSlots; // path hash -> m_bigTextures
    u32 m_bigTextureCount = 0;
};

} // namespace vd
