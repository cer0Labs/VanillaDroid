#include "formats/Dxt.h"

#include <doctest/doctest.h>

#include <array>

using vd::u8;

TEST_CASE("DXT1 decodes the 4-color palette") {
    // c0 = pure red (0xF800), c1 = pure blue (0x001F); c0 > c1 -> 4-color mode.
    // Indices, one per texel row-major: row 0 = 0,1,2,3, other rows = 0.
    const std::array<u8, 8> block{0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0x00, 0x00, 0x00};
    std::array<u8, 4 * 4 * 4> rgba{};
    vd::formats::decodeDxt(vd::formats::DxtFormat::Dxt1, block.data(), 4, 4, rgba.data(), true);

    auto texel = [&](int x, int y) { return &rgba[(y * 4 + x) * 4]; };
    CHECK(texel(0, 0)[0] == 255); // red
    CHECK(texel(0, 0)[2] == 0);
    CHECK(texel(1, 0)[0] == 0); // blue
    CHECK(texel(1, 0)[2] == 255);
    CHECK(texel(2, 0)[0] == 170); // 2/3 red + 1/3 blue
    CHECK(texel(2, 0)[2] == 85);
    CHECK(texel(3, 0)[0] == 85); // 1/3 red + 2/3 blue
    CHECK(texel(3, 0)[2] == 170);
    CHECK(texel(3, 3)[3] == 255);
}

TEST_CASE("DXT1 punch-through alpha is transparent unless forced opaque") {
    // c0 < c1 -> 3-color mode; index 3 = transparent black.
    const std::array<u8, 8> block{0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
    std::array<u8, 64> rgba{};
    vd::formats::decodeDxt(vd::formats::DxtFormat::Dxt1, block.data(), 4, 4, rgba.data(), false);
    CHECK(rgba[3] == 0);
    vd::formats::decodeDxt(vd::formats::DxtFormat::Dxt1, block.data(), 4, 4, rgba.data(), true);
    CHECK(rgba[3] == 255);
}

TEST_CASE("DXT5 interpolates alpha") {
    // alpha0 = 255, alpha1 = 0 (8-value mode); first texel index 0 -> 255, second index 1 -> 0,
    // third index 2 -> (6*255 + 0) / 7 = 218.
    std::array<u8, 16> block{};
    block[0] = 255;
    block[1] = 0;
    block[2] = 0b10001000; // texel0 = 0, texel1 = 1, texel2 low bits = 2
    block[3] = 0b00000000;
    std::array<u8, 64> rgba{};
    vd::formats::decodeDxt(vd::formats::DxtFormat::Dxt5, block.data(), 4, 4, rgba.data(), true);
    CHECK(rgba[0 * 4 + 3] == 255);
    CHECK(rgba[1 * 4 + 3] == 0);
    CHECK(rgba[2 * 4 + 3] == 218);
}

TEST_CASE("DXT decoding clips blocks at small mip sizes") {
    const std::array<u8, 8> block{0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}; // all red
    std::array<u8, 2 * 1 * 4> rgba{};
    vd::formats::decodeDxt(vd::formats::DxtFormat::Dxt1, block.data(), 2, 1, rgba.data(), true);
    CHECK(rgba[0] == 255);
    CHECK(rgba[4] == 255);
    CHECK(vd::formats::dxtImageSize(vd::formats::DxtFormat::Dxt1, 2, 1) == 8);
    CHECK(vd::formats::dxtImageSize(vd::formats::DxtFormat::Dxt5, 8, 8) == 64);
}
