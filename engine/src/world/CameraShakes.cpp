#include "world/CameraShakes.h"

#include "core/Profile.h"

#include <glm/geometric.hpp>

#include <cmath>

namespace vd {
namespace {

constexpr f32 kTwoPi = 6.28318531f;

} // namespace

f32 cameraShakeValue(const game::CameraShake& shake, f32 t, f32 amplitude) {
    // WoW.exe 0x511610: the phase counts in the time of both the sine and the decay.
    const f32 time = t + shake.phase;
    f32 v = amplitude * std::sin(time * shake.frequency * kTwoPi);
    if (shake.decays) {
        v *= std::exp(-time * shake.coefficient);
    }
    return v;
}

f32 cameraShakeFalloff(f32 amplitude, f32 distance) {
    // WoW.exe 0x5116E0 with 9 and 80 yards (squared at 0x50B2C0 / 0x50B2F0) and pow(0.7, ...).
    if (distance > CameraShakes::kMaxDistance) {
        return 0.0f;
    }
    if (distance <= CameraShakes::kFullDistance) {
        return amplitude;
    }
    constexpr f32 full = CameraShakes::kFullDistance;
    return amplitude * std::pow(CameraShakes::kFalloffBase, (distance - full) / full);
}

bool footstepEvent(u32 eventId) {
    const char kind = static_cast<char>((eventId >> 8) & 0xFF);
    const char side = static_cast<char>((eventId >> 16) & 0xFF);
    const char digit = static_cast<char>(eventId >> 24);
    return (eventId & 0xFF) == '$' &&
           (kind == 'F' || kind == 'B' || kind == 'S' || kind == 'R' || kind == 'W') &&
           (side == 'L' || side == 'R') && digit >= '0' && digit <= '9';
}

void CameraShakes::add(const game::CameraShake& shake, const glm::dvec3& at, u32 nowMs) {
    if (shake.amplitude == 0.0f || shake.duration <= shake.phase) {
        return;
    }
    u32 slot = m_count;
    if (m_count == kMaxShakes) {
        // Full: the one that started first goes.
        slot = 0;
        for (u32 i = 1; i < m_count; ++i) {
            if (static_cast<i32>(m_shakes[i].startMs - m_shakes[slot].startMs) < 0) {
                slot = i;
            }
        }
    } else {
        ++m_count;
    }
    m_shakes[slot] = {shake, at, nowMs};
}

void CameraShakes::addEffect(const game::SpellKitFx& fx, u32 effectShakesId, const glm::dvec3& at,
                             u32 nowMs) {
    for (u32 id : fx.effectShakes(effectShakesId)) {
        if (const game::CameraShake* shake = fx.cameraShake(id)) {
            add(*shake, at, nowMs);
        }
    }
}

void CameraShakes::clear() {
    m_count = 0;
}

glm::vec3 CameraShakes::sample(u32 nowMs, const glm::dvec3& eye, f32 yaw) {
    VD_PROFILE_ZONE("CameraShakes::sample");
    // The strongest (attenuated) shake of each direction, and its time.
    std::array<f32, 3> amplitude{};
    std::array<f32, 3> time{};
    std::array<game::CameraShake, 3> winner{};
    std::array<bool, 3> found{};
    for (u32 i = 0; i < m_count;) {
        const Active& a = m_shakes[i];
        const f32 t = static_cast<f32>(nowMs - a.startMs) * 0.001f;
        if (t + a.shake.phase >= a.shake.duration) {
            m_shakes[i] = m_shakes[--m_count]; // over
            continue;
        }
        ++i;
        const f32 distance = static_cast<f32>(glm::length(eye - a.at));
        const f32 attenuated = cameraShakeFalloff(a.shake.amplitude, distance);
        const u32 d = static_cast<u32>(a.shake.direction);
        if (attenuated > amplitude[d]) {
            amplitude[d] = attenuated;
            time[d] = t;
            winner[d] = a.shake;
            found[d] = true;
        }
    }
    glm::vec3 offset(0.0f);
    if (!enabled) {
        return offset;
    }
    for (u32 d = 0; d < 3; ++d) {
        if (!found[d]) {
            continue;
        }
        const f32 v = cameraShakeValue(winner[d], time[d], amplitude[d]);
        switch (winner[d].direction) {
        case game::CameraShake::Direction::Forward:
            offset += glm::vec3(std::cos(yaw), std::sin(yaw), 0.0f) * v;
            break;
        case game::CameraShake::Direction::Side:
            offset += glm::vec3(std::cos(yaw + 1.57079633f), std::sin(yaw + 1.57079633f), 0.0f) * v;
            break;
        case game::CameraShake::Direction::Up: offset.z += v; break;
        }
    }
    return offset;
}

} // namespace vd
