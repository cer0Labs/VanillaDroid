#version 320 es
#extension GL_GOOGLE_include_directive : require

// Distant horizon (WDL): one instanced draw for every low-resolution tile past the loaded terrain.
// Sample v of a tile: outer (column, row) = row * 17 + column, inner = 289 + row * 16 + column, heights
// as i16 pairs in `heights` (pack/TerrainFormat.h, HorizonHeader).

#include "frame.glsl"

layout(std430, set = 1, binding = 0) readonly buffer Heights {
    uint heights[];
};

struct Instance {
    vec3 origin; // tile north-west corner - camera
    uint slot;   // tile in `heights`
};
layout(std430, set = 1, binding = 1) readonly buffer Instances {
    Instance instances[];
};

const float kCellSize = 1600.0 / 3.0 / 16.0; // one chunk
const uint kStrideWords = 273u;              // 546 i16 per tile

void main() {
    Instance instance = instances[gl_InstanceIndex];
    uint v = uint(gl_VertexIndex);
    vec2 cell;
    if (v < 289u) {
        uint row = v / 17u;
        cell = vec2(float(v - row * 17u), float(row));
    } else {
        uint i = v - 289u;
        uint row = i / 16u;
        cell = vec2(float(i - row * 16u), float(row)) + 0.5;
    }
    uint word = heights[instance.slot * kStrideWords + v / 2u];
    uint half16 = (v & 1u) == 0u ? (word & 0xFFFFu) : (word >> 16u);
    float height = float(int(half16 << 16u) >> 16); // sign-extend the i16

    vec2 st = cell * kCellSize;
    vec3 position = vec3(instance.origin.x - st.y, instance.origin.y - st.x, instance.origin.z + height);
    gl_Position = frame.viewProj * vec4(position, 1.0);
}
