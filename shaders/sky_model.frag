#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Phase 11: a sky model's texture with its opacity; the pipeline blends it (opaque, alpha or added). No fog: the
// sky is past it. The stars are added over the dome but not where a moon's disc stands in front of them.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;

layout(push_constant) uniform Push {
    vec4 rotation0;
    vec4 rotation1;
    vec4 rotation2;
    vec4 params; // x: opacity, y: 1 = the stars
    highp vec4 mask0; // xyz: a moon's direction, w: cosine of its disc's angular radius (0 = none)
    highp vec4 mask1;
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) in float vAlpha;
layout(location = 2) in highp vec3 vDirection;

layout(location = 0) out vec4 outColor;

// 0 inside the disc, 1 outside, over the last 10% of its radius.
float outsideDisc(highp vec4 disc, highp vec3 direction) {
    if (disc.w <= 0.0) {
        return 1.0;
    }
    highp float edge = 1.0 - (1.0 - disc.w) * 0.81; // the cosine at 90% of the radius
    return 1.0 - smoothstep(disc.w, edge, dot(direction, disc.xyz));
}

void main() {
    vec4 color = texture(uTexture, vUV);
    float alpha = color.a * vAlpha;
    if (pc.params.y > 0.5) {
        highp vec3 direction = normalize(vDirection);
        alpha *= outsideDisc(pc.mask0, direction) * outsideDisc(pc.mask1, direction);
    }
    // The client's full-screen effects: the stars are added over the dome, the skyboxes cover it.
    vec3 graded = pc.params.y > 0.5 ? screenFxBlend(color.rgb, VD_SCREENFX_BLEND_ADD) : screenFx(color.rgb);
    outColor = vec4(graded, alpha);
}
