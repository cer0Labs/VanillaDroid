#include "ui/UiAssets.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/AppPack.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace vd::ui {
namespace {

constexpr const char* kFontNames[] = {"ui/font/frizqt", "ui/font/arialn", "ui/font/morpheus", "ui/font/skurri",
                                      pack::kAppFontAsset}; // phase 22 (apppack): FontId::App, from app.vdpk
static_assert(std::size(kFontNames) == static_cast<usize>(FontId::Count));

// Phase 22 (apppack): what one pack gives the UI (its atlas pages, sprites and font layers).
struct PackArt {
    std::span<const u8> atlas;
    std::vector<u8> atlasMemory; // the atlas, decompressed (app.vdpk stores it compressed)
    const pack::UiSprite* sprites = nullptr;
    u32 spriteCount = 0;
    std::vector<u8> fonts; // the font atlas, decompressed
    u16 atlasLayers = 0;
    u16 fontLayers = 0;
};

bool arrayHeader(std::span<const u8> blob, pack::TextureHeader& header) {
    if (blob.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.layers == 0 || header.mipCount == 0 || header.mipCount > pack::kMaxMips) {
        return false;
    }
    for (u32 level = 0; level < header.mipCount; ++level) {
        if (u64{header.mipOffsets[level]} + header.mipSizes[level] > blob.size()) {
            return false;
        }
    }
    return true;
}

// A pack's atlas, sprites and font atlas (an error when it lacks one of them).
Result<void> readPackArt(const pack::PackReader& reader, PackArt& art) {
    const std::optional<pack::PackAsset> atlas = reader.find(pack::kUiAtlasAsset);
    const std::optional<pack::PackAsset> sprites = reader.find(pack::kUiSpritesAsset);
    const std::optional<pack::PackAsset> fontAtlas = reader.find(pack::kUiFontAtlasAsset);
    if (!atlas || !sprites || !fontAtlas || sprites->compression != pack::Compression::None ||
        sprites->size < sizeof(pack::UiSpritesHeader)) {
        return Error{"atlas, sprites or fonts missing"};
    }
    pack::UiSpritesHeader header;
    std::memcpy(&header, sprites->data, sizeof(header));
    if (sizeof(header) + usize{header.count} * sizeof(pack::UiSprite) > sprites->size) {
        return Error{"sprite table truncated"};
    }
    art.sprites = reinterpret_cast<const pack::UiSprite*>(sprites->data + sizeof(header));
    art.spriteCount = header.count;
    if (atlas->compression == pack::Compression::None) {
        art.atlas = {atlas->data, atlas->size};
    } else {
        art.atlasMemory.resize(atlas->rawSize);
        VD_TRY(pack::PackReader::read(*atlas, art.atlasMemory.data(), art.atlasMemory.size()));
        art.atlas = art.atlasMemory;
    }
    art.fonts.resize(fontAtlas->rawSize);
    VD_TRY(pack::PackReader::read(*fontAtlas, art.fonts.data(), art.fonts.size()));
    pack::TextureHeader texture;
    art.atlasLayers = arrayHeader(art.atlas, texture) ? texture.layers : 0;
    art.fontLayers = arrayHeader(art.fonts, texture) ? texture.layers : 0;
    return {};
}

// Phase 22 (apppack): the layers of `a` then those of `b` (same format, size and mips) as one texture array blob: each
// mip holds every layer back to back (pack::TextureHeader).
bool appendLayers(std::span<const u8> a, std::span<const u8> b, std::vector<u8>& out) {
    pack::TextureHeader ha, hb;
    if (!arrayHeader(a, ha) || !arrayHeader(b, hb) || ha.format != hb.format || ha.width != hb.width ||
        ha.height != hb.height || ha.mipCount != hb.mipCount || u32{ha.layers} + hb.layers > 0xFFFFu) {
        return false;
    }
    pack::TextureHeader header = ha;
    header.layers = static_cast<u16>(ha.layers + hb.layers);
    usize offset = (sizeof(header) + 15) & ~usize{15};
    for (u32 level = 0; level < ha.mipCount; ++level) {
        header.mipOffsets[level] = static_cast<u32>(offset);
        header.mipSizes[level] = ha.mipSizes[level] + hb.mipSizes[level];
        offset = (offset + header.mipSizes[level] + 15) & ~usize{15};
    }
    out.assign(offset, u8{0});
    std::memcpy(out.data(), &header, sizeof(header));
    for (u32 level = 0; level < ha.mipCount; ++level) {
        std::memcpy(out.data() + header.mipOffsets[level], a.data() + ha.mipOffsets[level], ha.mipSizes[level]);
        std::memcpy(out.data() + header.mipOffsets[level] + ha.mipSizes[level], b.data() + hb.mipOffsets[level],
                    hb.mipSizes[level]);
    }
    return true;
}

} // namespace

