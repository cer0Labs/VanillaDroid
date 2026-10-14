#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Particles and ribbons (phase 9): texture cell times the vertex color, faded by the fog the way each blend needs.
// No discard: blending on the tile is cheap.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;

layout(push_constant) uniform Push {
    uvec4 params; // x: blend (pack::ModelBlend), y: kParticleDraw* flags
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vFog;

layout(location = 0) out vec4 outColor;

const uint kBlendAdd = 3u;
const uint kBlendMod = 4u;
const uint kBlendMod2x = 5u;

// Phase 14 (bodyfx): kParticleDrawShadow. A literal, not a const: a second mediump `4u` constant beside kBlendMod is
// the same SPIR-V id decorated RelaxedPrecision twice, which spirv-val rejects.
#define kShadow 4u

void main() {
    vec4 color = texture(uTexture, vUV) * vColor;
    uint blend = pc.params.x;
    if ((pc.params.y & kShadow) != 0u) {
        color.rgb = mix(vec3(1.0), color.rgb, vColor.a); // a blob shadow fades with its unit (Mod blend)
    }
    if (blend == kBlendAdd) {
        color.rgb *= 1.0 - vFog; // light fades out in the fog
    } else if (blend == kBlendMod) {
        color.rgb = mix(color.rgb, vec3(1.0), vFog);
    } else if (blend == kBlendMod2x) {
        color.rgb = mix(color.rgb, vec3(0.5), vFog);
    } else {
        color.rgb = mix(color.rgb, frame.fogColor.rgb, vFog);
    }
    outColor = color;
    outColor.rgb = screenFxBlend(outColor.rgb, blend); // phase 11: the client's full-screen effects
}
