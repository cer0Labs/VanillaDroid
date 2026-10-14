#pragma once

#include "core/Hash.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace vd::pack {

// Baked terrain (shared by the baker and the runtime). One pack per map: world_<map>.vdpk with
//   world/maps/<map>/<map>.wdt            TerrainMap   TerrainMapHeader + tile table + shared indices
//   world/maps/<map>/<map>_<tx>_<ty>.adt  TerrainTile  TerrainTileHeader + chunks + vertices + alpha (LZ4)
//   world/maps/<map>/terraintextures      Texture      2D array: every terrain texture the map uses
//   world/maps/<map>/horizon              TerrainHorizon low-resolution heights of every tile (WDL)
//   world/liquidtextures                  Texture      2D array: animated liquid frames (kLiquidFrames per type);
//                                         since phase 12 in liquids.vdpk, shared by every map (older world packs
//                                         carry their own copy, which wins)
//   world/maps/<map>/objects             TerrainObjects  phase 12: the WDT's global WMO (most dungeons are one
//                                         building and no ADT): a TerrainTileHeader with only its wmos section,
//                                         handed to the TerrainListener as tile kMapObjectsTile while the map is open
// A map without ADTs has no tiles and a one-layer placeholder texture array; its horizon is optional.
//
// World axes (server coordinates): X = north, Y = west, Z = up. Tile (tx, ty) comes from the file name
// Map_<tx>_<ty>.adt; its north-west corner is at X = (32 - ty) * kTileSize, Y = (32 - tx) * kTileSize.
// Inside a tile, positions are measured from that corner along
//   s (towards -Y): chunk column cx, vertex column
//   t (towards -X): chunk row cy, vertex row
// so world = (cornerX - t, cornerY - s, height).
//
// Vertex positions are implicit: a tile has 256 chunks x 145 vertices in chunk order (cy * 16 + cx), and
// each chunk stores 17 rows alternating 9 outer and 8 inner vertices (the ADT MCVT order):
//   v = row * 17 + column            outer vertex, (s, t) = (column, row) * kUnitSize
//   v = row * 17 + 9 + column        inner vertex, (s, t) = (column + 0.5, row + 0.5) * kUnitSize
// The vertex shader rebuilds (s, t) from gl_VertexIndex; the buffer only carries height and normal.

constexpr u32 kTerrainFormatVersion = 4; // 2: liquid meshes, 3: LOD index levels, 4: object placements

