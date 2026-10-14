#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"

#include <atomic>

namespace vd {

// Phase 18 (world): pack textures written into a staging block by an IO thread, in the layout the GPU samples, so that
// the game thread only creates the image and records the copy (Renderer::setTerrainTextures). Any thread.

// A pack texture blob's header as it is staged: the blob as it is (header included, the mip offsets unchanged) where the
// GPU samples its format, else its ASTC decoded to RGBA8 (mips from offset 0, 16-byte aligned). `bytes`: what it takes
// in the staging block.
pack::TextureHeader stagedTextureLayout(const pack::TextureHeader& blob, bool nativeAstc, u64& bytes);

// Writes the blob (`size` bytes, its header first) into `out` as `staged` (stagedTextureLayout's) lays it out, one mip
// of one layer at a time: `cancel` is polled between them (true: stop and return false) and `units`, when given, counts
// them (a texture adds layers * mipCount).
bool stageTexture(const u8* blob, usize size, const pack::TextureHeader& staged, u8* out,
                  const std::atomic<bool>* cancel = nullptr, std::atomic<u32>* units = nullptr);

} // namespace vd
