#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>

namespace vd::formats {

// Minimal PNG writer (RGBA8, uncompressed deflate) for inspecting decoded/baked data by eye.
// Debug tooling only: files are large, but there is no dependency.
Result<void> writePng(const std::filesystem::path& path, u32 width, u32 height, const u8* rgba);

} // namespace vd::formats
