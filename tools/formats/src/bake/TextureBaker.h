#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/Blp.h"
#include "pack/PackFormat.h"

#include <string_view>
#include <vector>

namespace vd::bake {

// Changes whenever the encoding of the same input would change (quality, swizzles, blob layout).
// Part of the bake-cache key, so bumping it invalidates cached textures.
constexpr u32 kTextureEncoderVersion = 2; // 2: color error no longer weighted by alpha

// Block size by category (canonical path): UI, characters, creatures and items are seen up close and
// get 4x4 (8 bpp); world geometry and terrain get 6x6 (3.56 bpp). Tune with on-device comparisons.
pack::TextureFormat chooseTextureFormat(std::string_view canonicalPath);

// Encodes every mip of the image and returns the texture blob (TextureHeader + mip data).
// Thread-safe: each thread keeps its own ASTC encoder contexts.
Result<std::vector<u8>> bakeTexture(const formats::BlpImage& image, pack::TextureFormat format);

// Resamples mip 0 to size x size (bilinear) and rebuilds the mip chain with a box filter. Images that
// already have that size keep their own mips. Used to give every terrain texture the same size.
formats::BlpImage resampleSquare(const formats::BlpImage& image, u32 size);

// 2D array blob (TextureHeader::layers = N) from N texture blobs of identical format, size and mips.
Result<std::vector<u8>> buildTextureArray(const std::vector<std::vector<u8>>& layers);

// Decodes an ASTC texture blob back to RGBA8 (mip `level`, array layer `layer`). Used by tests and
// tools to check the baked output.
Result<std::vector<u8>> decodeTextureBlob(const u8* blob, usize size, u32 level, u32 layer = 0);

} // namespace vd::bake
