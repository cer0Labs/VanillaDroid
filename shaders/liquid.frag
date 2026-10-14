#version 320 es
#extension GL_GOOGLE_include_directive : require

// River/ocean: Light.dbc close/far colors and shallow/deep alphas by depth, plus the animated glints of
// the XTextures frames (their RGB is black, the alpha holds the pattern). Magma/slime: opaque animated
// color textures.
precision mediump float;

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 1) uniform mediump sampler2DArray uLiquids;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) flat in uint vKind;
layout(location = 2) in float vDepth;
layout(location = 3) in float vFog;
layout(location = 8) in vec3 vSpecular; // phase 11

layout(location = 0) out vec4 outColor;

const float kFrames = 30.0;
const float kFramesPerSecond = 15.0;
// ocean0[_s]: the glints of the frames' alpha are (0.25 + the sun's highlight) (phase 11: 1.12's constant).
const float kGlint = 0.25;

void main() {
    highp float animation = floor(mod(frame.fogParams.z * kFramesPerSecond, kFrames));
    vec4 texel = texture(uLiquids, vec3(vUV, float(vKind) * kFrames + animation));
    float depth = clamp(vDepth, 0.0, 1.0);

    vec4 color;
    if (vKind == 0u) { // river / lake
        color.rgb = mix(frame.riverClose.rgb, frame.riverFar.rgb, depth) + texel.a * (kGlint + vSpecular);
        color.a = mix(frame.waterAlphas.x, frame.waterAlphas.y, depth);
    } else if (vKind == 1u) { // ocean
        color.rgb = mix(frame.oceanClose.rgb, frame.oceanFar.rgb, depth) + texel.a * (kGlint + vSpecular);
        color.a = mix(frame.waterAlphas.z, frame.waterAlphas.w, depth);
    } else if (vKind == 2u) { // magma glows on its own
        color = vec4(texel.rgb, 1.0);
    } else { // slime
        color = vec4(texel.rgb * (frame.ambientColor.rgb + frame.directColor.rgb * 0.5), 1.0);
    }
    outColor = vec4(mix(color.rgb, frame.fogColor.rgb, vFog), color.a);
    outColor.rgb = screenFx(outColor.rgb); // phase 11: the client's full-screen effects
}
