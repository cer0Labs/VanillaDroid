#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/CinematicFormat.h"

#include <glm/vec3.hpp>

namespace vd {

// Where a cinematic's camera is at a moment of its flight (world, server axes).
struct CinematicView {
    glm::dvec3 eye{0.0};
    glm::dvec3 target{0.0};
    f32 roll = 0.0f;           // radians about the view direction
    f32 fovDiagonal = 0.785f;  // radians, across the screen's diagonal (verticalFov converts it)
    f32 nearClip = 0.2f;
    f32 farClip = 1000.0f;
};

// Phase 14: a baked cinematic camera (pack/CinematicFormat.h) over its blob, which must outlive it. Sampling is pure:
// the position and target tracks (bezier between keys as the flybys are authored, also hermite, linear and step)
// plus their bases, in the model's space, placed at the CinematicCamera.dbc row's origin turned by its facing.
class CinematicCamera {
public:
    Result<void> bind(const u8* blob, usize size);
    bool valid() const { return m_header != nullptr; }
    u32 durationMs() const { return m_header ? m_header->durationMs : 0; }
    CinematicView sample(u32 ms, const glm::dvec3& origin, f32 facing) const;

private:
    glm::vec3 sampleTrack(const pack::CinematicTrack& track, u32 ms) const;

    const pack::CinematicCameraHeader* m_header = nullptr;
    const pack::CinematicKey* m_keys = nullptr;
};

// The vertical field of view (radians) of a diagonal one on a screen `aspect` (width / height) wide.
f32 verticalFov(f32 diagonal, f32 aspect);

} // namespace vd
