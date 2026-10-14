#version 320 es
#extension GL_GOOGLE_include_directive : require

// Liquid surfaces (MCLQ) of a terrain tile. Like the terrain, XY is implicit: `grid` = chunk * 81 +
// row * 9 + column on the 9x9 liquid grid of each chunk (pack/TerrainFormat.h, LiquidVertex).

#include "frame.glsl"
#include "surface.glsl"

layout(push_constant) uniform Push {
    vec4 origin; // xyz: tile corner - camera
} pc;

layout(location = 0) in float inHeight;
layout(location = 1) in uint inPacked; // grid | depth << 16 | kind << 24

layout(location = 0) out highp vec2 vUV;
layout(location = 1) flat out uint vKind;
layout(location = 2) out mediump float vDepth;
layout(location = 3) out mediump float vFog;
layout(location = 8) out mediump vec3 vSpecular; // phase 11: the sun's highlight (TERRAIN_HIGHLIGHTS)

const float kUnitSize = 1600.0 / 3.0 / 128.0;
const float kTextureRepeat = 0.5; // liquid textures repeat every 2 grid units (~8.3 yards)

void main() {
    uint grid = inPacked & 0xFFFFu;
    uint chunk = grid / 81u;
    uint v = grid - chunk * 81u;
    uint row = v / 9u;
    uint column = v - row * 9u;
    vec2 cell = vec2(float(chunk & 15u), float(chunk >> 4u)) * 8.0 + vec2(float(column), float(row));
    vec2 st = cell * kUnitSize;

    vec3 position = vec3(pc.origin.x - st.y, pc.origin.y - st.x, pc.origin.z + inHeight);
    gl_Position = frame.viewProj * vec4(position, 1.0);

    vUV = cell * kTextureRepeat;
    vKind = inPacked >> 24u;
    vDepth = float((inPacked >> 16u) & 0xFFu) * (1.0 / 64.0); // the byte is ~9 per yard: deep after ~7 yards
    vFog = fogFactor(position);
    vSpecular = surfaceSpecular(vec3(0.0, 0.0, 1.0), position);
}
