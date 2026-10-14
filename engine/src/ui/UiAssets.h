#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackReader.h"
#include "pack/UiFormat.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vd::ui {

enum class FontId : u8 {
    Friz,     // FRIZQT__: the interface's font
    Arial,    // ARIALN: chat, numbers
    Morpheus, // MORPHEUS: quest and book text
    Skurri,   // SKURRI: floating combat numbers
    App,      // phase 22 (apppack): DroidSans, ours (app.vdpk): the screens shown before there is any data
    Count,
};

// Where a client interface texture is in the atlas (pixels of its page).
struct Sprite {
    u16 page = 0;
    u16 x = 0;
    u16 y = 0;
    u16 width = 0;
    u16 height = 0;
};

// Glyph metrics of one baked font (pack/UiFormat.h), read in place from the mapped pack.
class Font {
public:
    bool valid() const { return m_glyphs != nullptr; }
    // The glyph of `codepoint`, or the font's fallback ('?') when it lacks it.
    const pack::UiGlyph& glyph(u32 codepoint) const;
    // Pen adjustment between two characters (atlas pixels at pixelHeight()).
    f32 kerning(u32 left, u32 right) const;
    f32 pixelHeight() const { return m_header.pixelHeight; }
    f32 ascent() const { return m_header.ascent; }
    f32 descent() const { return m_header.descent; }
    f32 lineGap() const { return m_header.lineGap; }
    f32 sdfRange() const { return m_header.sdfRange; }
    // Phase 22 (apppack): the layer of the font atlas its glyphs' `layer` counts from (app.vdpk's font comes after
    // ui.vdpk's layers).
    u16 layerBase() const { return m_layerBase; }

    bool load(const u8* data, u32 size, u16 layerBase = 0);

private:
    pack::UiFontHeader m_header{};
    u16 m_layerBase = 0;
    const pack::UiGlyph* m_glyphs = nullptr;
    const pack::UiKerning* m_kerning = nullptr;
    std::array<u16, 128> m_ascii{}; // glyph index + 1 of each ASCII character (0 = missing)
    u32 m_fallback = 0;             // index of the fallback glyph
};

// ui.vdpk, mapped: sprites, fonts and minimap tables. The atlas and font textures go to the renderer once.
// Phase 22 (apppack): and app.vdpk (pack/AppPack.h: our art and font), either of them alone: their sprites in one
// table (app.vdpk's atlas pages after ui.vdpk's), their font layers in one atlas; without ui.vdpk (a phone without
// data) the client's fonts are ours, without app.vdpk FontId::App is FRIZQT__.
class Assets {
public:
    // An empty path skips that pack; an error when neither gives an atlas and a font.
    Result<void> open(const std::filesystem::path& uiPack, const std::filesystem::path& appPack);
    // ui.vdpk and the app.vdpk next to it, if there (desktop, tests).
    Result<void> open(const std::filesystem::path& uiPack);
    void close();
    bool isOpen() const { return m_open; }
    // Phase 22 (apppack): ui.vdpk is open (the client's art, fonts, minimaps, macro icons, models).
    bool hasClientArt() const { return m_clientArt; }
    // Phase 22 (apppack): app.vdpk is open (our art and font).
    bool hasOwnArt() const { return m_ownArt; }

    std::optional<Sprite> sprite(u64 pathHash) const;
    std::optional<Sprite> sprite(std::string_view path) const;
    const Font& font(FontId id) const { return m_fonts[static_cast<usize>(id)]; }
    // Minimap textures of a map (directory name as in Map.dbc, any case), sorted by (y, x).
    std::span<const pack::UiMinimapTile> minimap(std::string_view map) const;
    // Phase 15: the indoor minimap tiles of a building (its WMO's assetPathHash), empty when it has none.
    std::span<const pack::UiWmoMinimapTile> wmoMinimap(u64 wmo) const;
    // Phase 15 (options): the macro icons in GetMacroIconInfo's order and their names ("ui/macroicons"); empty when
    // the pack predates them.
    struct MacroIcons {
        std::span<const pack::UiMacroIcon> icons;
        const char* names = nullptr;
        std::string_view name(u32 index) const {
            return index < icons.size() ? std::string_view(names + icons[index].nameOffset, icons[index].nameLength)
                                        : std::string_view{};
        }
    };
    MacroIcons macroIcons() const;

    // Texture blobs for Renderer::setUiTextures (the font atlas is decompressed into memory kept here; phase 22: so is
    // the atlas when app.vdpk adds pages to ui.vdpk's).
    std::span<const u8> atlasBlob() const { return m_atlas; }
    std::span<const u8> fontAtlasBlob() const { return m_fontAtlas; }
    // Phase 22 (apppack): frees the memory behind atlasBlob() and fontAtlasBlob() once the renderer has them.
    void releaseTextureBlobs();
    f32 atlasSize() const { return m_atlasSize; }
    f32 fontAtlasSize() const { return static_cast<f32>(pack::kUiFontAtlasSize); }
    // Retouches (ui models): an uncompressed asset of the pack as it is mapped (empty when missing).
    std::span<const u8> blob(std::string_view path) const;

private:
    pack::PackReader m_pack;    // ui.vdpk
    pack::PackReader m_appPack; // phase 22 (apppack): app.vdpk
    bool m_open = false;
    bool m_clientArt = false;
    bool m_ownArt = false;
    std::span<const u8> m_atlas;
    std::vector<u8> m_atlasMemory; // phase 22: the atlas when it is not ui.vdpk's mapped one as it is
    std::vector<u8> m_fontAtlas;
    f32 m_atlasSize = static_cast<f32>(pack::kUiAtlasSize);
    std::vector<pack::UiSprite> m_sprites; // both packs', sorted by pathHash
    std::array<Font, static_cast<usize>(FontId::Count)> m_fonts;
};

} // namespace vd::ui
