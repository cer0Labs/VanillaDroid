#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/M2.h"

#include <vector>

namespace vd::bake {

// Phase 11: a sky model blob (pack/SkyFormat.h) from an M2 of Environments\Stars: the bind-pose mesh with each
// batch's static opacity in its vertices, the batches merged by texture and blend, and the root bone's rotation
// over its loop (a global sequence, else the first sequence). Bones other than the root are not animated (only
// CavernsOfTimeSky has any, and no 1.12 map reaches it). Its path in models.vdpk: pack::skyModelPath.
Result<std::vector<u8>> bakeSkyModel(const formats::M2Model& model);

} // namespace vd::bake
