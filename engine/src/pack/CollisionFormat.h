#pragma once

#include "core/Hash.h"
#include "core/Types.h"
#include "pack/TerrainFormat.h"

#include <string>
#include <string_view>

namespace vd::pack {

// Baked collision (shared by the baker and the runtime). One pack per map, collision_<map>.vdpk, with one
// asset per tile of the map grid that has anything to collide with:
//   world/maps/<map>/<map>_<tx>_<ty>.col  CollisionTile  CollisionTileHeader + sections (LZ4)
//
// Everything a character can stand on or bump into inside the tile's square, in world coordinates (see
// TerrainFormat.h for the axes):
//   - the terrain as its height grid (the same vertices and 4-triangle fans as the rendered terrain, rebuilt
//     on demand by the queries) and the ADT hole masks;
//   - the collision triangles of the buildings (WMO) and doodads (M2) placed on the map, WMO doodad sets
//     included. A triangle is kept by every tile its XY box overlaps, so a query only needs the tiles its
//     own box overlaps. Triangles are two-sided; a BVH over them in leaf order;
//   - the liquid surfaces (terrain MCLQ, WMO MLIQ) as triangles with their kind, in a second BVH.
//
// Which triangles collide follows vMaNGOS' vmap extractor (contrib/vmap_extractor): WMO triangles flagged
// collision (MOPY 0x08) or rendered and not "detail" (0x20 without 0x04), and every M2 bounding triangle.

constexpr u32 kCollisionFormatVersion = 2; // 2: 8-triangle leaves, 16-bit terrain heights

// Pack file of a map, from its Map.dbc directory: "Azeroth" -> "collision_azeroth.vdpk".
inline std::string collisionPackFileName(std::string_view mapDirectory) {
    std::string name = worldPackFileName(mapDirectory);
    return "collision_" + name.substr(6); // drop "world_"
}

// Asset path of a tile: "World\Maps\Azeroth\Azeroth_32_48.col" (hashed canonically like every asset).
inline std::string collisionTilePath(std::string_view mapDirectory, u32 tx, u32 ty) {
    std::string path = "World\\Maps\\";
    path.append(mapDirectory);
    path += '\\';
    path.append(mapDirectory);
    path += '_' + std::to_string(tx) + '_' + std::to_string(ty) + ".col";
    return path;
}

// Terrain heights: 1/128 yard steps over the chunk's lowest point (a chunk spans at most 512 yards).
constexpr f32 kCollisionHeightStep = 1.0f / 128.0f;

// CollisionTileHeader::flags.
constexpr u32 kCollisionTileHasTerrain = 1u << 0;

// Per-triangle flags.
constexpr u8 kCollisionNoCamera = 1u << 0; // WMO MOPY 0x02: the camera goes through it
constexpr u8 kCollisionDoodad = 1u << 1;   // from an M2 (else a WMO)

// BVH node. Children of an inner node: the next node, and `first`.
struct CollisionNode {
    f32 boundsMin[3];
    f32 boundsMax[3];
    u32 first; // leaf: first triangle; inner: index of the second child
    u32 count; // leaf: triangles (> 0); inner: 0
};
static_assert(sizeof(CollisionNode) == 32, "CollisionNode layout");

constexpr u32 kCollisionLeafTriangles = 8; // most triangles per BVH leaf
constexpr u32 kCollisionMaxDepth = 48;     // deepest BVH the baker writes (the runtime's traversal stack)

// Blob layout (every section 16-byte aligned):
//   f32 heightBases[kTileChunks]      lowest terrain height of each chunk, if HasTerrain
//   u16 heights[kTileVertices]        terrain heights above their chunk's base, in kCollisionHeightStep units
//                                     (TerrainFormat vertex order), if HasTerrain
//   u16 holes[kTileChunks]            ADT hole mask per chunk: bit (row / 2) * 4 + column / 2 of a quad
//   f32 vertices[vertexCount][3]
//   u32 indices[triangleCount][3]     in BVH leaf order
//   u8  triangleFlags[triangleCount]
//   CollisionNode nodes[nodeCount]    node 0 = root
//   f32 liquidVertices[liquidVertexCount][3]
//   u32 liquidIndices[liquidTriangleCount][3]
//   u8  liquidKinds[liquidTriangleCount]  LiquidKind
//   CollisionNode liquidNodes[liquidNodeCount]
struct CollisionTileHeader {
    u32 version; // kCollisionFormatVersion
    u16 tileX;
    u16 tileY;
    u32 flags;
    f32 boundsMin[3]; // everything in the tile: terrain, objects, liquids
    f32 boundsMax[3];
    u32 heightsOffset;
    u32 holesOffset;
    u32 vertexCount;
    u32 verticesOffset;
    u32 triangleCount;
    u32 indicesOffset;
    u32 triangleFlagsOffset;
    u32 nodeCount;
    u32 nodesOffset;
    u32 liquidVertexCount;
    u32 liquidVerticesOffset;
    u32 liquidTriangleCount;
    u32 liquidIndicesOffset;
    u32 liquidKindsOffset;
    u32 liquidNodeCount;
    u32 liquidNodesOffset;
    u32 heightBasesOffset;
    u32 reserved[6];
};
static_assert(sizeof(CollisionTileHeader) == 128, "CollisionTileHeader layout");

} // namespace vd::pack
