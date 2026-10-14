#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/Blp.h"
#include "pack/UiFormat.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::bake {

// --- Sprite atlas (phase 7) ---------------------------------------------------------------------------------------

struct UiSpriteInput {
    std::string path;            // client path of the texture ("Interface\\Buttons\\UI-Quickslot2.blp")
    const formats::BlpImage* image = nullptr; // mip 0 is used
};

struct UiAtlas {
    std::vector<std::vector<u8>> pages; // RGBA8, pageSize x pageSize each
    std::vector<pack::UiSprite> sprites; // sorted by pathHash
    u32 pageSize = 0;
    f32 fill = 0.0f; // share of the pages' pixels covered by sprites and their gutters
};

// Packs whole textures into pages (shelves, tallest first): each one at a 4-aligned spot with kUiAtlasGutter pixels
// of edge extension all around, so ASTC 4x4 blocks and bilinear filtering never mix two sprites.
Result<UiAtlas> packUiAtlas(std::span<const UiSpriteInput> inputs, u32 pageSize = pack::kUiAtlasSize);

// --- Our own art (phase 20) ----------------------------------------------------------------------------------------

// A single-colour SVG icon (art/: the touch cluster's jump, mount and target glyphs) drawn white, its coverage as the
// alpha, into a `size` x `size` image: its <path> elements' outlines (M L H V C S Q T A Z, absolute and relative)
// filled with the non-zero rule, the viewBox centered with `margin` (a share of the size) left empty around it.
// Transforms, strokes and other shapes are not supported (an error names them).
Result<formats::BlpImage> rasterizeSvgIcon(std::string_view svg, u32 size, f32 margin = 0.06f);

// --- Fonts ----------------------------------------------------------------------------------------------------------

// The characters the fonts are baked with: ASCII, Latin-1, Latin Extended-A and typographic punctuation.
std::vector<u32> uiCharacterSet();

struct UiFontInput {
    std::string name;        // asset: "ui/font/<name>"
    std::span<const u8> ttf; // the TrueType file
    f32 pixelHeight = 40.0f; // ascent - descent in atlas pixels
    f32 sdfRange = 6.0f;     // distance encoded on each side of the outline
};

struct UiFontBake {
    std::vector<std::vector<u8>> layers; // R8 atlas layers, pack::kUiFontAtlasSize square, shared by the fonts
    std::vector<std::pair<std::string, std::vector<u8>>> fonts; // asset name -> UiFontHeader + glyphs + kerning
};

// Signed distance fields of every character of `charset` each font has (stb_truetype), packed into shared layers.
Result<UiFontBake> bakeUiFonts(std::span<const UiFontInput> fonts, std::span<const u32> charset);

// A pack texture blob (TextureHeader + data) of R8 layers of the same square size, one mip.
std::vector<u8> buildR8Array(const std::vector<std::vector<u8>>& layers, u32 size);

// "ui/strings/<locale>": key -> text, sorted by stringHash(key) (pack::UiStringsHeader). Fails on two keys with the
// same hash.
struct UiText {
    std::string key;
    std::string text;
};
Result<std::vector<u8>> buildStringTable(std::span<const UiText> texts);

} // namespace vd::bake