bool Font::load(const u8* data, u32 size, u16 layerBase) {
    if (size < sizeof(pack::UiFontHeader)) {
        return false;
    }
    std::memcpy(&m_header, data, sizeof(m_header));
    const usize needed = sizeof(pack::UiFontHeader) + usize{m_header.glyphCount} * sizeof(pack::UiGlyph) +
                         usize{m_header.kerningCount} * sizeof(pack::UiKerning);
    if (m_header.glyphCount == 0 || needed > size || m_header.pixelHeight <= 0.0f) {
        return false;
    }
    m_glyphs = reinterpret_cast<const pack::UiGlyph*>(data + sizeof(pack::UiFontHeader));
    m_kerning = reinterpret_cast<const pack::UiKerning*>(data + sizeof(pack::UiFontHeader) +
                                                          usize{m_header.glyphCount} * sizeof(pack::UiGlyph));
    m_layerBase = layerBase;
    m_ascii.fill(0);
    m_fallback = 0;
    for (u32 i = 0; i < m_header.glyphCount; ++i) {
        const u32 c = m_glyphs[i].codepoint;
        if (c < m_ascii.size()) {
            m_ascii[c] = static_cast<u16>(i + 1);
        }
        if (c == m_header.fallback) {
            m_fallback = i;
        }
    }
    return true;
}

const pack::UiGlyph& Font::glyph(u32 codepoint) const {
    if (codepoint < m_ascii.size()) {
        const u16 index = m_ascii[codepoint];
        return m_glyphs[index ? index - 1 : m_fallback];
    }
    const pack::UiGlyph* end = m_glyphs + m_header.glyphCount;
    const pack::UiGlyph* found = std::lower_bound(
        m_glyphs, end, codepoint, [](const pack::UiGlyph& g, u32 c) { return g.codepoint < c; });
    return found != end && found->codepoint == codepoint ? *found : m_glyphs[m_fallback];
}

f32 Font::kerning(u32 left, u32 right) const {
    if (m_header.kerningCount == 0) {
        return 0.0f;
    }
    const u64 pair = (u64{left} << 32) | right;
    const pack::UiKerning* end = m_kerning + m_header.kerningCount;
    const pack::UiKerning* found =
        std::lower_bound(m_kerning, end, pair, [](const pack::UiKerning& k, u64 p) { return k.pair < p; });
    return found != end && found->pair == pair ? found->adjust : 0.0f;
}

