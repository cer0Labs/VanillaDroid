#version 320 es
#extension GL_GOOGLE_include_directive : require

// Terrain tile: one draw per tile (pack/TerrainFormat.h). Vertex positions are implicit: the index
// buffer holds tile vertex numbers (chunk * 145 + v) and the tile-local (s, t) position is rebuilt from
// gl_VertexIndex; the vertex buffer only carries height and normal.
// Positions are camera-relative (origin = tile north-west corner - camera) to keep float precision.

#include "frame.glsl"
#include "surface.glsl"

struct Chunk {
    uint layers;     // 4 x u8 texture array layers, 0xFF = unused
    uint layerFlags; // 4 x u8 MCLY flags
    float minZ;
    float maxZ;
    uint firstIndex;
    uint indexCount;
    uint areaId;
    uint flags;
};
layout(std430, set = 2, binding = 1) readonly buffer Chunks {
    Chunk chunks[256];
};

layout(push_constant) uniform Push {
    vec4 origin; // xyz: tile corner - camera
} pc;

layout(location = 0) in float inHeight;
layout(location = 1) in vec3 inNormal; // snorm8, world axes

layout(location = 0) out highp vec2 vChunkUV;   // 0..1 across the chunk
layout(location = 1) flat out highp vec2 vAlphaBase; // chunk corner in the tile's alpha texture
layout(location = 2) flat out uvec4 vLayers;
layout(location = 3) out mediump vec3 vNormal;
layout(location = 4) out mediump float vFog;
// Phase 11 (f11/interiors): the specular highlight (per vertex, like 1.12's secondary color) and the layers' scrolling.
layout(location = 8) out mediump vec3 vSpecular;
layout(location = 9) flat out highp vec4 vLayerOffset01;
layout(location = 10) flat out highp vec4 vLayerOffset23;
layout(location = 11) flat out uint vOverbright; // bit i: layer i glows (MCLY 0x80)

const float kUnitSize = 1600.0 / 3.0 / 128.0; // yards between outer vertices

void main() {
    uint index = uint(gl_VertexIndex);
    uint chunk = index / 145u;
    uint v = index - chunk * 145u;
    uint row = v / 17u;
    uint k = v - row * 17u;
    // (column, row) inside the chunk, in vertex units: outer vertices first, then the 8 inner ones.
    vec2 local = k < 9u ? vec2(float(k), float(row)) : vec2(float(k - 9u) + 0.5, float(row) + 0.5);
    vec2 chunkXY = vec2(float(chunk & 15u), float(chunk >> 4u));
    vec2 st = (chunkXY * 8.0 + local) * kUnitSize;

    // world = (cornerX - t, cornerY - s, height), relative to the camera.
    vec3 position = vec3(pc.origin.x - st.y, pc.origin.y - st.x, pc.origin.z + inHeight);
    gl_Position = frame.viewProj * vec4(position, 1.0);

    vChunkUV = local * 0.125;
    vAlphaBase = chunkXY * (1.0 / 16.0);
    uint packed = chunks[chunk].layers;
    vLayers = uvec4(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu, packed >> 24u);
    vNormal = inNormal;
    vFog = fogFactor(position);
    vSpecular = surfaceSpecular(normalize(inNormal), position);
    uint layerFlags = chunks[chunk].layerFlags;
    vLayerOffset01 = vec4(terrainLayerOffset(layerFlags & 0xFFu), terrainLayerOffset((layerFlags >> 8u) & 0xFFu));
    vLayerOffset23 = vec4(terrainLayerOffset((layerFlags >> 16u) & 0xFFu), terrainLayerOffset(layerFlags >> 24u));
    vOverbright = ((layerFlags >> 7u) & 1u) | ((layerFlags >> 14u) & 2u) | ((layerFlags >> 21u) & 4u) |
                  ((layerFlags >> 28u) & 8u);
}
