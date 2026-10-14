#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace vd {

// The per-frame scene constants (set 0 of the world pipelines, internal to render/). Must match the Frame
// uniform block in shaders/frame.glsl (std140).
struct FrameUniforms {
    glm::mat4 viewProj;
    glm::vec4 lightDirection;
    glm::vec4 directColor;
    glm::vec4 ambientColor;
    glm::vec4 fogColor;
    glm::vec4 fogParams;
    glm::vec4 sky[5];
    glm::vec4 riverClose;
    glm::vec4 riverFar;
    glm::vec4 oceanClose;
    glm::vec4 oceanFar;
    glm::vec4 waterAlphas;
    // Phase 11.
    glm::vec4 shadowColor;
    glm::vec4 sunColor;      // w: celestial glow
    glm::vec4 sunHalo;       // w: cloud density
    glm::vec4 cloudEmissive; // w: LightParams.glow
    glm::vec4 cloudLayer1;   // w: weather intensity
    glm::vec4 cloudLayer2;   // w: time of day, 0..1 from midnight
    glm::vec4 conditions;    // x: under water, y: ghost (0 or 1); z: phase 15, a model frame (coverage alpha)
    glm::vec4 screenFx;      // shaders/screenfx.glsl: x = full-screen glow weight, y = the ghost's grading (0 or 1)
    glm::vec4 surfaceLight;  // phase 11 (f11/interiors): x: night glow 0..1, y: highlights on, z: terrain masks
};
static_assert(sizeof(FrameUniforms) == 448, "Frame uniform layout");

} // namespace vd
