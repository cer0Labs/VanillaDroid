#include "bake/TextureBaker.h"

#include <astcenc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vd::bake {
namespace {

using pack::TextureFormat;

const float kAstcQuality = ASTCENC_PRE_MEDIUM; // a static const float: not constexpr for clang

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

int blockSlot(u32 block) {
    switch (block) {
    case 4: return 0;
    case 5: return 1;
    case 6: return 2;
    default: return 3; // 8
    }
}

// One encoder context per (thread, block size, alpha mode): contexts are expensive to create and
// can only process one image at a time. Freed when the thread exits.
astcenc_context* encoderFor(u32 block, bool alpha) {
    struct Cache {
        std::array<std::array<astcenc_context*, 2>, 4> contexts{};
        ~Cache() {
            for (auto& row : contexts) {
                for (astcenc_context* context : row) {
                    if (context) {
                        astcenc_context_free(context);
                    }
                }
            }
        }
    };
    thread_local Cache cache;
    astcenc_context*& context = cache.contexts[blockSlot(block)][alpha ? 1 : 0];
    if (!context) {
        astcenc_config config{};
        // No ASTCENC_FLG_USE_ALPHA_WEIGHT: it scales the color error by alpha, and many textures keep a mask in
        // alpha (specular, glow, terrain height) while the color stays visible where it is 0. With it, those texels
        // came out black in 6x6 blocks (Orgrimmar's floors: 13,200 black texels of 65,536, none in the source).
        const unsigned flags = 0;
        if (astcenc_config_init(ASTCENC_PRF_LDR, block, block, 1, kAstcQuality, flags, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) {
            context = nullptr;
        }
    }
    return context;
}

astcenc_context* decoderFor(u32 block) {
    struct Cache {
        std::array<astcenc_context*, 4> contexts{};
        ~Cache() {
            for (astcenc_context* context : contexts) {
                if (context) {
                    astcenc_context_free(context);
                }
            }
        }
    };
    thread_local Cache cache;
    astcenc_context*& context = cache.contexts[blockSlot(block)];
    if (!context) {
        astcenc_config config{};
        if (astcenc_config_init(ASTCENC_PRF_LDR, block, block, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY,
                                &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) {
            context = nullptr;
        }
    }
    return context;
}

bool alphaIsUsed(const formats::BlpImage& image) {
    if (image.alphaDepth == 0) {
        return false;
    }
    const std::vector<u8>& texels = image.mips[0];
    for (usize i = 3; i < texels.size(); i += 4) {
        if (texels[i] != 255) {
            return true;
        }
    }
    return false; // declared alpha but every texel is opaque
}

usize alignUp(usize value, usize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

TextureFormat chooseTextureFormat(std::string_view path) {
    if (startsWith(path, "interface/") || startsWith(path, "character/") || startsWith(path, "creature/") ||
        startsWith(path, "item/")) {
        return TextureFormat::Astc4x4;
    }
    return TextureFormat::Astc6x6;
}

Result<std::vector<u8>> bakeTexture(const formats::BlpImage& image, TextureFormat format) {
    const u32 mipCount = std::min<u32>(static_cast<u32>(image.mips.size()), pack::kMaxMips);
    const bool alpha = alphaIsUsed(image);

    pack::TextureHeader header{};
    header.format = static_cast<u16>(format);
    header.flags = static_cast<u16>((alpha ? pack::kTextureHasAlpha : 0) |
                                    (alpha && image.alphaDepth == 1 ? pack::kTextureAlphaCutout : 0));
    header.width = static_cast<u16>(image.width);
    header.height = static_cast<u16>(image.height);
    header.mipCount = static_cast<u16>(mipCount);

    const u32 block = pack::astcBlockSize(format);
    usize offset = alignUp(sizeof(header), 16);
    for (u32 level = 0; level < mipCount; ++level) {
        const u32 w = std::max(1u, image.width >> level);
        const u32 h = std::max(1u, image.height >> level);
        const usize size = block == 0 ? usize{w} * h * 4 : usize{(w + block - 1) / block} * ((h + block - 1) / block) * 16;
        header.mipOffsets[level] = static_cast<u32>(offset);
        header.mipSizes[level] = static_cast<u32>(size);
        offset = alignUp(offset + size, 16);
    }

    std::vector<u8> blob(offset);
    std::memcpy(blob.data(), &header, sizeof(header));

    for (u32 level = 0; level < mipCount; ++level) {
        u8* out = blob.data() + header.mipOffsets[level];
        const std::vector<u8>& rgba = image.mips[level];
        if (block == 0) {
            std::memcpy(out, rgba.data(), header.mipSizes[level]);
            continue;
        }
        astcenc_context* context = encoderFor(block, alpha);
        if (!context) {
            return Error{"Could not create ASTC encoder context"};
        }
        void* slice = const_cast<u8*>(rgba.data());
        astcenc_image astcImage{};
        astcImage.dim_x = std::max(1u, image.width >> level);
        astcImage.dim_y = std::max(1u, image.height >> level);
        astcImage.dim_z = 1;
        astcImage.data_type = ASTCENC_TYPE_U8;
        astcImage.data = &slice;
        // Opaque textures: alpha forced to 1 so the encoder spends no bits on it.
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B,
                                      alpha ? ASTCENC_SWZ_A : ASTCENC_SWZ_1};
        const astcenc_error result =
            astcenc_compress_image(context, &astcImage, &swizzle, out, header.mipSizes[level], 0);
        astcenc_compress_reset(context);
        if (result != ASTCENC_SUCCESS) {
            return Error{std::string("ASTC encode failed: ") + astcenc_get_error_string(result)};
        }
    }
    return blob;
}

formats::BlpImage resampleSquare(const formats::BlpImage& image, u32 size) {
    if (image.width == size && image.height == size && !image.mips.empty()) {
        return image;
    }
    formats::BlpImage out;
    out.width = size;
    out.height = size;
    out.alphaDepth = image.alphaDepth;
    out.encoding = image.encoding;

    // Bilinear with texel centers aligned and wrap-around (terrain textures tile seamlessly).
    const std::vector<u8>& src = image.mips[0];
    const u32 sw = image.width;
    const u32 sh = image.height;
    std::vector<u8> level(usize{size} * size * 4);
    for (u32 y = 0; y < size; ++y) {
        const f32 fy = (static_cast<f32>(y) + 0.5f) * static_cast<f32>(sh) / static_cast<f32>(size) - 0.5f;
        const f32 floorY = std::floor(fy);
        const f32 wy = fy - floorY;
        const u32 y0 = (static_cast<u32>(static_cast<i32>(floorY) + static_cast<i32>(sh))) % sh;
        const u32 y1 = (y0 + 1) % sh;
        for (u32 x = 0; x < size; ++x) {
            const f32 fx = (static_cast<f32>(x) + 0.5f) * static_cast<f32>(sw) / static_cast<f32>(size) - 0.5f;
            const f32 floorX = std::floor(fx);
            const f32 wx = fx - floorX;
            const u32 x0 = (static_cast<u32>(static_cast<i32>(floorX) + static_cast<i32>(sw))) % sw;
            const u32 x1 = (x0 + 1) % sw;
            for (u32 c = 0; c < 4; ++c) {
                auto at = [&](u32 px, u32 py) { return static_cast<f32>(src[(usize{py} * sw + px) * 4 + c]); };
                const f32 top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * wx;
                const f32 bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * wx;
                level[(usize{y} * size + x) * 4 + c] = static_cast<u8>(std::lround(top + (bottom - top) * wy));
            }
        }
    }
    out.mips.push_back(std::move(level));

    // Box-filtered mips down to 1x1.
    for (u32 w = size / 2; w >= 1; w /= 2) {
        const std::vector<u8>& prev = out.mips.back();
        const u32 pw = w * 2;
        std::vector<u8> next(usize{w} * w * 4);
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                for (u32 c = 0; c < 4; ++c) {
                    const u32 sum = prev[((2 * y) * pw + 2 * x) * 4 + c] + prev[((2 * y) * pw + 2 * x + 1) * 4 + c] +
                                    prev[((2 * y + 1) * pw + 2 * x) * 4 + c] +
                                    prev[((2 * y + 1) * pw + 2 * x + 1) * 4 + c];
                    next[(usize{y} * w + x) * 4 + c] = static_cast<u8>((sum + 2) / 4);
                }
            }
        }
        out.mips.push_back(std::move(next));
    }
    return out;
}

Result<std::vector<u8>> buildTextureArray(const std::vector<std::vector<u8>>& layers) {
    if (layers.empty() || layers.size() > 0xFFFF) {
        return Error{"Bad texture array layer count"};
    }
    pack::TextureHeader first;
    if (layers[0].size() < sizeof(first)) {
        return Error{"Texture blob too small"};
    }
    std::memcpy(&first, layers[0].data(), sizeof(first));
    const u32 count = static_cast<u32>(layers.size());

    pack::TextureHeader header = first;
    header.flags = 0;
    header.layers = static_cast<u16>(count);
    usize offset = alignUp(sizeof(header), 16);
    for (u32 level = 0; level < first.mipCount; ++level) {
        header.mipOffsets[level] = static_cast<u32>(offset);
        header.mipSizes[level] = first.mipSizes[level] * count;
        offset = alignUp(offset + header.mipSizes[level], 16);
    }
    std::vector<u8> blob(offset);
    std::memcpy(blob.data(), &header, sizeof(header));
    for (u32 layer = 0; layer < count; ++layer) {
        pack::TextureHeader h;
        if (layers[layer].size() < sizeof(h)) {
            return Error{"Texture blob too small"};
        }
        std::memcpy(&h, layers[layer].data(), sizeof(h));
        if (h.format != first.format || h.width != first.width || h.height != first.height ||
            h.mipCount != first.mipCount || h.layers != 0) {
            return Error{"Texture array layer " + std::to_string(layer) + " does not match layer 0"};
        }
        for (u32 level = 0; level < first.mipCount; ++level) {
            if (h.mipSizes[level] != first.mipSizes[level] ||
                h.mipOffsets[level] + usize{h.mipSizes[level]} > layers[layer].size()) {
                return Error{"Texture array layer " + std::to_string(layer) + ": bad mip " + std::to_string(level)};
            }
            std::memcpy(blob.data() + header.mipOffsets[level] + usize{layer} * first.mipSizes[level],
                        layers[layer].data() + h.mipOffsets[level], h.mipSizes[level]);
        }
    }
    return blob;
}

Result<std::vector<u8>> decodeTextureBlob(const u8* blob, usize size, u32 level, u32 layer) {
    if (size < sizeof(pack::TextureHeader)) {
        return Error{"Texture blob too small"};
    }
    pack::TextureHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (level >= header.mipCount || header.mipOffsets[level] + usize{header.mipSizes[level]} > size) {
        return Error{"Bad texture mip"};
    }
    const u32 layers = std::max<u32>(1, header.layers);
    if (layer >= layers) {
        return Error{"Bad texture layer"};
    }
    // Arrays store every layer of a mip back to back.
    header.mipSizes[level] /= layers;
    header.mipOffsets[level] += header.mipSizes[level] * layer;
    const u32 w = std::max(1u, u32{header.width} >> level);
    const u32 h = std::max(1u, u32{header.height} >> level);
    std::vector<u8> rgba(usize{w} * h * 4);
    if (static_cast<TextureFormat>(header.format) == TextureFormat::R8) {
        const u8* src = blob + header.mipOffsets[level];
        for (usize i = 0; i < usize{w} * h; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = src[i];
            rgba[i * 4 + 3] = 255;
        }
        return rgba;
    }
    const u32 block = pack::astcBlockSize(static_cast<TextureFormat>(header.format));
    if (block == 0) {
        std::memcpy(rgba.data(), blob + header.mipOffsets[level], rgba.size());
        return rgba;
    }
    astcenc_context* context = decoderFor(block);
    if (!context) {
        return Error{"Could not create ASTC decoder context"};
    }
    void* slice = rgba.data();
    astcenc_image image{};
    image.dim_x = w;
    image.dim_y = h;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = &slice;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error result = astcenc_decompress_image(context, blob + header.mipOffsets[level],
                                                          header.mipSizes[level], &image, &swizzle, 0);
    astcenc_decompress_reset(context);
    if (result != ASTCENC_SUCCESS) {
        return Error{std::string("ASTC decode failed: ") + astcenc_get_error_string(result)};
    }
    return rgba;
}

} // namespace vd::bake