// Pack file of a map, from its Map.dbc directory: "Stratholme Raid" -> "world_stratholme_raid.vdpk".
inline std::string worldPackFileName(std::string_view mapDirectory) {
    std::string name = "world_";
    for (char c : mapDirectory) {
        const char lower = normalizeAssetPathChar(c);
        name.push_back((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ? lower : '_');
    }
    return name + ".vdpk";
}

// Phase 12: the liquid frames every map uses, in one pack.
constexpr const char* kLiquidPackFileName = "liquids.vdpk";
constexpr const char* kLiquidTexturesAsset = "world/liquidtextures";

constexpr f32 kTileSize = 1600.0f / 3.0f; // 533.33 yards
constexpr f32 kChunkSize = kTileSize / 16.0f;
constexpr f32 kUnitSize = kChunkSize / 8.0f;
constexpr f32 kMapHalfSize = 32.0f * kTileSize;
constexpr u32 kMapTiles = 64;
constexpr u32 kMapObjectsTile = kMapTiles * kMapTiles; // TerrainListener tile of the map's global objects
constexpr u32 kTileChunks = 256;
constexpr u32 kChunkVertices = 145;
constexpr u32 kTileVertices = kTileChunks * kChunkVertices; // 37120: u16 indices are enough
constexpr u32 kChunkIndices = 8 * 8 * 4 * 3;                // 8x8 quads, 4 triangles around the inner vertex
constexpr u32 kTileIndices = kTileChunks * kChunkIndices;
constexpr u32 kTerrainAlphaSize = 1024;   // alpha/shadow texture per tile: 64 texels per chunk
// Largest raw tile blob (the baker refuses bigger ones; the runtime sizes its staging slots with it).
// Typical: ~2.4 MB; with holes (own LOD indices) and a lot of ocean up to ~3.3 MB.
constexpr u32 kMaxTerrainTileBytes = 4u << 20;
constexpr u32 kTerrainTextureSize = 256;  // every layer of the terrain texture array
constexpr u32 kTerrainTextureRepeat = 8;  // layer textures repeat 8 times per chunk
constexpr u8 kNoTerrainLayer = 0xFF;
constexpr u32 kLiquidGrid = 9;    // MCLQ: 9x9 heights per chunk, 8x8 quads
// Level of detail = index buffers over the same vertices. Level 0: every quad as a fan of 4 triangles
// around its inner vertex. Level L > 0: quads of kLodStep[L] x kLodStep[L] outer vertices, each a fan around
// the outer vertex at its center. The tile border keeps every outer vertex at every level, so tiles of
// different levels meet without cracks.
constexpr u32 kLodLevels = 4;
constexpr u32 kLodStep[kLodLevels] = {1, 2, 4, 8};

struct LodRange {
    u32 first; // into the index buffer
    u32 count;
};
constexpr u32 kLiquidFrames = 30; // animation frames per liquid type in the liquid texture array

// World position of a tile's north-west corner.
constexpr f32 tileCornerX(u32 ty) {
    return (32.0f - static_cast<f32>(ty)) * kTileSize;
}
constexpr f32 tileCornerY(u32 tx) {
    return (32.0f - static_cast<f32>(tx)) * kTileSize;
}

struct TerrainVertex {
    f32 height;   // absolute world Z
    i8 normal[4]; // world X, Y, Z as snorm8; w = 0
};
static_assert(sizeof(TerrainVertex) == 8, "TerrainVertex layout");

// Per chunk; also read by the terrain vertex shader (std430 SSBO, must stay 32 bytes).
struct TerrainChunk {
    u8 layers[4];     // index into the terrain texture array; kNoTerrainLayer = unused
    u8 layerFlags[4]; // MCLY flags, low byte: animation (0x7F), overbright (0x80)
    f32 minZ;
    f32 maxZ;
    u32 firstIndex; // this chunk's triangles in the tile's index buffer (holes removed)
    u32 indexCount;
    u32 areaId; // AreaTable.dbc
    u32 flags;  // MCNK flags (impassable, liquids...)
};
static_assert(sizeof(TerrainChunk) == 32, "TerrainChunk layout");

constexpr u32 kTerrainTileHasHoles = 1u << 0; // the tile carries its own index buffer

// Blob layout: header, TerrainChunk[256], TerrainVertex[kTileVertices], u16 indices (holes only),
// alpha texels. Sections are 64-byte aligned.
struct TerrainTileHeader {
    u32 version; // kTerrainFormatVersion
    u16 tileX;
    u16 tileY;
    u32 flags;
    f32 minZ;
    f32 maxZ;
    u32 chunksOffset;
    u32 verticesOffset;
    // 0 when the tile has no holes: use TerrainMapHeader's shared index buffer instead. Otherwise every
    // LOD level of this tile, back to back (see lod[]).
    u32 indicesOffset;
    u32 indexCount;
    // kTerrainAlphaSize^2 texels, VK_FORMAT_B4G4R4A4_UNORM_PACK16 (u16 = B<<12 | G<<8 | R<<4 | A):
    // R, G, B = alpha of layers 1..3, A = baked shadow (0 or 15). Rows advance along t, columns along s.
    u32 alphaOffset;
    u32 alphaSize;
    // Liquids (MCLQ): LiquidVertex[] and u16 indices; translucent (river, ocean) triangles first, then
    // opaque ones (magma, slime) from liquidOpaqueFirst. Zero counts when the tile has no liquid.
    u32 liquidVerticesOffset;
    u32 liquidVertexCount;
    u32 liquidIndicesOffset;
    u32 liquidIndexCount;
    u32 liquidOpaqueFirst;
    LodRange lod[kLodLevels]; // own indices only (flags & kTerrainTileHasHoles)
    // Objects placed on the tile (CPU-side data, not uploaded): TerrainDoodadInstance[], TerrainWmoInstance[].
    u32 doodadsOffset;
    u32 doodadCount;
    u32 wmosOffset;
    u32 wmoCount;
    u32 reserved[4];
};
static_assert(sizeof(TerrainTileHeader) == 128, "TerrainTileHeader layout");

// An M2 doodad placed on the tile (ADT MDDF). transform: model space -> world, 3x4 row-major (rows =
// world X, Y, Z), scale included.
struct TerrainDoodadInstance {
    u64 model; // assetPathHash of the model in models.vdpk
    f32 transform[12];
    u32 uniqueId;
    u32 reserved;
};
static_assert(sizeof(TerrainDoodadInstance) == 64, "TerrainDoodadInstance layout");

// A WMO placed on (or reaching into) the tile (ADT MODF). The same building appears in every tile it
// touches, with the same uniqueId: the runtime keeps one instance.
struct TerrainWmoInstance {
    u64 model;
    f32 transform[12];
    f32 boundsMin[3]; // world box (includes doodads sticking out)
    f32 boundsMax[3];
    u32 uniqueId;
    u16 doodadSet; // shown with the model's default set 0
    u16 reserved;
    u32 reserved2[2];
};
static_assert(sizeof(TerrainWmoInstance) == 96, "TerrainWmoInstance layout");

enum class LiquidKind : u8 { River = 0, Ocean = 1, Magma = 2, Slime = 3 };

// One liquid vertex. The XY position is implicit like the terrain's: grid = chunk * 81 + row * 9 + column,
// (s, t) = (cx * 8 + column, cy * 8 + row) * kUnitSize.
struct LiquidVertex {
    f32 height; // absolute world Z
    u16 grid;
    u8 depth; // water depth under this vertex, ~9 per yard (0 for magma/slime)
    u8 kind;  // LiquidKind
};
static_assert(sizeof(LiquidVertex) == 8, "LiquidVertex layout");

// Distant horizon: the client's WDL, 17x17 heights at the chunk corners of every tile plus 16x16 at the
// chunk centers, drawn in the fog color past the loaded terrain (the vanilla silhouettes).
constexpr u32 kHorizonOuter = 17;
constexpr u32 kHorizonInner = 16;
constexpr u32 kHorizonSamples = kHorizonOuter * kHorizonOuter + kHorizonInner * kHorizonInner; // 545
constexpr u32 kHorizonStride = kHorizonSamples + 1; // i16 per tile, padded to whole u32 words
constexpr u16 kNoHorizon = 0xFFFF;

// Blob layout: header, u16 slot[64 * 64] (index ty * 64 + tx, kNoHorizon = no data), i16 heights
// [tileCount][kHorizonStride] (sample v: outer (column, row) = row * 17 + column, inner = 289 + row * 16 +
// column; rows along -X from the tile's north edge), u16 indices of one tile mesh (counter-clockwise from
// above, fans around the inner samples).
struct HorizonHeader {
    u32 version; // kTerrainFormatVersion
    u32 tileCount;
    u32 slotsOffset;
    u32 heightsOffset;
    u32 indicesOffset;
    u32 indexCount;
    u32 reserved[2];
};
static_assert(sizeof(HorizonHeader) == 32, "HorizonHeader layout");

constexpr u16 kMapTileExists = 1u << 0;
constexpr u16 kMapTileHasHoles = 1u << 1;

struct TerrainMapTile {
    u16 flags;
    u16 reserved;
    f32 minZ; // height bounds, to cull tiles before they are loaded
    f32 maxZ;
};
static_assert(sizeof(TerrainMapTile) == 12, "TerrainMapTile layout");

// Blob layout: header, TerrainMapTile[64 * 64] (index ty * 64 + tx), u16 shared indices.
struct TerrainMapHeader {
    u32 version; // kTerrainFormatVersion
    u32 textureCount; // layers of the map's terrain texture array
    u32 tileCount;
    u32 tilesOffset;
    u32 sharedIndicesOffset; // index buffer of any tile without holes: every LOD level (lod[])
    u32 sharedIndexCount;
    LodRange lod[kLodLevels];
    u32 reserved[2];
};
static_assert(sizeof(TerrainMapHeader) == 64, "TerrainMapHeader layout");

} // namespace vd::pack
