#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Phase 11: the sun, the moons and their glares, added over what is there (src * a + dst). The bodies are cut at
// the camera's horizontal plane and fade over its last 0.4 units above it, as the 1.12 client cuts and fades their
// quads' vertices. Under water they are not seen.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;

layout(push_constant) uniform Push {
    vec4 center; // w: 1 = cut and faded at the horizon
    vec4 right;
    vec4 up;
    vec4 color;
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) in highp float vHeight;

layout(location = 0) out vec4 outColor;

const float kHorizonFadeRate = 2.5; // celestial::kHorizonFadeRate

void main() {
    vec4 color = texture(uTexture, vUV) * pc.color;
    if (pc.center.w > 0.5) {
        color.a *= clamp(vHeight * kHorizonFadeRate, 0.0, 1.0);
    }
    color.a *= 1.0 - frame.conditions.x;
    color.rgb = screenFxBlend(color.rgb, VD_SCREENFX_BLEND_ADD); // phase 11: the client's full-screen effects
    outColor = color;
}
