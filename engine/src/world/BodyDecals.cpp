#include "world/BodyDecals.h"

#include "core/Profile.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr f32 kTwoPi = 6.28318531f;
constexpr f32 kMinNormalZ =
    0.3f; // steeper floors are taken as this steep (a shadow on a wall would be a streak)

u16 unorm16(f32 value) {
    return static_cast<u16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

u32 white(f32 opacity) {
    const u32 alpha = static_cast<u32>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (alpha << 24) | 0x00FFFFFFu;
}

// Height of the plane through (0, 0, 0) with `normal` at (x, y).
f32 planeHeight(const glm::vec3& normal, f32 x, f32 y) {
    const f32 nz = std::max(normal.z, kMinNormalZ);
    return -(normal.x * x + normal.y * y) / nz;
}

// A quad lying on a plane: `center` camera-relative, `side` and `ahead` the half extents along its texture's
// u (left to right) and v (bottom to top) in the horizontal plane; corners lifted to the plane of `normal`.
// Texture coordinates over the whole texture, `mirror` flips u.
void writeQuad(const glm::vec3& center, const glm::vec2& side, const glm::vec2& ahead,
               const glm::vec3& normal, f32 lift, u32 color, bool mirror, ParticleVertex* v) {
    const auto corner = [&](f32 sx, f32 sy) {
        const glm::vec2 offset = side * sx + ahead * sy;
        ParticleVertex out;
        out.position[0] = center.x + offset.x;
        out.position[1] = center.y + offset.y;
        out.position[2] = center.z + planeHeight(normal, offset.x, offset.y) + lift;
        const f32 u = 0.5f + 0.5f * (mirror ? -sx : sx);
        out.uv[0] = unorm16(u);
        out.uv[1] = unorm16(0.5f - 0.5f * sy);
        out.color = color;
        return out;
    };
    // The particle pass's order: (-x, +y), (-x, -y), (+x, +y), (+x, -y).
    v[0] = corner(-1.0f, 1.0f);
    v[1] = corner(-1.0f, -1.0f);
    v[2] = corner(1.0f, 1.0f);
    v[3] = corner(1.0f, -1.0f);
}

} // namespace

f32 decalLift(f32 distance) {
    return 0.03f + 0.0015f * distance; // as the selection circle (app/SelectionCircle)
}

bool buildShadow(const ShadowQuad& s, const glm::dvec3& camera, std::span<ParticleVertex> out) {
    if (out.size() < 4) {
        return false;
    }
    // The box's corners turned with the unit (model +X forward, +Y left), and their bounding rectangle.
    const glm::vec2 forward(std::cos(s.yaw), std::sin(s.yaw));
    const glm::vec2 left(-forward.y, forward.x);
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const glm::vec2& c :
         {s.boxMin, glm::vec2(s.boxMax.x, s.boxMin.y), s.boxMax, glm::vec2(s.boxMin.x, s.boxMax.y)}) {
        const glm::vec2 p = (forward * c.x + left * c.y) * s.scale;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec2 offset = (lo + hi) * 0.5f;
    const glm::vec2 half = (hi - lo) * 0.5f;
    glm::vec3 center(s.feet - camera);
    center.x += offset.x;
    center.y += offset.y;
    // The floor's plane passes through the floor under the feet.
    center.z = static_cast<f32>(s.floorZ - camera.z) + planeHeight(s.normal, offset.x, offset.y);
    // Texture u along the world's -Y, v along +X: the disc does not care.
    writeQuad(center, glm::vec2(0.0f, -half.y), glm::vec2(half.x, 0.0f), s.normal,
              decalLift(glm::length(center)), white(s.opacity), false, out.data());
    return true;
}

// --- Footprints
// -------------------------------------------------------------------------------------------------

FootprintPool::FootprintPool() : m_slots(kPlayerSlots + kOtherSlots) {}

void FootprintPool::setOtherSlots(u32 slots) {
    slots = std::clamp(slots, 1u, kOtherSlots);
    if (slots < m_otherSlots) {
        for (u32 i = kPlayerSlots + slots; i < kPlayerSlots + m_otherSlots; ++i) {
            m_alive -= m_slots[i].alive ? 1 : 0;
            m_slots[i].alive = false;
        }
    }
    m_otherSlots = slots;
    m_nextOther %= m_otherSlots;
}

void FootprintPool::add(const Footprint& footprint, bool player) {
    // The client's rings (0x699EF5): the next slot, whatever it held.
    u32& next = player ? m_nextPlayer : m_nextOther;
    const u32 index = player ? next : kPlayerSlots + next;
    next = (next + 1) % (player ? kPlayerSlots : m_otherSlots);
    Slot& slot = m_slots[index];
    m_alive += slot.alive ? 0 : 1;
    slot.print = footprint;
    slot.age = 0.0f;
    slot.alive = true;
}

void FootprintPool::update(f32 dt) {
    if (m_alive == 0) {
        return;
    }
    VD_PROFILE_ZONE("FootprintPool::update");
    for (Slot& slot : m_slots) {
        if (slot.alive) {
            slot.age += dt;
            if (slot.age >= kLifeSeconds) {
                slot.alive = false;
                --m_alive;
            }
        }
    }
}

void FootprintPool::clear() {
    for (Slot& slot : m_slots) {
        slot.alive = false;
    }
    m_alive = 0;
}

f32 FootprintPool::opacity(f32 age) {
    // 0x69A425: 255 * (1 - age / 6 s), at most 127.
    return std::clamp(1.0f - age / kLifeSeconds, 0.0f, kMaxOpacity);
}

