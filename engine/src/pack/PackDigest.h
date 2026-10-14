#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>

namespace vd::pack {

// Phase 22 (distribution): the recipe of a pack file (pack/PackManifest.h), a digest of what it holds rather
// than of its bytes: the pack version and client build, then every asset in path-hash order with its type,
// raw size and raw (decompressed) bytes. Blob order, alignment, compression and the name table do not count,
// so two bakes of the same data that wrote their blobs in another order have the same recipe. The first 16
// bytes of a SHA-256, in hex. Reads the whole pack (seconds for the big ones): tools only.
Result<std::string> packDigest(const std::filesystem::path& packFile);

} // namespace vd::pack
