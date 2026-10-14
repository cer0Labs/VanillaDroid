#version 320 es
#extension GL_GOOGLE_include_directive : require

// Liquid surfaces baked into WMOs (MLIQ): model vertices placed by the instance transform like the rest of
// the building, shaded by the terrain's liquid.frag. color.r holds the liquid kind, color.g the depth
// (pack/ModelFormat.h, kModelMaterialLiquid).

#include "frame.glsl"
#include "surface.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal; // unused: liquids are flat
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inRow0; // per instance
layout(location = 5) in vec4 inRow1;
layout(location = 6) in vec4 inRow2;

layout(location = 0) out highp vec2 vUV;
layout(location = 1) flat out uint vKind;
layout(location = 2) out mediump float vDepth;
layout(location = 3) out mediump float vFog;
layout(location = 8) out mediump vec3 vSpecular; // phase 11: the sun's highlight (TERRAIN_HIGHLIGHTS)

void main() {
    vec4 p = vec4(inPosition, 1.0);
    vec3 position = vec3(dot(inRow0, p), dot(inRow1, p), dot(inRow2, p));
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vUV = inUV;
    vKind = uint(inColor.r * 255.0 + 0.5);
    vDepth = inColor.g;
    vFog = fogFactor(position);
    vSpecular = surfaceSpecular(vec3(0.0, 0.0, 1.0), position);
}
