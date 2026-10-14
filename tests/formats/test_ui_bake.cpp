#include "bake/UiBaker.h"
#include "core/Hash.h"
#include "pack/UiFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>

using namespace vd;

namespace {

formats::BlpImage solid(u32 width, u32 height, u8 r, u8 g, u8 b) {
    formats::BlpImage image;
    image.width = width;
    image.height = height;
    image.mips.emplace_back(usize{width} * height * 4);
    for (usize i = 0; i < usize{width} * height; ++i) {
        image.mips[0][i * 4 + 0] = r;
        image.mips[0][i * 4 + 1] = g;
        image.mips[0][i * 4 + 2] = b;
        image.mips[0][i * 4 + 3] = 255;
    }
    // A distinct corner, to check the texture is not flipped.
    image.mips[0][0] = 7;
    return image;
}

const u8* pixel(const bake::UiAtlas& atlas, u32 page, u32 x, u32 y) {
    return atlas.pages[page].data() + (usize{y} * atlas.pageSize + x) * 4;
}

} // namespace

TEST_CASE("UI atlas: whole textures at 4-aligned spots with extended edges") {
    const formats::BlpImage red = solid(10, 6, 200, 0, 0);
    const formats::BlpImage green = solid(32, 32, 0, 200, 0);
    const formats::BlpImage blue = solid(5, 5, 0, 0, 200);
    const std::vector<bake::UiSpriteInput> inputs{{"Interface\\A.blp", &red}, {"Interface\\B.blp", &green},
                                                  {"Interface\\C.blp", &blue}};
    Result<bake::UiAtlas> packed = bake::packUiAtlas(inputs, 64);
    REQUIRE(packed.ok());
    const bake::UiAtlas& atlas = packed.value();
    REQUIRE(atlas.sprites.size() == 3);
    CHECK(std::is_sorted(atlas.sprites.begin(), atlas.sprites.end(),
                         [](const pack::UiSprite& a, const pack::UiSprite& b) { return a.pathHash < b.pathHash; }));
    for (const pack::UiSprite& sprite : atlas.sprites) {
        CHECK(sprite.x % 4 == 0);
        CHECK(sprite.y % 4 == 0);
        CHECK(sprite.x >= pack::kUiAtlasGutter);
        CHECK(sprite.y >= pack::kUiAtlasGutter);
    }
    auto find = [&](const char* path) {
        const u64 hash = assetPathHash(path);
        return *std::find_if(atlas.sprites.begin(), atlas.sprites.end(),
                             [&](const pack::UiSprite& s) { return s.pathHash == hash; });
    };
    const pack::UiSprite a = find("interface/a.blp"); // any spelling of the path
    CHECK(a.width == 10);
    CHECK(a.height == 6);
    CHECK(pixel(atlas, a.page, a.x, a.y)[0] == 7);             // the corner, not flipped
    CHECK(pixel(atlas, a.page, a.x + 9, a.y + 5)[0] == 200);   // last texel
    CHECK(pixel(atlas, a.page, a.x - 4, a.y - 4)[0] == 7);     // gutter: the corner extended
    CHECK(pixel(atlas, a.page, a.x + 13, a.y + 9)[0] == 200);  // gutter past the far corner
    CHECK(pixel(atlas, a.page, a.x + 11, a.y + 2)[0] == 200);  // 4-alignment padding
    const pack::UiSprite c = find("Interface\\C.blp");
    CHECK(pixel(atlas, c.page, c.x + 2, c.y + 2)[2] == 200);

    // Too big for a page.
    const formats::BlpImage huge = solid(70, 4, 1, 1, 1);
    const std::vector<bake::UiSpriteInput> tooBig{{"Interface\\Huge.blp", &huge}};
    CHECK_FALSE(bake::packUiAtlas(tooBig, 64).ok());
}

TEST_CASE("UI fonts: the client's fonts as distance fields with metrics and kerning") {
    VD_REQUIRE_MPQ(mpq);
    Result<std::vector<u8>> friz = mpq->read("Fonts\\FRIZQT__.TTF");
    REQUIRE(friz.ok());
    bake::UiFontInput input;
    input.name = "frizqt";
    input.ttf = friz.value();
    const std::vector<u32> charset = bake::uiCharacterSet();
    CHECK(charset.size() > 300);
    Result<bake::UiFontBake> baked = bake::bakeUiFonts({&input, 1}, charset);
    REQUIRE(baked.ok());
    REQUIRE(baked.value().fonts.size() == 1);
    CHECK(baked.value().fonts[0].first == "ui/font/frizqt");
    const std::vector<u8>& blob = baked.value().fonts[0].second;
    pack::UiFontHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.glyphCount > 200); // FRIZQT has Latin-1, not Latin Extended-A
    CHECK(header.ascent + header.descent == doctest::Approx(40.0f).epsilon(0.02));
    CHECK(header.fallback == '?');
    const auto* glyphs = reinterpret_cast<const pack::UiGlyph*>(blob.data() + sizeof(header));
    const pack::UiGlyph* end = glyphs + header.glyphCount;
    CHECK(std::is_sorted(glyphs, end, [](const pack::UiGlyph& a, const pack::UiGlyph& b) {
        return a.codepoint < b.codepoint;
    }));
    const pack::UiGlyph* big = std::lower_bound(glyphs, end, u32{'A'},
                                                [](const pack::UiGlyph& g, u32 c) { return g.codepoint < c; });
    REQUIRE(big != end);
    REQUIRE(big->codepoint == 'A');
    CHECK(big->advance > 10.0f);
    CHECK(big->width > 20);
    CHECK(big->offsetY < 0); // above the baseline
    // Its field: the middle of the cell is inside the letter's outline region or edge; the corner far outside.
    const std::vector<u8>& layer = baked.value().layers[big->layer];
    CHECK(layer[usize{big->y} * pack::kUiFontAtlasSize + big->x] < 40);
    const pack::UiGlyph* space = std::lower_bound(glyphs, end, u32{' '},
                                                  [](const pack::UiGlyph& g, u32 c) { return g.codepoint < c; });
    REQUIRE(space->codepoint == ' ');
    CHECK(space->width == 0);
    CHECK(space->advance > 0.0f);
    const pack::UiGlyph* enye = std::lower_bound(glyphs, end, u32{0xF1},
                                                 [](const pack::UiGlyph& g, u32 c) { return g.codepoint < c; });
    CHECK(enye->codepoint == 0xF1); // Spanish chat works
}
