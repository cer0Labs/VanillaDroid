#version 320 es
#extension GL_GOOGLE_include_directive : require

// Static models (M2 doodads, WMO groups), instanced: per-instance model -> world transform as three
// rows of a 3x4 matrix, already relative to the camera (translation = position - camera).

#include "frame.glsl"
#include "model_push.glsl"
#include "interior.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal; // snorm8
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;  // unorm8: WMO vertex colors (baked interior light)
layout(location = 4) in vec4 inRow0;   // per instance
layout(location = 5) in vec4 inRow1;
layout(location = 6) in vec4 inRow2;
layout(location = 7) in uvec2 inLight; // phase 11: the interior light the instance carries
layout(location = 8) in float inFade;  // phase 14: its opacity in the doodads' distance fade (1: none)

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec3 vNormal;
layout(location = 2) out mediump vec4 vColor;
layout(location = 3) out mediump float vFog;
layout(location = 6) out mediump vec4 vInstanceLight;

void main() {
    vec4 p = vec4(inPosition, 1.0);
    vec3 position = vec3(dot(inRow0, p), dot(inRow1, p), dot(inRow2, p));
    gl_Position = frame.viewProj * vec4(position, 1.0);
    // Uniform scale only: the rotation part transforms normals directly.
    vNormal = normalize(vec3(dot(inRow0.xyz, inNormal), dot(inRow1.xyz, inNormal), dot(inRow2.xyz, inNormal)));
    vUV = animatedUV(inUV);
    vColor = vec4(inColor.rgb, inColor.a * inFade); // phase 14: an M2's alpha (white) carries its fade
    vFog = fogFactor(position);
    vInstanceLight = instanceLight(inLight, vNormal) + vec4(selfLight(inPosition, inNormal, pc.selfLight, pc.selfLightColor), 0.0);
}
