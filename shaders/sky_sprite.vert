#version 320 es
#extension GL_GOOGLE_include_directive : require

// Phase 11: a quad of the sky facing the screen (render/SkyRenderer: the sun, the moons, their glares), from
// gl_VertexIndex: corners center +- right +- up, camera-relative, on the far plane.

#include "frame.glsl"

layout(push_constant) uniform Push {
    vec4 center; // xyz: camera-relative (direction * celestial distance); w: 1 = cut and faded at the horizon
    vec4 right;  // xyz: half the quad along the screen's right
    vec4 up;     // xyz: half the quad along the screen's up
    vec4 color;  // RGB, A = opacity
} pc;

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out highp float vHeight; // above the camera, in units of the celestial distance / 12

const vec2 kCorners[6] = vec2[](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                vec2(1.0, -1.0));

void main() {
    vec2 corner = kCorners[gl_VertexIndex];
    vec3 position = pc.center.xyz + pc.right.xyz * corner.x + pc.up.xyz * corner.y;
    gl_Position = (frame.viewProj * vec4(position, 1.0)).xyww;
    vUV = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    vHeight = position.z;
}
