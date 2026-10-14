#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"

namespace vd::pack {

// ui.vdpk (phase 7, `vd_baker bake --only ui`): what the touch UI draws with. See docs/decisions/ADR-0002-ui.md.
//
//   "ui/atlas"          Texture: 2D array of kUiAtlasSize pages (ASTC 4x4, one mip) holding the client's interface
//                       textures the UI uses, each whole, at a 4-aligned spot with a 4-pixel edge-extended gutter
//   "ui/sprites"        UiSpritesHeader + UiSprite[count] sorted by pathHash: where each of those textures went
//   "ui/fonts"          Texture: 2D array of kUiFontAtlasSize R8 layers: signed distance fields of the glyphs
//   "ui/font/<name>"    UiFontHeader + UiGlyph[glyphCount] sorted by codepoint + UiKerning[kerningCount] sorted by
//                       pair: one per client font (frizqt, arialn, morpheus, skurri)
//   "ui/minimap/<map>"  UiMinimapHeader + UiMinimapTile[count] sorted by (y << 8 | x): the minimap texture of each
//                       ADT tile (an asset of textures.vdpk, textures/minimap/<md5>.blp)
//   "ui/wmominimaps"    phase 15: UiWmoMinimapHeader + UiWmoMinimap[wmoCount] sorted by wmo + UiWmoMinimapTile[tileCount]:
//                       the indoor minimap of every building (textures\Minimap\md5translate.trs "WMO\..." lines)
//   "ui/strings/<loc>"  UiStringsHeader + UiString[count] sorted by keyHash + UTF-8 texts: the interface's texts in
//                       one language (enus, eses...): the client's GlobalStrings.lua and GlueStrings.lua and the rules
//                       derived from them (phase 22: ours, locale/<locale>/*.lang, are in app.vdpk, pack/AppPack.h; the
//                       pack's header says the base client's language; core/Text looks a key up in both)

constexpr u32 kUiAtlasSize = 2048;
constexpr u32 kUiAtlasGutter = 4; // pixels of edge extension around each sprite (one ASTC 4x4 block)
constexpr u32 kUiFontAtlasSize = 1024;

struct UiSpritesHeader {
    u32 count;
    u32 reserved;
};

// One client texture in the atlas: its pixels are at (x, y) of page `page`, width x height, as in the BLP.
struct UiSprite {
    u64 pathHash; // assetPathHash() of the texture's path in the client ("Interface\\Buttons\\UI-Quickslot2.blp")
    u16 page;
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 flags; // kTextureHasAlpha...
    u32 reserved;
};
static_assert(sizeof(UiSprite) == 24, "UiSprite layout");

// Font metrics at the size the glyphs were rasterized (pixelHeight): the runtime scales them to any size.
// Distances in the atlas: 0.5 (128) on the outline, +/- 0.5 at +/- sdfRange pixels inside / outside.
struct UiFontHeader {
    u32 glyphCount;
    u32 kerningCount;
    f32 pixelHeight; // ascent - descent at the rasterized size, in atlas pixels
    f32 ascent;      // above the baseline (positive)
    f32 descent;     // below the baseline (positive)
    f32 lineGap;
    f32 sdfRange;    // pixels of distance encoded on each side of the outline
    u32 fallback;    // codepoint drawn for characters the font lacks ('?')
};
static_assert(sizeof(UiFontHeader) == 32, "UiFontHeader layout");

struct UiGlyph {
    u32 codepoint;
    u16 layer; // of "ui/fonts"
    u16 x;     // cell in the atlas layer, sdfRange of padding included
    u16 y;
    u16 width;
    u16 height;
    i16 offsetX; // cell's top-left corner relative to the pen on the baseline (y down)
    i16 offsetY;
    u16 reserved;
    f32 advance; // pen advance
};
static_assert(sizeof(UiGlyph) == 24, "UiGlyph layout");

struct UiKerning {
    u64 pair; // (left codepoint << 32) | right codepoint
    f32 adjust;
    u32 reserved;
};
static_assert(sizeof(UiKerning) == 16, "UiKerning layout");

struct UiMinimapHeader {
    u32 count;
    u32 reserved;
};

struct UiMinimapTile {
    u8 x; // ADT tile (WDT column / row)
    u8 y;
    u16 reserved;
    u32 reserved2;
    u64 textureHash; // assetPathHash() of its texture in textures.vdpk
};
static_assert(sizeof(UiMinimapTile) == 16, "UiMinimapTile layout");

// Phase 15: the indoor minimaps. Each WMO group with a map has tiles "<wmo>_<group>_<x>_<y>.blp" drawn at 2 pixels a
// yard from the group's bounding box corner: tile (x, y) starts 128 yards * (x, y) past the box's minimum along the
// building's own X and Y, its texture's columns run along +X and its rows along -Y (the top row is the tile's high Y);
// the last tiles and small groups use textures of the next power of two, transparent past the box. The client shows,
// indoors, the tiles of the building the player is in whose box meets [p - r, p + r] across and [p.z - 1.5 r, p.z +
// 0.5 r] in height, r the minimap's radius (WoW.exe 0x6D95F8, the indoor zoom table at 0x8116E8).
constexpr const char* kUiWmoMinimapsAsset = "ui/wmominimaps";
constexpr f32 kWmoMinimapPixelsPerYard = 2.0f;
constexpr f32 kWmoMinimapTileYards = 128.0f;

struct UiWmoMinimapHeader {
    u32 wmoCount;
    u32 tileCount;
};

struct UiWmoMinimap {
    u64 wmo; // assetPathHash() of the WMO root ("World\wmo\...\Stormwind.wmo"), as models.vdpk keys it
    u32 firstTile;
    u32 tileCount;
};
static_assert(sizeof(UiWmoMinimap) == 16, "UiWmoMinimap layout");

struct UiWmoMinimapTile {
    u64 texture;      // assetPathHash() of its texture in textures.vdpk (textures/minimap/<md5>.blp)
    f32 boundsMin[3]; // the building's space: x, y where its texture's (u 0, v 1) corner is; z the group's floor
    f32 boundsMax[3]; // x, y of its (u 1, v 0) corner (the texture's whole size at 2 pixels a yard); z its ceiling
};
static_assert(sizeof(UiWmoMinimapTile) == 32, "UiWmoMinimapTile layout");

constexpr const char* kUiAtlasAsset = "ui/atlas";
constexpr const char* kUiSpritesAsset = "ui/sprites";
constexpr const char* kUiFontAtlasAsset = "ui/fonts";

// The interface's texts of one language. Each text is NUL-terminated UTF-8 at textOffset + offset (from the start of
// the blob), `length` bytes without the NUL; printf-like specifiers as the client's strings use them (core/Text).
struct UiStringsHeader {
    u32 count;
    u32 textOffset;
    u32 textSize;
    u32 reserved;
};
struct UiString {
    u64 keyHash; // stringHash() of the key ("ERR_INV_FULL", "VD_SELL_JUNK")
    u32 offset;
    u32 length;
};
static_assert(sizeof(UiString) == 16, "UiString layout");
constexpr const char* kUiStringsPrefix = "ui/strings/"; // + locale in lowercase ("ui/strings/enus")

// Phase 15 (options): the macro icons (Blizzard_MacroUI's GetNumMacroIcons / GetMacroIconInfo). WoW.exe (0x4F0090)
// lists every file of Interface\Icons in its archives, sorts the names without the extension case-insensitively and
// drops repeats; GetMacroIconInfo(i) is "Interface\Icons\" .. name. "ui/macroicons": UiMacroIconsHeader,
// UiMacroIcon[count] in that order, then the names (UTF-8, each NUL-terminated) at namesOffset from the blob's start.
struct UiMacroIconsHeader {
    u32 count;
    u32 namesOffset;
    u32 namesSize;
    u32 reserved;
};
struct UiMacroIcon {
    u64 textureHash; // assetPathHash() of Interface\Icons\<name>.blp in textures.vdpk
    u32 nameOffset;  // from namesOffset
    u32 nameLength;  // without the NUL
};
static_assert(sizeof(UiMacroIcon) == 16, "UiMacroIcon layout");
constexpr const char* kUiMacroIconsAsset = "ui/macroicons";

} // namespace vd::pack
