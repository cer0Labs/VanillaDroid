#pragma once

#include "core/Types.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>

namespace vd {

// Phase 11: the 1.12 client's full-screen effects (FFXEffects.cpp), which every world shader applies to its color
// (shaders/screenfx.glsl). This is their CPU side: which are on, and the same formula for the colors the CPU fills
// with (the clear color) and for the tests.
//
// Measured in WoW.exe 1.12.1: both final passes take a vertex color built from the current light's LightParams.glow
// (alpha = glow * 255); the glow pass's RGB is how much of the blurred image replaces the scene (a third, more when
// drunk), the death pass's RGB is the constant 0x5393A8. CVars ffxGlow and ffxDeath, both "1" by default.
struct ScreenFxParams {
    f32 glow = 0.0f;  // weight of glow^2 added to the scene (0: no glow)
    f32 death = 0.0f; // 1: the ghost's grading
};

constexpr glm::vec3 kScreenFxLuminance{0.299f, 0.587f, 0.144f}; // FFXDeath.bls (the blue weight is the client's)
constexpr glm::vec3 kScreenFxDeathTint{83.0f / 255.0f, 147.0f / 255.0f, 168.0f / 255.0f};

// `lightGlow`: LightParams.glow of the light in use (the death set's while a ghost). The death effect brings its own
// glow, so a ghost glows with the death effect on even when the full-screen glow is off.
inline ScreenFxParams screenFxParams(f32 lightGlow, bool ghost, bool fullScreenGlow, bool deathEffect) {
    ScreenFxParams p;
    const bool death = ghost && deathEffect;
    p.glow = fullScreenGlow || death ? std::max(lightGlow, 0.0f) : 0.0f;
    p.death = death ? 1.0f : 0.0f;
    return p;
}

// screenFx() of shaders/screenfx.glsl: a surface's color after the effects.
inline glm::vec3 applyScreenFx(glm::vec3 color, const ScreenFxParams& p) {
    color += p.glow * color * color;
    if (p.death > 0.0f) {
        const f32 l = std::clamp(glm::dot(color, kScreenFxLuminance), 0.0f, 1.0f);
        color = glm::vec3(l) + kScreenFxDeathTint * std::clamp(4.0f * l * (1.0f - l), 0.0f, 1.0f);
    }
    return color;
}

} // namespace vd