Result<void> Assets::open(const std::filesystem::path& uiPack, const std::filesystem::path& appPack) {
    close();
    // Phase 22 (apppack): ui.vdpk (the client's art) and app.vdpk (ours), each optional.
    PackArt client, app;
    if (!uiPack.empty()) {
        Result<void> read = m_pack.open(uiPack);
        if (read) {
            read = readPackArt(m_pack, client);
        }
        if (!read) {
            VD_LOG_WARN("UI: %s: %s", uiPack.string().c_str(), read.error().message.c_str());
            m_pack.close();
        }
        m_clientArt = read.ok();
    }
    bool appArt = false;
    if (!appPack.empty()) {
        Result<void> read = m_appPack.open(appPack);
        if (read) {
            read = readPackArt(m_appPack, app);
        }
        if (!read) {
            VD_LOG_WARN("UI: %s: %s (our art and font: vd_baker bake --out packs --only app)",
                        appPack.string().c_str(), read.error().message.c_str());
            m_appPack.close();
        }
        appArt = read.ok();
    }
    if (!m_clientArt && !appArt) {
        close();
        return Error{"ui.vdpk / app.vdpk: atlas, sprites or fonts missing"};
    }

    // The atlas: ui.vdpk's pages, then app.vdpk's (the sprites of each say their page in it).
    bool appSprites = appArt;
    u16 appPageBase = 0;
    if (m_clientArt && appArt) {
        appSprites = appendLayers(client.atlas, app.atlas, m_atlasMemory);
        if (appSprites) {
            m_atlas = m_atlasMemory;
            appPageBase = client.atlasLayers;
        } else {
            VD_LOG_WARN("UI: app.vdpk's atlas does not match ui.vdpk's: our art is not drawn");
        }
    }
    if (!m_clientArt || !appSprites) {
        // One pack's atlas: in place, or its decompressed memory (moved: the bytes stay where they are).
        PackArt& only = m_clientArt ? client : app;
        m_atlas = only.atlas;
        m_atlasMemory = std::move(only.atlasMemory);
    }
    if (m_atlas.size() >= sizeof(pack::TextureHeader)) {
        pack::TextureHeader texture;
        std::memcpy(&texture, m_atlas.data(), sizeof(texture));
        m_atlasSize = static_cast<f32>(texture.width);
    }
    if (m_clientArt) {
        m_sprites.assign(client.sprites, client.sprites + client.spriteCount);
    }
    if (appSprites) {
        for (u32 i = 0; i < app.spriteCount; ++i) {
            pack::UiSprite sprite = app.sprites[i];
            sprite.page = static_cast<u16>(sprite.page + appPageBase);
            m_sprites.push_back(sprite);
        }
    }
    std::stable_sort(m_sprites.begin(), m_sprites.end(),
                     [](const pack::UiSprite& a, const pack::UiSprite& b) { return a.pathHash < b.pathHash; });

    // The fonts' atlas: ui.vdpk's layers, then app.vdpk's.
    bool appFonts = appArt;
    u16 appLayerBase = 0;
    if (m_clientArt && appArt) {
        appFonts = appendLayers(client.fonts, app.fonts, m_fontAtlas);
        appLayerBase = client.fontLayers;
        if (!appFonts) {
            VD_LOG_WARN("UI: app.vdpk's font atlas does not match ui.vdpk's: our font is not drawn");
            m_fontAtlas = std::move(client.fonts);
        }
    } else {
        m_fontAtlas = std::move(m_clientArt ? client.fonts : app.fonts);
    }
    for (usize i = 0; i < m_fonts.size(); ++i) {
        const bool own = i == static_cast<usize>(FontId::App);
        if (own ? !appFonts : !m_clientArt) {
            continue;
        }
        const pack::PackReader& source = own ? m_appPack : m_pack;
        const std::optional<pack::PackAsset> font = source.find(kFontNames[i]);
        if (!font || font->compression != pack::Compression::None ||
            !m_fonts[i].load(font->data, font->size, own ? appLayerBase : 0)) {
            VD_LOG_WARN("%s: font %s missing or invalid", own ? "app.vdpk" : "ui.vdpk", kFontNames[i]);
            m_fonts[i] = {};
        }
    }
    // Missing fonts: the client's become FRIZQT__ (ours without ui.vdpk), ours FRIZQT__.
    Font& friz = m_fonts[static_cast<usize>(FontId::Friz)];
    if (!friz.valid()) {
        friz = m_fonts[static_cast<usize>(FontId::App)];
    }
    for (Font& font : m_fonts) {
        if (!font.valid()) {
            font = friz;
        }
    }
    if (!friz.valid()) {
        close();
        return Error{"ui.vdpk / app.vdpk: no interface font"};
    }
    m_ownArt = appSprites || appFonts;
    m_open = true;
    return {};
}

Result<void> Assets::open(const std::filesystem::path& uiPack) {
    std::filesystem::path appPack = uiPack.parent_path() / pack::kAppPackFile;
    std::error_code error;
    if (!std::filesystem::exists(appPack, error)) {
        appPack.clear();
    }
    return open(uiPack, appPack);
}

void Assets::close() {
    m_pack.close();
    m_appPack.close();
    m_open = false;
    m_clientArt = false;
    m_ownArt = false;
    releaseTextureBlobs();
    m_sprites.clear();
    m_fonts = {};
}

void Assets::releaseTextureBlobs() {
    m_atlas = {};
    m_atlasMemory.clear();
    m_atlasMemory.shrink_to_fit();
    m_fontAtlas.clear();
    m_fontAtlas.shrink_to_fit();
}

