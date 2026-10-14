#pragma once

#include "core/Types.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace vd {

// Free camera in world axes (X north, Y west, Z up). Position in double precision: the world spans
// +-17066 yards and everything is rendered relative to the camera.
struct Camera {
    glm::dvec3 position{0.0};
    f32 yaw = 0.0f;   // radians, 0 = looking north (+X), positive turns towards west (+Y)
    f32 pitch = 0.0f; // radians, positive looks up
    f32 roll = 0.0f;  // phase 14 (cinematics): radians about the view direction, positive tilts clockwise
    f32 fovY = 1.0f;  // vertical field of view, radians
    f32 nearPlane = 0.5f;
    f32 farPlane = 1000.0f;

    glm::vec3 forward() const;
    glm::vec3 right() const; // horizontal
    // Rotation-only view (camera at the origin): world data is sent camera-relative.
    glm::mat4 view() const;
    // Vulkan clip space: y down, depth 0..1.
    glm::mat4 projection(f32 aspect) const;
};

// Six planes (xyz = normal pointing inside, w = distance) in camera-relative world space.
struct Frustum {
    std::array<glm::vec4, 6> planes{};

    static Frustum fromViewProj(const glm::mat4& viewProj);
    // Axis-aligned box, camera-relative. Conservative: may accept boxes just outside the corners.
    bool intersects(const glm::vec3& min, const glm::vec3& max) const;
    bool intersectsSphere(const glm::vec3& center, f32 radius) const;
};

} // namespace vd
