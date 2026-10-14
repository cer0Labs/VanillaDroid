#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/ModelFormat.h"

#include <array>
#include <vector>

namespace vd::bake {

// Baked model blobs (pack/ModelFormat.h). Triangles are regrouped so every group has one part per
// material, parts sorted by blend class (opaque, alpha key, then blended). M2 batches the original never shows
// (opacity 0 in every key) are left out and counted into `invisibleBatches`.
Result<std::vector<u8>> bakeM2(const formats::M2Model& model, u32* invisibleBatches = nullptr);
Result<std::vector<u8>> bakeWmo(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups);

// Model -> world transform of an ADT/WDT placement (MDDF/MODF): position and rotation (degrees) in the
// placement axes (x' = 17066.67 - Y, y' = Z, z' = 17066.67 - X), uniform scale. 3x4 row-major: world =
// M * (x, y, z, 1) with rows for world X, Y, Z.
std::array<f32, 12> placementTransform(const f32 (&position)[3], const f32 (&rotationDegrees)[3], f32 scale);
std::array<f32, 12> placementTransform(const std::array<f32, 3>& position, const std::array<f32, 3>& rotationDegrees,
                                       f32 scale);
// Placement-axes point -> world.
std::array<f32, 3> placementToWorld(const std::array<f32, 3>& position);

} // namespace vd::bake
