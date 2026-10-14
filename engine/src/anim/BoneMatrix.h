#pragma once

#include "core/Types.h"

namespace vd::anim {

// Skinning matrix of one bone: 3x4 row-major affine (rows of [rotation*scale | translation]) taking bind-pose
// model space to animated model space. Layout shared with the GPU bone palette (shaders/actor.vert).
struct BoneMatrix {
    f32 rows[12];
};

} // namespace vd::anim
