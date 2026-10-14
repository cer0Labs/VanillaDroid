#version 320 es
#extension GL_GOOGLE_include_directive : require

// Phase 11: a sky model around the camera (render/SkyRenderer: the stars, a skybox), turned by its root bone and
// put on the far plane: it stands behind everything whatever its size.

#include "frame.glsl"

layout(push_constant) uniform Push {
    vec4 rotation0; // model -> world rotation, rows
    vec4 rotation1;
    vec4 rotation2;
    vec4 params; // x: opacity, y: 1 = the stars (hidden behind the moons' discs)
    vec4 mask0;  // the stars: a moon's disc, xyz = unit direction, w = cosine of its angular radius (0 = none)
    vec4 mask1;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor; // unorm8: alpha = the batch's opacity

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump float vAlpha;
layout(location = 2) out highp vec3 vDirection; // from the camera

void main() {
    vec3 position = vec3(dot(pc.rotation0.xyz, inPosition), dot(pc.rotation1.xyz, inPosition),
                         dot(pc.rotation2.xyz, inPosition));
    gl_Position = (frame.viewProj * vec4(position, 1.0)).xyww;
    vUV = inUV;
    vAlpha = inColor.a * pc.params.x;
    vDirection = position;
}
