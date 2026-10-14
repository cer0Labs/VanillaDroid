#include "formats/Dxt.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace vd::formats {
namespace {

using Rgba = std::array<u8, 4>;

u16 readU16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 readU32(const u8* p) {
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

Rgba expand565(u16 c) {
    const u8 r = static_cast<u8>((c >> 11) & 31);
    const u8 g = static_cast<u8>((c >> 5) & 63);
    const u8 b = static_cast<u8>(c & 31);
    return {static_cast<u8>((r << 3) | (r >> 2)), static_cast<u8>((g << 2) | (g >> 4)),
            static_cast<u8>((b << 3) | (b >> 2)), 255};
}

u8 mix(u8 a, u8 b, int wa, int wb, int div) {
    return static_cast<u8>((a * wa + b * wb) / div);
}

// 8-byte color block. DXT3/DXT5 always use the 4-color mode.
void decodeColorBlock(const u8* block, bool allowPunchThrough, bool opaque, Rgba out[16]) {
    const u16 c0 = readU16(block);
    const u16 c1 = readU16(block + 2);
    std::array<Rgba, 4> palette{expand565(c0), expand565(c1), Rgba{}, Rgba{}};
    if (c0 > c1 || !allowPunchThrough) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = mix(palette[0][i], palette[1][i], 2, 1, 3);
            palette[3][i] = mix(palette[0][i], palette[1][i], 1, 2, 3);
        }
        palette[2][3] = 255;
        palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = mix(palette[0][i], palette[1][i], 1, 1, 2);
        }
        palette[2][3] = 255;
        palette[3] = {0, 0, 0, static_cast<u8>(opaque ? 255 : 0)};
    }
    const u32 indices = readU32(block + 4);
    for (int i = 0; i < 16; ++i) {
        out[i] = palette[(indices >> (2 * i)) & 3];
    }
}

void decodeDxt3Alpha(const u8* block, Rgba out[16]) {
    for (int i = 0; i < 16; ++i) {
        const u8 nibble = static_cast<u8>((block[i / 2] >> ((i & 1) * 4)) & 0xF);
        out[i][3] = static_cast<u8>(nibble * 17);
    }
}

void decodeDxt5Alpha(const u8* block, Rgba out[16]) {
    const u8 a0 = block[0];
    const u8 a1 = block[1];
    std::array<u8, 8> alphas{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) {
            alphas[i + 1] = static_cast<u8>(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            alphas[i + 1] = static_cast<u8>(((5 - i) * a0 + i * a1) / 5);
        }
        alphas[6] = 0;
        alphas[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= u64{block[2 + i]} << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        out[i][3] = alphas[(bits >> (3 * i)) & 7];
    }
}

usize blockBytes(DxtFormat format) {
    return format == DxtFormat::Dxt1 ? 8 : 16;
}

} // namespace

usize dxtImageSize(DxtFormat format, u32 width, u32 height) {
    const usize blocksX = (std::max(width, 1u) + 3) / 4;
    const usize blocksY = (std::max(height, 1u) + 3) / 4;
    return blocksX * blocksY * blockBytes(format);
}

void decodeDxt(DxtFormat format, const u8* blocks, u32 width, u32 height, u8* rgbaOut, bool opaqueDxt1) {
    const u32 blocksX = (width + 3) / 4;
    const u32 blocksY = (height + 3) / 4;
    const usize stride = blockBytes(format);
    Rgba texels[16];
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            const u8* block = blocks + (usize{by} * blocksX + bx) * stride;
            switch (format) {
            case DxtFormat::Dxt1:
                decodeColorBlock(block, true, opaqueDxt1, texels);
                break;
            case DxtFormat::Dxt3:
                decodeColorBlock(block + 8, false, true, texels);
                decodeDxt3Alpha(block, texels);
                break;
            case DxtFormat::Dxt5:
                decodeColorBlock(block + 8, false, true, texels);
                decodeDxt5Alpha(block, texels);
                break;
            }
            // Copy the 4x4 block, clipping at the image edge (mips smaller than 4 texels).
            for (u32 y = 0; y < 4 && by * 4 + y < height; ++y) {
                for (u32 x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    u8* dst = rgbaOut + ((usize{by} * 4 + y) * width + bx * 4 + x) * 4;
                    std::memcpy(dst, texels[y * 4 + x].data(), 4);
                }
            }
        }
    }
}

} // namespace vd::formats
