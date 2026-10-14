#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Phase 11: the clouds of the 1.12 client. WoW.exe builds them on the CPU: octaves of value noise (values in
// -1..1, each octave half the amplitude of the last) mapped to 128 + 64 * n, cut by Hugo Elias' cloud cover
// (cover = 1 - sharpness^((n - threshold) * density), threshold = 255 * (1 - density), sharpness 0.96, density from
// LightFloatBand 3) and shaded with LightIntBand 12 (base), 11 (thinner parts) and 10 (the light of the sun or the
// moon on the side facing it). Here the noise is a tiling texture made at load (world/Sky), two layers drift over
// each other on a flat layer above the camera, and the slope towards the light comes from one more sample.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uNoise; // r, g: one field per layer, 128 + 64 * n

layout(push_constant) uniform Push {
    highp vec4 drift; // xy: offset of the first layer, zw: of the second
    vec4 shape;       // x: cover threshold (1 - density), y: sharpness, z: light strength, w: storm
    vec4 light;       // xyz: towards the sun or the moon lighting the clouds
    vec4 unused;
} pc;

layout(location = 0) in highp vec3 vDirection;

layout(location = 0) out vec4 outColor;

const highp float kScale1 = 0.30;  // texture tiles per unit of the layer (the layer is at height 1)
const highp float kScale2 = 0.47;
const highp float kLightStep = 0.08; // layer units towards the light for the shading sample
const float kLightGain = 4.0;

float field(highp vec2 uv1, highp vec2 uv2) {
    return texture(uNoise, uv1).r * 0.6 + texture(uNoise, uv2).g * 0.4;
}

void main() {
    highp vec3 direction = normalize(vDirection);
    highp float height = max(direction.z, 0.04);
    highp vec2 layer = direction.xy / height;
    highp vec2 uv1 = layer * kScale1 + pc.drift.xy;
    highp vec2 uv2 = layer * kScale2 + pc.drift.zw;
    float n = field(uv1, uv2);

    float density = 1.0 - pc.shape.x;
    float excess = max(n - pc.shape.x, 0.0) * 255.0 * density;
    float cover = 1.0 - pow(pc.shape.y, excess);

    // Lambert on the cover seen as a height field, as WoW.exe shades its texels: the light's height plus the slope
    // towards it (one sample ahead along the light's horizontal direction).
    highp vec2 towardsLight = normalize(pc.light.xy + vec2(1e-4, 0.0)) * kLightStep;
    float ahead = field(uv1 + towardsLight * kScale1, uv2 + towardsLight * kScale2);
    float aheadExcess = max(ahead - pc.shape.x, 0.0) * 255.0 * density;
    float slope = (1.0 - pow(pc.shape.y, aheadExcess)) - cover; // > 0: rising towards the light
    float lit = clamp(pc.light.z - slope * length(pc.light.xy) * kLightGain, 0.0, 1.0) * pc.shape.z;

    vec3 color = frame.cloudLayer1.rgb + frame.cloudEmissive.rgb * (0.75 - 0.5 * cover) + frame.sunHalo.rgb * lit;
    float horizon = smoothstep(0.0, 0.3, direction.z); // thinning into the distance, as the dome's edge
    outColor = vec4(screenFx(min(color, vec3(1.0))), cover * horizon); // and the client's full-screen effects
}