u32 FootprintPool::build(u32 texture, const glm::dvec3& camera, std::span<ParticleVertex> out) const {
    u32 quads = 0;
    if (m_alive == 0) {
        return 0;
    }
    for (const Slot& slot : m_slots) {
        if (!slot.alive || slot.print.texture != texture) {
            continue;
        }
        if (out.size() < u64{quads + 1} * 4) {
            break;
        }
        const Footprint& f = slot.print;
        const glm::vec2 forward(std::cos(f.facing), std::sin(f.facing));
        const glm::vec2 right(forward.y, -forward.x);
        const glm::vec3 center(f.position - camera);
        writeQuad(center, right * (f.width * 0.5f), forward * (f.length * 0.5f), f.normal,
                  decalLift(glm::length(center)),
                  (static_cast<u32>(opacity(slot.age) * 255.0f + 0.5f) << 24) | 0xFFFFFFu, f.right,
                  out.data() + u64{quads} * 4);
        ++quads;
    }
    return quads;
}

// --- Water ripples
// ------------------------------------------------------------------------------------------------

bool makesRipples(f32 depth, f32 height) {
    return depth > 0.0f && depth < std::max(2.0f * height, 1.0f); // 0x5FA7FC
}

Ripple rippleFor(RippleKind kind, const glm::dvec3& surface, f32 facing, f32 scale, f32 speed, f32 depth,
                 f32 height, const std::array<f32, 3>& random, f32& nextSeconds) {
    // 0x5FA760: the deeper the unit (past its collision height), the smaller, fainter and shorter its
    // ripples.
    const f32 reach = std::max(2.0f * height, 1.0f);
    f32 shallow = 1.0f;
    if (depth > reach * 0.5f) {
        shallow = std::clamp((reach - depth) / (reach * 0.5f) * 0.5f + 0.5f, 0.5f, 1.0f);
    }
    const bool wake = kind == RippleKind::Moving;
    // A wake grows with the speed (2.5 yd/s: as is, up to 20) and comes more often.
    const f32 pace = wake && speed > 0.0001f ? std::min(speed, 20.0f) / 2.5f : 1.0f;
    Ripple r;
    r.center = surface;
    r.wake = wake;
    r.facing = wake ? facing : random[0] * kTwoPi;
    r.size = std::clamp(scale / 3.0f * (0.9f + 0.2f * random[0]), 1.0f / 3.0f, 5.0f / 3.0f) * shallow;
    r.life = (0.6f + 0.1f * random[1]) * shallow;
    r.opacity = shallow;
    r.growth = (1.0f + 0.5f * random[2]) * pace;
    if (kind == RippleKind::Idle) {
        r.size *= 0.6f;
        r.opacity *= 0.8f;
        r.growth *= 0.25f;
    }
    nextSeconds = wake ? 0.25f * shallow / pace : 0.4f + 0.05f * random[1];
    return r;
}

RipplePool::RipplePool() : m_slots(kPlayerSlots + kOtherSlots) {}

void RipplePool::add(const Ripple& ripple, bool player) {
    u32& next = player ? m_nextPlayer : m_nextOther;
    const u32 index = player ? next : kPlayerSlots + next;
    next = (next + 1) % (player ? kPlayerSlots : kOtherSlots);
    Slot& slot = m_slots[index];
    m_alive += slot.alive ? 0 : 1;
    slot.ripple = ripple;
    slot.age = 0.0f;
    slot.alive = true;
}

void RipplePool::update(f32 dt) {
    if (m_alive == 0) {
        return;
    }
    VD_PROFILE_ZONE("RipplePool::update");
    for (Slot& slot : m_slots) {
        if (slot.alive) {
            slot.age += dt;
            if (slot.age >= slot.ripple.life) {
                slot.alive = false;
                --m_alive;
            }
        }
    }
}

void RipplePool::clear() {
    for (Slot& slot : m_slots) {
        slot.alive = false;
    }
    m_alive = 0;
}

f32 RipplePool::opacity(const Ripple& ripple, f32 age) {
    // 0x68F5C0: in over the first 40 % of its life, out over the rest.
    if (ripple.life <= 0.0f || age >= ripple.life) {
        return 0.0f;
    }
    const f32 in = ripple.life * kFadeInShare;
    const f32 t = age < in ? age / in : (ripple.life - age) / (ripple.life - in);
    return std::clamp(t, 0.0f, 1.0f) * ripple.opacity;
}

u32 RipplePool::build(bool wake, const glm::dvec3& camera, std::span<ParticleVertex> out) const {
    u32 quads = 0;
    if (m_alive == 0) {
        return 0;
    }
    for (const Slot& slot : m_slots) {
        if (!slot.alive || slot.ripple.wake != wake) {
            continue;
        }
        if (out.size() < u64{quads + 1} * 4) {
            break;
        }
        const Ripple& r = slot.ripple;
        const f32 half = r.size + r.growth * slot.age;
        const glm::vec2 forward(std::cos(r.facing), std::sin(r.facing));
        const glm::vec2 right(forward.y, -forward.x);
        const glm::vec3 center(r.center - camera);
        // Flat on the water; the wake's V (its texture's top) pointing the way the unit swims.
        writeQuad(center, right * half, forward * half, glm::vec3(0.0f, 0.0f, 1.0f), 0.02f,
                  white(opacity(r, slot.age)), false, out.data() + u64{quads} * 4);
        ++quads;
    }
    return quads;
}

} // namespace vd
