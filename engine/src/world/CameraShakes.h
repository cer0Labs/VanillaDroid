#pragma once

#include "core/Types.h"
#include "game/SpellKitFx.h"

#include <glm/vec3.hpp>

#include <array>

namespace vd {

// Phase 14 (spellfx): the camera shakes of the 1.12 client (CGCamera, WoW.exe 0x511760 / 0x511610 /
// 0x5116E0). Spell kits (SpellVisualKit's shake: three CameraShakes of SpellEffectCameraShakes), the `$SHK`
// events of models and the footsteps and death thuds of big creatures (CreatureModelData) add a shake at a
// place; every frame the camera moves by the sum, over the three directions, of the strongest shake in that
// direction: amplitude * sin(2 pi * frequency * t) (times exp(-coefficient * t) when it decays), t = seconds
// since it started + its phase, until t reaches its duration. Its amplitude counts in full within 9 yards of
// the camera, then falls by 0.7 every 9 yards further, and nothing beyond 80 yards. Forward and side follow
// the facing of the unit the camera follows. Pure (no world, no allocation): tested in tests/world.
class CameraShakes {
public:
    static constexpr u32 kMaxShakes = 32;
    static constexpr f32 kFullDistance = 9.0f; // yards
    static constexpr f32 kMaxDistance = 80.0f; // yards
    static constexpr f32 kFalloffBase = 0.7f;  // per kFullDistance beyond it

    // Starts a shake at a place. The oldest is replaced when the table is full.
    void add(const game::CameraShake& shake, const glm::dvec3& at, u32 nowMs);
    // The three shakes of SpellEffectCameraShakes `id` (a kit's, a `$SHK` event's).
    void addEffect(const game::SpellKitFx& fx, u32 effectShakesId, const glm::dvec3& at, u32 nowMs);
    void clear();
    u32 active() const { return m_count; }

    // The camera's offset (world yards) at nowMs for a camera at `eye` behind a unit facing `yaw` (radians
    // from +X towards +Y). Drops the shakes that ended.
    glm::vec3 sample(u32 nowMs, const glm::dvec3& eye, f32 yaw);

    // Development (VD_CAMERA_SHAKE=0): nothing moves (shakes are still kept, so switching back shows the
    // rest).
    bool enabled = true;

private:
    struct Active {
        game::CameraShake shake;
        glm::dvec3 at{0.0};
        u32 startMs = 0;
    };
    std::array<Active, kMaxShakes> m_shakes{};
    u32 m_count = 0;
};

// One shake's offset along its direction at time t (seconds since it started, before its phase) and amplitude
// (already attenuated by distance). Public for the tests.
f32 cameraShakeValue(const game::CameraShake& shake, f32 t, f32 amplitude);
// The amplitude left at `distance` yards (the 1.12 falloff).
f32 cameraShakeFalloff(f32 amplitude, f32 distance);
// A footstep event of a unit's model (pack::actorEventId of "$FL0", "$BR1", "$SL0", "$RR0", "$WL2"...):
// WoW.exe's unit event switch (0x5FFBE0) sends them to its footstep (0x5FBF70), where CreatureModelData's
// shake starts.
bool footstepEvent(u32 eventId);

} // namespace vd