std::optional<Sprite> Assets::sprite(u64 pathHash) const {
    const auto found = std::lower_bound(m_sprites.begin(), m_sprites.end(), pathHash,
                                        [](const pack::UiSprite& s, u64 h) { return s.pathHash < h; });
    if (found == m_sprites.end() || found->pathHash != pathHash) {
        return std::nullopt;
    }
    return Sprite{found->page, found->x, found->y, found->width, found->height};
}

std::optional<Sprite> Assets::sprite(std::string_view path) const {
    return sprite(assetPathHash(path));
}

std::span<const pack::UiMinimapTile> Assets::minimap(std::string_view map) const {
    std::string name = "ui/minimap/";
    name += normalizeAssetPath(map);
    const std::optional<pack::PackAsset> table = m_pack.find(name);
    if (!table || table->compression != pack::Compression::None || table->size < sizeof(pack::UiMinimapHeader)) {
        return {};
    }
    pack::UiMinimapHeader header;
    std::memcpy(&header, table->data, sizeof(header));
    if (sizeof(header) + usize{header.count} * sizeof(pack::UiMinimapTile) > table->size) {
        return {};
    }
    return {reinterpret_cast<const pack::UiMinimapTile*>(table->data + sizeof(header)), header.count};
}

std::span<const pack::UiWmoMinimapTile> Assets::wmoMinimap(u64 wmo) const {
    const std::optional<pack::PackAsset> table = m_pack.find(pack::kUiWmoMinimapsAsset);
    if (!table || table->compression != pack::Compression::None || table->size < sizeof(pack::UiWmoMinimapHeader)) {
        return {};
    }
    pack::UiWmoMinimapHeader header;
    std::memcpy(&header, table->data, sizeof(header));
    const usize wmosEnd = sizeof(header) + usize{header.wmoCount} * sizeof(pack::UiWmoMinimap);
    if (wmosEnd + usize{header.tileCount} * sizeof(pack::UiWmoMinimapTile) > table->size) {
        return {};
    }
    const auto* wmos = reinterpret_cast<const pack::UiWmoMinimap*>(table->data + sizeof(header));
    const pack::UiWmoMinimap* end = wmos + header.wmoCount;
    const pack::UiWmoMinimap* found =
        std::lower_bound(wmos, end, wmo, [](const pack::UiWmoMinimap& w, u64 key) { return w.wmo < key; });
    if (found == end || found->wmo != wmo || u64{found->firstTile} + found->tileCount > header.tileCount) {
        return {};
    }
    const auto* tiles = reinterpret_cast<const pack::UiWmoMinimapTile*>(table->data + wmosEnd);
    return {tiles + found->firstTile, found->tileCount};
}

Assets::MacroIcons Assets::macroIcons() const {
    const std::optional<pack::PackAsset> blob = m_pack.find(pack::kUiMacroIconsAsset);
    if (!blob || blob->compression != pack::Compression::None || blob->size < sizeof(pack::UiMacroIconsHeader)) {
        return {};
    }
    pack::UiMacroIconsHeader header;
    std::memcpy(&header, blob->data, sizeof(header));
    if (sizeof(header) + usize{header.count} * sizeof(pack::UiMacroIcon) > header.namesOffset ||
        usize{header.namesOffset} + header.namesSize > blob->size) {
        return {};
    }
    MacroIcons out;
    out.icons = {reinterpret_cast<const pack::UiMacroIcon*>(blob->data + sizeof(header)), header.count};
    out.names = reinterpret_cast<const char*>(blob->data + header.namesOffset);
    for (const pack::UiMacroIcon& icon : out.icons) {
        if (usize{icon.nameOffset} + icon.nameLength >= header.namesSize) {
            return {}; // a name past the table (or without its NUL)
        }
    }
    return out;
}

std::span<const u8> Assets::blob(std::string_view path) const { // retouches (ui models)
    std::optional<pack::PackAsset> asset = m_open ? m_pack.find(path) : std::nullopt;
    if (!asset && m_open) {
        asset = m_appPack.find(path); // phase 22 (apppack)
    }
    if (!asset || asset->compression != pack::Compression::None) {
        return {};
    }
    return {asset->data, static_cast<usize>(asset->size)};
}

} // namespace vd::ui
