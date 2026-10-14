// Phase 20: our own art (art/touch/*.svg) rasterized by the baker into app.vdpk's atlas (bake::rasterizeSvgIcon): a
// path's outlines filled white with the non-zero rule (a hole drawn the other way round stays empty), arcs, relative
// commands with numbers run together, and the three icons of the touch cluster themselves.
#include "bake/UiBaker.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace vd;

namespace {

u8 alphaAt(const formats::BlpImage& image, u32 x, u32 y) {
    return image.mips[0][(static_cast<usize>(y) * image.width + x) * 4 + 3];
}

} // namespace

TEST_CASE("svg icon: squares, a hole, an arc") {
    // A square from 2 to 8 in a 10-unit box, 20 pixels: covered between pixels 4 and 16.
    const char* square = R"(<svg viewBox="0 0 10 10"><path d="M2 2H8V8H2z"/></svg>)";
    Result<formats::BlpImage> a = bake::rasterizeSvgIcon(square, 20, 0.0f);
    REQUIRE(a);
    CHECK(alphaAt(a.value(), 10, 10) == 255);
    CHECK(alphaAt(a.value(), 2, 10) == 0);
    CHECK(alphaAt(a.value(), 10, 18) == 0);
    CHECK(a.value().mips[0][0] == 255); // white: the HUD tints it

    // The same square with a smaller one drawn the other way round inside (relative commands, numbers run together).
    const char* ring = R"(<svg viewBox="0 0 10 10"><path d="M1 1h8v8h-8zM3 3v4h4v-4z"/></svg>)";
    Result<formats::BlpImage> b = bake::rasterizeSvgIcon(ring, 20, 0.0f);
    REQUIRE(b);
    CHECK(alphaAt(b.value(), 10, 10) == 0);   // the hole
    CHECK(alphaAt(b.value(), 4, 10) == 255);  // the ring

    // Half a disc: an arc over the top from left to right.
    const char* arc = R"(<svg viewBox="0 0 10 10"><path d="M0 5a5 5 0 0 1 10 0z"/></svg>)";
    Result<formats::BlpImage> c = bake::rasterizeSvgIcon(arc, 40, 0.0f);
    REQUIRE(c);
    CHECK(alphaAt(c.value(), 20, 8) == 255);  // over the middle
    CHECK(alphaAt(c.value(), 20, 30) == 0);   // under it
    CHECK(alphaAt(c.value(), 2, 2) == 0);     // the corner outside the circle

    // What the baker does not draw is named, not drawn wrong.
    CHECK_FALSE(bake::rasterizeSvgIcon(R"(<svg viewBox="0 0 10 10"><circle r="3"/></svg>)", 16));
    const char* transformed = R"svg(<svg viewBox="0 0 10 10"><g transform="scale(2)"><path d="M0 0H5V5z"/></g></svg>)svg";
    CHECK_FALSE(bake::rasterizeSvgIcon(transformed, 16));
}

TEST_CASE("svg icon: the touch cluster's art") {
    for (const char* name : {"touch/jump.svg", "touch/mount.svg", "touch/target.svg"}) {
        std::ifstream in(std::filesystem::path(VD_ART_DIR) / name, std::ios::binary);
        const std::string svg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        INFO(name);
        REQUIRE_FALSE(svg.empty());
        Result<formats::BlpImage> icon = bake::rasterizeSvgIcon(svg, 128);
        REQUIRE(icon);
        // Something drawn (an outline icon such as the mount's covers little: 5% is enough), nothing in the margin
        // (6% of the size) along the border.
        u32 covered = 0, border = 0;
        for (u32 y = 0; y < 128; ++y) {
            for (u32 x = 0; x < 128; ++x) {
                const u8 alpha = alphaAt(icon.value(), x, y);
                covered += alpha > 128 ? 1u : 0u;
                border += (x < 6 || y < 6 || x >= 122 || y >= 122) && alpha > 0 ? 1u : 0u;
            }
        }
        CHECK(covered > 128u * 128u / 20u);
        CHECK(border == 0u);
    }
}
