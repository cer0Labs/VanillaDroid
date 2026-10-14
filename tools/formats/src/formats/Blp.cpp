#include "formats/Blp.h"

#include "formats/Dxt.h"

#include <algorithm>
#include <cstring>

namespace vd::formats {
namespace {

// BLP2 layout: magic, type, compression, alphaDepth, alphaType, hasMips, width, height,
// 16 mip offsets, 16 mip sizes, then a 256-entry BGRA palette.
constexpr usize kHeaderSize = 148;
constexpr usize kPaletteSize = 256 * 4;
constexpr u8 kCompressionPalette = 1;
constexpr u8 kCompressionDxt = 2;
constexpr u8 kCompressionArgb = 3;
constexpr u8 kAlphaTypeDxt5 = 7;

u32 readU32(const u8* p) {
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

usize paletteAlphaBytes(u8 alphaDepth, usize pixels) {
    switch (alphaDepth) {
    case 1: return (pixels + 7) / 8;
    case 4: return (pixels + 1) / 2;
    case 8: return pixels;
    default: return 0;
    }
}

// 2x2 box filter; odd or 1-texel dimensions clamp to the edge.
std::vector<u8> downsample(const std::vector<u8>& src, u32 w, u32 h) {
    const u32 dw = std::max(1u, w >> 1);
    const u32 dh = std::max(1u, h >> 1);
    std::vector<u8> dst(usize{dw} * dh * 4);
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = std::min(y * 2, h - 1);
        const u32 y1 = std::min(y * 2 + 1, h - 1);
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = std::min(x * 2, w - 1);
            const u32 x1 = std::min(x * 2 + 1, w - 1);
            for (u32 c = 0; c < 4; ++c) {
                const u32 sum = src[(usize{y0} * w + x0) * 4 + c] + src[(usize{y0} * w + x1) * 4 + c] +
                                src[(usize{y1} * w + x0) * 4 + c] + src[(usize{y1} * w + x1) * 4 + c];
                dst[(usize{y} * dw + x) * 4 + c] = static_cast<u8>((sum + 2) / 4);
            }
        }
    }
    return dst;
}

u8 paletteAlpha(u8 alphaDepth, const u8* alpha, usize i) {
    switch (alphaDepth) {
    case 1: return ((alpha[i / 8] >> (i % 8)) & 1) ? 255 : 0;
    case 4: return static_cast<u8>(((alpha[i / 2] >> ((i & 1) * 4)) & 0xF) * 17);
    case 8: return alpha[i];
    default: return 255;
    }
}

} // namespace

Result<BlpImage> decodeBlp(std::span<const u8> data) {
    if (data.size() < kHeaderSize + kPaletteSize || std::memcmp(data.data(), "BLP2", 4) != 0) {
        return Error{"Not a BLP2 file"};
    }
    const u8* p = data.data();
    if (readU32(p + 4) != 1) {
        return Error{"JPEG-compressed BLP is not supported"};
    }
    const u8 compression = p[8];
    const u8 alphaDepth = p[9];
    const u8 alphaType = p[10];
    const bool hasMips = p[11] != 0;

    BlpImage image;
    image.width = readU32(p + 12);
    image.height = readU32(p + 16);
    image.alphaDepth = alphaDepth;
    if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192) {
        return Error{"Invalid BLP dimensions"};
    }

    DxtFormat dxtFormat = DxtFormat::Dxt1;
    switch (compression) {
    case kCompressionPalette: image.encoding = BlpEncoding::Palette; break;
    case kCompressionArgb: image.encoding = BlpEncoding::Argb8888; break;
    case kCompressionDxt:
        if (alphaDepth <= 1) {
            dxtFormat = DxtFormat::Dxt1;
            image.encoding = BlpEncoding::Dxt1;
        } else if (alphaType == kAlphaTypeDxt5) {
            dxtFormat = DxtFormat::Dxt5;
            image.encoding = BlpEncoding::Dxt5;
        } else {
            dxtFormat = DxtFormat::Dxt3;
            image.encoding = BlpEncoding::Dxt3;
        }
        break;
    default: return Error{"Unknown BLP compression " + std::to_string(compression)};
    }

    const u8* palette = p + kHeaderSize;
    for (u32 level = 0; level < 16; ++level) {
        const u32 offset = readU32(p + 20 + level * 4);
        const u32 size = readU32(p + 84 + level * 4);
        if (offset == 0 || size == 0) {
            break;
        }
        if (usize{offset} + size > data.size()) {
            return Error{"BLP mip " + std::to_string(level) + " out of bounds"};
        }
        const u32 w = std::max(1u, image.width >> level);
        const u32 h = std::max(1u, image.height >> level);
        const usize pixels = usize{w} * h;
        const u8* src = p + offset;
        std::vector<u8> rgba(pixels * 4);

        if (compression == kCompressionPalette) {
            if (pixels + paletteAlphaBytes(alphaDepth, pixels) > size) {
                return Error{"BLP palette mip " + std::to_string(level) + " truncated"};
            }
            const u8* alpha = src + pixels;
            for (usize i = 0; i < pixels; ++i) {
                const u8* bgra = palette + src[i] * 4;
                rgba[i * 4 + 0] = bgra[2];
                rgba[i * 4 + 1] = bgra[1];
                rgba[i * 4 + 2] = bgra[0];
                rgba[i * 4 + 3] = paletteAlpha(alphaDepth, alpha, i);
            }
        } else if (compression == kCompressionDxt) {
            if (dxtImageSize(dxtFormat, w, h) > size) {
                // Blizzard's encoder stored a single block for mips narrower than 4 texels on tall
                // textures (e.g. 2x16 -> 8 bytes). Those tiny mips are regenerated below instead.
                if (level > 0) {
                    break;
                }
                return Error{"BLP DXT mip 0 truncated"};
            }
            decodeDxt(dxtFormat, src, w, h, rgba.data(), alphaDepth == 0);
        } else {
            if (pixels * 4 > size) {
                return Error{"BLP ARGB mip " + std::to_string(level) + " truncated"};
            }
            for (usize i = 0; i < pixels; ++i) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = alphaDepth == 0 ? 255 : src[i * 4 + 3];
            }
        }
        image.mips.push_back(std::move(rgba));

        if (!hasMips || (w == 1 && h == 1)) {
            break;
        }
    }
    if (image.mips.empty()) {
        return Error{"BLP has no image data"};
    }

    // Complete the chain down to 1x1 when the file had mips but some were unusable.
    if (hasMips) {
        u32 level = static_cast<u32>(image.mips.size()) - 1;
        u32 w = std::max(1u, image.width >> level);
        u32 h = std::max(1u, image.height >> level);
        while (w > 1 || h > 1) {
            image.mips.push_back(downsample(image.mips.back(), w, h));
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
        }
    }
    return image;
}

} // namespace vd::formats
