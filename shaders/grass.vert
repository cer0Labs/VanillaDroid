#version 320 es
#extension GL_GOOGLE_include_directive : require

// Phase 14 (grass): the terrain's detail doodads, as WoW.exe draws them (CDetailDoodadInst, 0x6B2600): the model's
// vertices turned about Z and scaled by the instance (they never animate), lit per vertex like the terrain with the
// normal of the terrain triangle under it, darkened to 0xC0 where the chunk's shadow falls, fogged, and faded with the
// view depth by the client's alpha ramp (a 64-texel texture generated from the camera-space position: opaque up to
// 0.75 x 70 yards, transparent at 70). Instance layout: GrassInstance (render/Renderer.h).

#include "frame.glsl"

layout(push_constant) uniform Push {
    highp vec4 fade; // x: fade end (yards of view depth), y: 1 / (end - start)
} pc;

layout(location = 0) in vec3 inPosition; // model space
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inOrigin;   // per instance: its base, camera-relative
layout(location = 3) in vec2 inRotation; // cosine and sine of its turn, times its scale
layout(location = 4) in vec4 inNormal;   // snorm8: xyz normal, w light (1 lit, 0.75 in shadow)

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec3 vLight;
layout(location = 2) out mediump float vFog;
layout(location = 3) out mediump float vFade;

void main() {
    float scale = length(inRotation);
    vec3 position = inOrigin + vec3(inRotation.x * inPosition.x - inRotation.y * inPosition.y,
                                    inRotation.y * inPosition.x + inRotation.x * inPosition.y,
                                    inPosition.z * scale);
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vUV = inUV;
    float diffuse = max(dot(inNormal.xyz, frame.lightDirection.xyz), 0.0);
    // Saturated like the terrain (the client's fixed-function light with the vertex color as material).
    vLight = min(frame.ambientColor.rgb + frame.directColor.rgb * diffuse, vec3(1.0)) * inNormal.w;
    vFog = fogFactor(position);
    // The ramp's texels run from 252 down to 0 (WoW.exe 0x6B2320); w is the view depth.
    vFade = clamp((pc.fade.x - gl_Position.w) * pc.fade.y, 0.0, 1.0) * (252.0 / 255.0);
}
