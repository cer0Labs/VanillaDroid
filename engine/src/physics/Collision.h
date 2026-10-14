#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace vd::physics {

// Flag of the terrain triangles rebuilt from the height grid, next to the pack's kCollision* flags.
constexpr u8 kTriangleTerrain = 1u << 7;
// Phase 8: triangles of an object that moves (a transport's deck), see CollisionWorld::addObject.
constexpr u8 kTriangleMoving = 1u << 6;

// A collision triangle relative to its set's origin. Two-sided: the normal's sign means nothing.
struct Triangle {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
    glm::vec3 normal; // unit, cross(b - a, c - a)
    u8 flags;
};

// The triangles around one query, or around one step of a character, relative to `origin` (world, double)
// so the math runs on small floats. The capacity is fixed at construction: gathering never allocates.
class TriangleSet {
public:
    explicit TriangleSet(u32 capacity = 4096);

    void reset(const glm::dvec3& origin);
    // Degenerate triangles are skipped. False (and overflowed() set) when the set is full.
    bool add(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, u8 flags);

    const glm::dvec3& origin() const { return m_origin; }
    std::span<const Triangle> triangles() const { return m_triangles; }
    usize size() const { return m_triangles.size(); }
    bool overflowed() const { return m_overflowed; }
    glm::vec3 toLocal(const glm::dvec3& world) const { return glm::vec3(world - m_origin); }
    glm::dvec3 toWorld(const glm::vec3& local) const { return m_origin + glm::dvec3(local); }

private:
    glm::dvec3 m_origin{0.0};
    std::vector<Triangle> m_triangles;
    u32 m_capacity = 0;
    bool m_overflowed = false;
};

// First contact along a motion.
struct Hit {
    f32 t = 1.0f;                        // fraction of the motion travelled before touching
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  // unit, on the side the moving object comes from
    glm::vec3 point{0.0f};               // contact point on the surface
    u8 flags = 0;                        // the triangle's
    bool hit = false;
};

// All in the set's local coordinates; triangles with any of `ignore` flags are skipped.
// Segment from `from` to `from + delta`.
bool raycast(const TriangleSet& set, const glm::vec3& from, const glm::vec3& delta, Hit& hit, u8 ignore = 0);
// Sphere of `radius` moving by `delta`. A sphere already touching a surface it moves into stops at t = 0;
// moving away from it is free, so a sphere never gets stuck on what it merely touches.
bool sweepSphere(const TriangleSet& set, const glm::vec3& center, f32 radius, const glm::vec3& delta, Hit& hit,
                 u8 ignore = 0);
// Moves a sphere out of the surfaces it overlaps (a few relaxation passes). Returns the total push applied to
// `center`, zero when it was free.
glm::vec3 depenetrateSphere(const TriangleSet& set, glm::vec3& center, f32 radius, u8 ignore = 0);

glm::vec3 closestPointOnTriangle(const glm::vec3& p, const Triangle& triangle);

} // namespace vd::physics
