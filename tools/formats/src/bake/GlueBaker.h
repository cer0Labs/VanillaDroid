#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/M2.h"

#include <vector>

namespace vd::bake {

// Phase 18: a glue scene's blob (pack/GlueFormat.h) from its M2 (Interface\Glues\Models\UI_*.m2): its first camera over
// its first sequence (the cinematic camera blob of bakeCinematicCamera) and its lights at the first frame.
Result<std::vector<u8>> bakeGlueScene(const formats::M2Model& model);

} // namespace vd::bake
