#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <vector>

namespace vd::vk {

// CPU ASTC -> RGBA8 decode for GPUs without ASTC (desktop development only). Always an error on
// Android, where every GPU samples ASTC natively.
Result<std::vector<u8>> decodeAstcSoftware(const u8* data, usize size, u32 width, u32 height, u32 blockSize);

} // namespace vd::vk
