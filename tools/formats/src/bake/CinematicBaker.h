#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/M2.h"

#include <vector>

namespace vd::bake {

// Phase 14: a cinematic camera's blob (pack/CinematicFormat.h) from its M2 (Cameras\*.m2): the model's first camera
// with its tracks over the first sequence (times from its start), keeping the keys' control points.
Result<std::vector<u8>> bakeCinematicCamera(const formats::M2Model& model);

} // namespace vd::bake
