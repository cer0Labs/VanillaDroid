#include "app/SelectionCircle.h"

#include "core/Profile.h"
#include "ui/DrawList.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr f32 kMinRingRadius = 0.45f; // a critter's circle still shows around it
constexpr f32 kRingPerBoundingRadius = 2.0f;
constexpr f32 kDefaultBoundingRadius = 0.389f; // DEFAULT_WORLD_OBJECT_SIZE (vMaNGOS Object.h): no field yet
constexpr f32 kResampleYards = 0.02f; // the floor is sampled again when the unit moved more than this
constexpr f32 kFlatWindow = 1.0f;     // no floor this near the feet: swimming, jumping, flying: lie flat

u16 unorm16(f32 value) {
    return static_cast<u16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

} // namespace

f32 SelectionCircle::ringRadius(f32 boundingRadius) {
    const f32 radius =
        boundingRadius > 0.0f && std::isfinite(boundingRadius) ? boundingRadius : kDefaultBoundingRadius;
    return std::max(kMinRingRadius, radius * kRingPerBoundingRadius);
}

u32 SelectionCircle::color(game::Reaction reaction) {
    switch (reaction) {
    case game::Reaction::Hostile: return ui::rgba(255, 0, 0);
    case game::Reaction::Friendly: return ui::rgba(0, 255, 0);
    default: return ui::rgba(255, 255, 0);
    }
}

u32 SelectionCircle::build(const glm::dvec3& feet, f32 ringRadius, u32 color, const glm::dvec3& camera,
                           const glm::vec3& forward, std::span<ParticleVertex> out, GroundQuery ground,
                           const void* context) {
    const u32 cells = m_cells;
    const u32 corners = cells + 1;
    if (!(ringRadius > 0.0f) || out.size() < u64{cells} * cells * 4) {
        return 0;
    }
    VD_PROFILE_ZONE("SelectionCircle::build");
    const f32 half = ringRadius / kRingFraction;
    const f32 step = 2.0f * half / static_cast<f32>(cells);
    const glm::dvec3 moved = feet - m_feet;
    if (half != m_halfSize || glm::dot(moved, moved) > f64{kResampleYards} * kResampleYards) {
        // The floor under each grid corner. The centre first: with no floor at the feet (swimming, jumping, a
        // flying mount) the circle lies flat where the unit is; corners with no floor (a cliff's edge) take
        // the centre's. Floors are looked for as far up and down as a 40 degree slope reaches.
        m_feet = feet;
        m_halfSize = half;
        const f32 fz = static_cast<f32>(feet.z);
        const std::optional<f32> centre =
            ground ? ground(context, static_cast<f32>(feet.x), static_cast<f32>(feet.y), fz + kFlatWindow,
                            fz - kFlatWindow)
                   : std::nullopt;
        const f32 centreHeight = centre ? *centre - fz : 0.0f;
        const f32 reach = kFlatWindow + half * 0.85f;
        for (u32 j = 0; j < corners; ++j) {
            for (u32 i = 0; i < corners; ++i) {
                f32 height = centreHeight;
                if (centre && (i != cells / 2 || j != cells / 2)) {
                    const f32 x = static_cast<f32>(feet.x) - half + step * static_cast<f32>(i);
                    const f32 y = static_cast<f32>(feet.y) - half + step * static_cast<f32>(j);
                    if (const std::optional<f32> floor = ground(context, x, y, fz + reach, fz - reach)) {
                        height = *floor - fz;
                    }
                }
                m_heights[j * kMaxSide + i] = height;
            }
        }
    }

    // The texture's bright side (its bottom) towards the camera; looking straight down, the way the camera
    // faces.
    glm::vec2 toCamera(static_cast<f32>(camera.x - feet.x), static_cast<f32>(camera.y - feet.y));
    if (glm::dot(toCamera, toCamera) < 0.0025f) {
        toCamera = -glm::vec2(forward);
    }
    if (glm::dot(toCamera, toCamera) >= 0.0025f) {
        m_toCamera = glm::normalize(toCamera);
    }
    const glm::vec2 facing = m_worldAligned ? glm::vec2(0.0f, 1.0f) : m_toCamera; // ours: a spell's shadow
    const glm::vec2 side(-facing.y, facing.x); // the screen's right, seen from the camera
    // Lifted off the floor by a little more far away, where the depth buffer is coarser.
    const glm::vec3 base(feet - camera);
    const f32 lift = 0.03f + 0.0015f * glm::length(base);
    const auto corner = [&](u32 i, u32 j) {
        const glm::vec2 offset(-half + step * static_cast<f32>(i), -half + step * static_cast<f32>(j));
        ParticleVertex v;
        v.position[0] = base.x + offset.x;
        v.position[1] = base.y + offset.y;
        v.position[2] = base.z + m_heights[j * kMaxSide + i] + lift;
        v.uv[0] = unorm16(0.5f + 0.5f * glm::dot(offset, side) / half);
        v.uv[1] = unorm16(0.5f + 0.5f * glm::dot(offset, facing) / half);
        v.color = color;
        return v;
    };
    // Quads as the particle pass draws them: corners (-x, +y), (-x, -y), (+x, +y), (+x, -y).
    ParticleVertex* v = out.data();
    for (u32 j = 0; j < cells; ++j) {
        for (u32 i = 0; i < cells; ++i) {
            *v++ = corner(i, j + 1);
            *v++ = corner(i, j);
            *v++ = corner(i + 1, j + 1);
            *v++ = corner(i + 1, j);
        }
    }
    return cells * cells;
}

} // namespace vd
