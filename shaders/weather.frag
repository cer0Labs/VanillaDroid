#version 320 es
#extension GL_GOOGLE_include_directive : require
precision mediump float;

// Weather (phase 11): the texture's cell times the vertex color, cut to the cell (the triangles that hold a sprite
// reach past it), fogged like the particles. No discard: blending on the tile is cheap.

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 2, binding = 0) uniform mediump sampler2D uTexture;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) flat in highp vec4 vCell;
layout(location = 2) in vec4 vColor;
layout(location = 3) in float vFog;
layout(location = 4) in highp vec3 vMask; // xy: where in the sprite (-1..1); z: 1 = round it off (soft textures
                                          // whose grain reaches the square's edges)

layout(location = 0) out vec4 outColor;

void main() {
    highp vec2 inside = step(vec2(0.0), vUV) * step(vUV, vec2(1.0));
    vec4 color = texture(uTexture, vCell.xy + clamp(vUV, 0.0, 1.0) * vCell.zw) * vColor;
    color.a *= inside.x * inside.y * mix(1.0, 1.0 - smoothstep(0.6, 1.0, length(vMask.xy)), vMask.z);
    color.rgb = mix(color.rgb, frame.fogColor.rgb, vFog);
    color.rgb = screenFx(color.rgb); // phase 11: the client's full-screen effects
    outColor = color;
}
