#version 320 es
#extension GL_GOOGLE_include_directive : require

// Phase 11: the clouds, over the part of the sky dome above the horizon (render/SkyRenderer), on the far plane.

#include "frame.glsl"
#include "sky_dome.glsl"

layout(location = 0) out highp vec3 vDirection; // from the camera, not unit length

void main() {
    int ring;
    vec3 position = skyDomeVertex(gl_VertexIndex, ring);
    gl_Position = (frame.viewProj * vec4(position * 10.0, 1.0)).xyww;
    vDirection = position;
}
