#version 320 es
#extension GL_GOOGLE_include_directive : require

// Phase 14: animated doodads (windmills, water wheels, banners, flickering halos), instanced like model.vert. Each
// vertex is skinned by up to four bones of its instance's pose in the frame's doodad palette (one pose per model and
// phase, world/DoodadAnimation.h); a billboard vertex (all weights zero, rigid on its billboard bone) is instead turned
// about the bone's pivot to face the camera, as 1.12 turns M2 billboard bones (the actors' rule: the bone's X against
// the view direction, Z as near the model's up as it can, its scale kept; about the model's Z only for cylindrical
// ones), here per vertex so no instance costs the CPU anything.

#include "frame.glsl"
#include "model_push.glsl"
#include "interior.glsl"

// 3x4 row-major matrices, three vec4 rows each (anim::BoneMatrix). A billboard bone's entry (billboardEntry() in
// world/DoodadAnimation.cpp): row 0 = its posed pivot and scale, row 1 = its bind pivot and kind (1 spherical, 2 about Z).
layout(std430, set = 2, binding = 0) readonly buffer Bones {
    highp vec4 rows[];
} bones;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal; // snorm8
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;  // unorm8: white in M2s
layout(location = 4) in vec4 inRow0;   // per instance
layout(location = 5) in vec4 inRow1;
layout(location = 6) in vec4 inRow2;
layout(location = 7) in uvec2 inLight;
layout(location = 8) in float inFade;
layout(location = 9) in uint inBoneBase;
layout(location = 10) in uvec4 inBones;  // pack::ModelSkinVertex
layout(location = 11) in vec4 inWeights; // unorm8, sum 1; all 0: a billboard vertex

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec3 vNormal;
layout(location = 2) out mediump vec4 vColor;
layout(location = 3) out mediump float vFog;
layout(location = 6) out mediump vec4 vInstanceLight;

highp vec3 toWorld(highp vec3 p) {
    highp vec4 h = vec4(p, 1.0);
    return vec3(dot(inRow0, h), dot(inRow1, h), dot(inRow2, h));
}

mediump vec3 rotateToWorld(mediump vec3 v) {
    return vec3(dot(inRow0.xyz, v), dot(inRow1.xyz, v), dot(inRow2.xyz, v));
}

void main() {
    highp vec3 position;  // world, camera-relative
    mediump vec3 normal;  // world
    highp vec3 posed;     // model space, for the model's own light
    mediump vec3 posedNormal;
    if (inWeights.x == 0.0) {
        uint m = (inBoneBase + inBones.x) * 3u;
        highp vec4 r0 = bones.rows[m];
        highp vec4 r1 = bones.rows[m + 1u];
        highp vec3 pivot = toWorld(r0.xyz);
        highp float scale = r0.w * length(vec3(inRow0.x, inRow1.x, inRow2.x));
        mediump vec3 up = normalize(vec3(inRow0.z, inRow1.z, inRow2.z)); // the model's Z in the world
        // The view direction is the clip w row of the camera-relative view-projection (no translation).
        mediump vec3 x = -normalize(vec3(frame.viewProj[0][3], frame.viewProj[1][3], frame.viewProj[2][3]));
        mediump vec3 z;
        if (r1.w > 1.5) { // cylindrical: turns about the model's Z only
            x -= up * dot(x, up);
            x = dot(x, x) > 1e-6 ? normalize(x) : vec3(up.z, 0.0, -up.x);
            z = up;
        } else {
            z = up - x * dot(x, up);
            z = dot(z, z) > 1e-6 ? normalize(z) : vec3(1.0, 0.0, 0.0);
        }
        mediump vec3 y = cross(z, x);
        highp vec3 offset = inPosition - r1.xyz;
        position = pivot + (x * offset.x + y * offset.y + z * offset.z) * scale;
        normal = x * inNormal.x + y * inNormal.y + z * inNormal.z;
        posed = r0.xyz + offset * r0.w;
        posedNormal = inNormal;
    } else {
        highp vec4 p = vec4(inPosition, 1.0);
        posed = vec3(0.0);
        posedNormal = vec3(0.0);
        for (int i = 0; i < 4; ++i) {
            uint m = (inBoneBase + inBones[i]) * 3u;
            highp vec4 r0 = bones.rows[m];
            highp vec4 r1 = bones.rows[m + 1u];
            highp vec4 r2 = bones.rows[m + 2u];
            float w = inWeights[i];
            posed += w * vec3(dot(r0, p), dot(r1, p), dot(r2, p));
            posedNormal += w * vec3(dot(r0.xyz, inNormal), dot(r1.xyz, inNormal), dot(r2.xyz, inNormal));
        }
        position = toWorld(posed);
        normal = rotateToWorld(posedNormal);
    }
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vNormal = dot(normal, normal) > 1e-8 ? normalize(normal) : vec3(0.0, 0.0, 1.0);
    vUV = animatedUV(inUV);
    vColor = vec4(inColor.rgb, inColor.a * inFade);
    vFog = fogFactor(position);
    mediump vec3 ownNormal = dot(posedNormal, posedNormal) > 1e-8 ? normalize(posedNormal) : vec3(0.0, 0.0, 1.0);
    vInstanceLight = instanceLight(inLight, vNormal) + vec4(selfLight(posed, ownNormal, pc.selfLight, pc.selfLightColor), 0.0);
}
