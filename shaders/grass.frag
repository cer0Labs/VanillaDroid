#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Phase 14 (grass): the detail doodads' pixels as WoW.exe draws them (0x6B2B80): alpha blended, with the alpha
// reference at 128 (CVar detailDoodadAlpha) over texture alpha x depth ramp, depth written. Near, their soft edges blend
// over the ground; far, they first turn translucent and then thin out until the ramp hides them. Not sorted (neither
// are the client's): a blended edge may hide a blade behind it by a pixel.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;

layout(push_constant) uniform Push {
    highp vec4 fade; // x: fade end (yards of view depth), y: 1 / (end - start)
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) in vec3 vLight;
layout(location = 2) in float vFog;
layout(location = 3) in float vFade;

layout(location = 0) out vec4 outColor;

const float kAlphaRef = 128.0 / 255.0;

void main() {
    vec4 texel = texture(uTexture, vUV);
    float alpha = texel.a * vFade;
    if (alpha < kAlphaRef) {
        discard;
    }
    vec3 color = mix(texel.rgb * vLight, frame.fogColor.rgb, vFog);
    outColor = vec4(screenFx(color), alpha); // phase 11: the client's full-screen effects
}
