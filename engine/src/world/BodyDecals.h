#pragma once

#include "core/Types.h"
#include "render/Renderer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <span>
#include <vector>

namespace vd {

// Phase 14 (bodyfx): the marks the 1.12 client's units leave on the ground and on the water, and their blob
// shadows, as quads for the renderer's particle buffer (ParticleVertex: camera-relative corners, 4 per quad).
// Pure bookkeeping and geometry, tested on its own (world/UnitBodyEffects feeds it); nothing allocates after
// construction. Game thread.

// --- Blob shadows (WoW.exe 0x6D7920)
// --------------------------------------------------------------------------------

// Textures\ShadowBlob.blp (grey disc, 160/255 at its centre, white around it, 1-bit alpha) multiplied into
// what is already drawn (Mod blend), fading with the model's own opacity. It covers the world-aligned
// rectangle around the box of the sequence the unit's model plays (its xy turned with the unit and scaled,
// clamped to 5 yards; 0x6D7CB2 takes the bounding rectangle of the four turned corners): a long model facing
// a diagonal casts a rounder, bigger blob. The client projects it over the triangles it finds from 1.67
// half-heights of the box below its feet to one above; here it is one quad on the plane of the floor under
// the feet (the floor's point and normal), lifted a little against z-fighting.
struct ShadowQuad {
    glm::dvec3 feet{0.0};
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
    glm::vec2 boxMin{-0.5f}; // model space, yards (unscaled)
    glm::vec2 boxMax{0.5f};
    f64 floorZ = 0.0;                   // the floor's height under the feet
    glm::vec3 normal{0.0f, 0.0f, 1.0f}; // the floor's
    f32 opacity = 1.0f;
};
// The shadow's 4 corners into `out` (ParticleVertex order: (-x, +y), (-x, -y), (+x, +y), (+x, -y)); color
// white with the opacity in its alpha (drawn with kParticleDrawShadow). False when `out` is too small.
bool buildShadow(const ShadowQuad& shadow, const glm::dvec3& camera, std::span<ParticleVertex> out);
// How far the shadow is lifted off its floor, by its distance to the camera (the depth buffer is coarser far
// away).
f32 decalLift(f32 distance);

// --- Footprints (WoW.exe 0x699E70, drawn by 0x69A3E0)
// ---------------------------------------------------------------

struct Footprint {
    glm::dvec3 position{0.0}; // where the foot came down, on the ground
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    f32 facing = 0.0f;  // radians, the unit's
    f32 width = 0.0f;   // yards, across the foot
    f32 length = 0.0f;  // along it
    u32 texture = 0;    // the caller's texture index
    bool right = false; // the right foot: the texture mirrored
};

// The client's two rings of footprints: 64 for the player's own, 512 for everyone else's; a new one takes the
// oldest slot of its ring. Each lives 6 s: drawn at most half opaque (alpha 127), fading out linearly over
// the last 3 s (alpha = min(127, 255 * (1 - age / 6))).
class FootprintPool {
public:
    static constexpr u32 kPlayerSlots = 64;
    static constexpr u32 kOtherSlots = 512;
    static constexpr f32 kLifeSeconds = 6.0f;
    static constexpr f32 kMaxOpacity = 127.0f / 255.0f;

    FootprintPool();
    // Quality: how many of the others' slots are used (<= kOtherSlots); fewer drops the oldest at once.
    void setOtherSlots(u32 slots);
    void add(const Footprint& footprint, bool player);
    void update(f32 dt);
    void clear();
    u32 alive() const { return m_alive; }
    static f32 opacity(f32 age);

    // The quads of the live footprints using `texture`, into `out` (from its start); returns how many were
    // written (stops when `out` is full).
    u32 build(u32 texture, const glm::dvec3& camera, std::span<ParticleVertex> out) const;

private:
    struct Slot {
        Footprint print;
        f32 age = 0.0f;
        bool alive = false;
    };
    std::vector<Slot> m_slots; // the player's ring, then the others'
    u32 m_nextPlayer = 0;
    u32 m_nextOther = 0;
    u32 m_otherSlots = kOtherSlots;
    u32 m_alive = 0;
};

// --- Water ripples (WoW.exe 0x5FA760 emits, 0x68F9F0 / 0x68F5C0 keep, XTextures\splash)
// ------------------------------

// What a unit in water is doing, for its ripples.
enum class RippleKind : u8 {
    Idle,   // standing: small rings, now and then
    Moving, // a wake behind it
    Enter,  // entering or leaving the water: a ring at once
};

// One ripple flat on the water: XTextures\splash\splash.blp (a ring) or wake.blp (a V pointing the way the
// unit swims), added to what is drawn (Add blend). It grows `growth` yards a second from `size` (half its
// width) for `life` seconds, fading in for the first 40 % of it and out for the rest, at most `opacity`.
struct Ripple {
    glm::dvec3 center{0.0}; // on the surface
    f32 facing = 0.0f;      // the wake's direction (radians); a ring's random turn
    f32 size = 0.5f;
    f32 growth = 1.0f;
    f32 opacity = 1.0f;
    f32 life = 0.65f;
    bool wake = false;
};

// The client's rule for a unit's next ripple. `scale`: OBJECT_FIELD_SCALE_X; `speed`: yards per second (a
// wake's); `depth`: of the water over its feet; `height`: its collision height (CreatureModelData x scale);
// `random`: three numbers in [0, 1). Returns the ripple (centred on `surface`) and, in `nextSeconds`, when
// the unit makes the next: a wake every 0.25 s x 2.5 / speed, rings every 0.4-0.45 s.
Ripple rippleFor(RippleKind kind, const glm::dvec3& surface, f32 facing, f32 scale, f32 speed, f32 depth,
                 f32 height, const std::array<f32, 3>& random, f32& nextSeconds);
// Whether a unit this deep (feet under the surface) makes ripples: up to twice its collision height (at least
// 1 yd).
bool makesRipples(f32 depth, f32 height);

// 32 slots for the player's ripples, 96 for everyone else's (oldest replaced).
class RipplePool {
public:
    static constexpr u32 kPlayerSlots = 32;
    static constexpr u32 kOtherSlots = 96;
    static constexpr f32 kFadeInShare = 0.4f;

    RipplePool();
    void add(const Ripple& ripple, bool player);
    void update(f32 dt);
    void clear();
    u32 alive() const { return m_alive; }
    // The opacity of a ripple `age` seconds old.
    static f32 opacity(const Ripple& ripple, f32 age);
    // The quads of the live ripples of one texture (`wake` or the rings) into `out`; returns how many.
    u32 build(bool wake, const glm::dvec3& camera, std::span<ParticleVertex> out) const;

private:
    struct Slot {
        Ripple ripple;
        f32 age = 0.0f;
        bool alive = false;
    };
    std::vector<Slot> m_slots;
    u32 m_nextPlayer = 0;
    u32 m_nextOther = 0;
    u32 m_alive = 0;
};

} // namespace vd
