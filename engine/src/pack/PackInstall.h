#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>

// Phase 22 (phonebake, ROADMAP 22.7): a bake step done on the phone goes into a staging folder next to packs/
// (a step cut short never leaves half a pack where the game reads them); once it ends well, its packs move
// into the packs folder, each renamed in place (the same file system: atomic, and a pack the game has mapped
// keeps its old bytes until it lets go of them), and their entries of the staging folder's manifest.json (the
// recipes the bake recorded) join packs/manifest.json, as the PC's bake leaves it. `vd_baker install` runs
// it.
namespace vd::pack {

struct InstallResult {
    u32 packs = 0;
    u64 bytes = 0;
};
// Moves the packs the staging folder's manifest.json lists (each with the size it recorded), merges their
// entries into <packsDir>/manifest.json, then removes the staging manifest and the staging folder's .cache (a
// textures step's resume cache). An error when the staging folder holds no bake or a pack is missing or of
// another size (nothing is moved then), or when a move or the manifest fails.
Result<InstallResult> installStagedPacks(const std::filesystem::path& stagingDir,
                                         const std::filesystem::path& packsDir);

} // namespace vd::pack
