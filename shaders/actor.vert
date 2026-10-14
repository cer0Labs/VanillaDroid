#version 320 es
#extension GL_GOOGLE_include_directive : require

// Animated actors (characters, creatures, what they carry): bind-pose vertices skinned by up to four bones of
// the frame's bone palette, then placed by the instance's model -> world transform (camera-relative).

#include "frame.glsl"
#include "model_push.glsl"
#include "interior.glsl"

// 3x4 row-major bone matrices, three vec4 rows each. Must match anim::BoneMatrix.
layout(std430, set = 2, binding = 0) readonly buffer Bones {
    highp vec4 rows[];
} bones;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;  // snorm8
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inWeights; // unorm8, sum 1
layout(location = 4) in uvec4 inBones;
layout(location = 5) in vec4 inRow0;    // per instance
layout(location = 6) in vec4 inRow1;
layout(location = 7) in vec4 inRow2;
layout(location = 8) in uvec4 inExtra;  // x: first bone matrix of the instance, yz: its interior light (phase 11)

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec3 vNormal;
layout(location = 2) out mediump vec4 vColor;
layout(location = 3) out mediump float vFog;
layout(location = 6) out mediump vec4 vInstanceLight;

void main() {
    vec4 p = vec4(inPosition, 1.0);
    vec3 skinned = vec3(0.0);
    vec3 normal = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        uint m = (inExtra.x + inBones[i]) * 3u;
        vec4 r0 = bones.rows[m];
        vec4 r1 = bones.rows[m + 1u];
        vec4 r2 = bones.rows[m + 2u];
        float w = inWeights[i];
        skinned += w * vec3(dot(r0, p), dot(r1, p), dot(r2, p));
        normal += w * vec3(dot(r0.xyz, inNormal), dot(r1.xyz, inNormal), dot(r2.xyz, inNormal));
    }
    vec4 s = vec4(skinned, 1.0);
    vec3 position = vec3(dot(inRow0, s), dot(inRow1, s), dot(inRow2, s));
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vec3 n = vec3(dot(inRow0.xyz, normal), dot(inRow1.xyz, normal), dot(inRow2.xyz, normal));
    vNormal = dot(n, n) > 1e-8 ? normalize(n) : vec3(0.0, 0.0, 1.0);
    vUV = animatedUV(inUV);
    vColor = vec4(1.0);
    vFog = fogFactor(position);
    vInstanceLight = instanceLight(inExtra.yz, vNormal);
}
