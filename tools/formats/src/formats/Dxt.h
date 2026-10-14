#pragma once

#include "core/Types.h"

namespace vd::formats {

enum class DxtFormat : u8 { Dxt1, Dxt3, Dxt5 };

// Decodes a DXT image (width x height, blocks of 4x4) into tightly packed RGBA8.
// `opaqueDxt1`: treat DXT1's transparent index as opaque black (BLP textures without alpha).
void decodeDxt(DxtFormat format, const u8* blocks, u32 width, u32 height, u8* rgbaOut, bool opaqueDxt1);

// Size in bytes of a DXT image of the given dimensions.
usize dxtImageSize(DxtFormat format, u32 width, u32 height);

} // namespace vd::formats
