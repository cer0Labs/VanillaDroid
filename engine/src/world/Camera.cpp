#include "world/Camera.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <cmath>

namespace vd {

glm::vec3 Camera::forward() const {
    const f32 c = std::cos(pitch);
    return {c * std::cos(yaw), c * std::sin(yaw), std::sin(pitch)};
}

glm::vec3 Camera::right() const {
    // X north, Y west: turning right (clockwise seen from above) from north faces east (-Y).
    return {std::sin(yaw), -std::cos(yaw), 0.0f};
}

glm::mat4 Camera::view() const {
    const glm::vec3 f = forward();
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (roll != 0.0f) { // up turned about the view direction (Rodrigues)
        const f32 c = std::cos(roll);
        const f32 s = std::sin(roll);
        up = up * c + glm::cross(f, up) * s + f * glm::dot(f, up) * (1.0f - c);
    }
    return glm::lookAtRH(glm::vec3(0.0f), f, up);
}

glm::mat4 Camera::projection(f32 aspect) const {
    glm::mat4 projection = glm::perspectiveRH_ZO(fovY, aspect, nearPlane, farPlane);
    projection[1][1] = -projection[1][1]; // Vulkan: clip-space y points down
    return projection;
}

Frustum Frustum::fromViewProj(const glm::mat4& m) {
    // Gribb/Hartmann on the rows of the matrix; Vulkan depth range: near plane is row 2 alone.
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
    Frustum f;
    f.planes = {row3 + row0, row3 - row0, row3 + row1, row3 - row1, row2, row3 - row2};
    for (glm::vec4& plane : f.planes) {
        plane /= glm::length(glm::vec3(plane));
    }
    return f;
}

bool Frustum::intersects(const glm::vec3& min, const glm::vec3& max) const {
    for (const glm::vec4& plane : planes) {
        // The box corner furthest along the plane normal.
        const glm::vec3 corner(plane.x >= 0.0f ? max.x : min.x, plane.y >= 0.0f ? max.y : min.y,
                               plane.z >= 0.0f ? max.z : min.z);
        if (plane.x * corner.x + plane.y * corner.y + plane.z * corner.z + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsSphere(const glm::vec3& center, f32 radius) const {
    for (const glm::vec4& plane : planes) {
        if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

} // namespace vd
