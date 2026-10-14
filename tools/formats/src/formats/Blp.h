#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <span>
#include <vector>

namespace vd::formats {

// How the texture was stored in the client (drives the choice of the baked format).
enum class BlpEncoding : u8 { Palette, Dxt1, Dxt3, Dxt5, Argb8888 };

struct BlpImage {
    u32 width = 0;
    u32 height = 0;
    u8 alphaDepth = 0; // 0 = opaque, 1 = cut-out, 4/8 = smooth alpha
    BlpEncoding encoding = BlpEncoding::Palette;
    // RGBA8, mip 0 first. Mip i is max(1, width >> i) x max(1, height >> i).
    std::vector<std::vector<u8>> mips;
};

// Decodes a BLP2 texture (the only BLP version shipped with 1.12) into RGBA8 mips.
Result<BlpImage> decodeBlp(std::span<const u8> data);

} // namespace vd::formats
