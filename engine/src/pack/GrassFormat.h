#pragma once

#include "core/Types.h"

namespace vd::pack {

// Phase 14 (grass): the terrain's detail doodads, the 1.12 client's "ground effects" (shared by the baker and the
// runtime). One pack for every map, grass.vdpk, kept apart from the world packs so adding it costs the phones a few
// MB instead of re-pushing the terrain:
//   grass/models               GrassModels  GrassModelsHeader: the detail doodad models (World\NoDXT\Detail\*.m2,
//                                           static: no bones, one texture each) and the ground effect table
//                                           (GroundEffectTexture.dbc rows the maps use)
//   grass/<map>/<tx>_<ty>      GrassTile    GrassTileHeader + GrassChunk[256] + shadow bits (LZ4); only tiles with
//                                           at least one chunk that grows grass
// The placement itself is not baked: the runtime runs WoW.exe's own generator over these records
// (world/GrassPlacement.h), so the grass lands where 1.12 puts it for any density.
//
// How 1.12 picks a cell's grass (CMapChunk::CreateDetailDoodads, WoW.exe 0x6BFC10): the chunk's 8x8 cells each
// belong to one texture layer (MCNK's low-quality texture map, 2 bits per cell); that layer's MCLY effectId is a
// GroundEffectTexture row with four GroundEffectDoodad references and a density; MCNK's "no effect doodad" bits and
// the holes switch cells off.
constexpr u32 kGrassFormatVersion = 1;
constexpr const char* kGrassPackFileName = "grass.vdpk";
constexpr const char* kGrassModelsAsset = "grass/models";

constexpr u16 kNoGrassModel = 0xFFFF;  // a GroundEffectTexture slot with no doodad (-1 in the DBC)
constexpr u16 kNoGrassEffect = 0xFFFF; // a layer without ground effect
constexpr u16 kNoGrassShadow = 0xFFFF;

// GrassModel::flags.
constexpr u16 kGrassModelClamp = 1u << 0; // its texture does not wrap (M2 texture flags 0): clamp the sampler

// One detail doodad model: its view-0 triangles over its own vertices, drawn with its first texture only (WoW.exe
// CDetailDoodadData: the materials and blend modes of the M2 are ignored, every detail doodad is alpha-keyed).
struct GrassModel {
    u64 texture;      // assetPathHash of the texture in textures.vdpk
    u32 firstIndex;   // into the index array
    u32 indexCount;   // the client's batch budget counts these (and vertexCount)
    u32 firstVertex;  // base vertex of its indices (model-local)
    u32 vertexCount;
    f32 height;       // top of its box (yards over its base), for culling
    u16 flags;        // kGrassModel*
    u16 reserved;
};
static_assert(sizeof(GrassModel) == 32, "GrassModel layout");

// GrassEffect::flags.
// The row names a doodad the client cannot load (GroundEffectDoodad has no such row, or its model is missing):
// WoW.exe waits for every doodad of every layer of a chunk before creating its grass, so such a chunk never gets any.
constexpr u16 kGrassEffectBroken = 1u << 0;

// A GroundEffectTexture.dbc row (1.12.1: ID, DoodadId[4] (GroundEffectDoodad's second column, not its ID), Density,
// TerrainType).
struct GrassEffect {
    u32 id;          // GroundEffectTexture ID
    u16 models[4];   // into GrassModel[], kNoGrassModel where the row has -1
    u16 density;     // doodads per chosen cell; the DBC's 0 already turned into WoW.exe's 8
    u16 flags;       // kGrassEffect*
};
static_assert(sizeof(GrassEffect) == 16, "GrassEffect layout");

// Vertex of a detail doodad, model space (Z up, the model's base at 0).
struct GrassVertex {
    f32 position[3];
    f32 uv[2];
};
static_assert(sizeof(GrassVertex) == 20, "GrassVertex layout");

// Blob layout: header, GrassModel[modelCount] (sorted by texture), GrassEffect[effectCount], GrassVertex[vertexCount],
// u16 indices[indexCount]. Sections 16-byte aligned.
struct GrassModelsHeader {
    u32 version; // kGrassFormatVersion
    u32 modelCount;
    u32 effectCount;
    u32 vertexCount;
    u32 indexCount;
    u32 modelsOffset;
    u32 effectsOffset;
    u32 verticesOffset;
    u32 indicesOffset;
    u32 reserved[3];
};
static_assert(sizeof(GrassModelsHeader) == 48, "GrassModelsHeader layout");

// GrassChunk::flags.
constexpr u16 kGrassChunkGrows = 1u << 0; // some cell's layer has a ground effect and every doodad of the chunk loads

// One terrain chunk (MCNK), in the tile's chunk order (row * 16 + column, as pack/TerrainFormat.h).
struct GrassChunk {
    u16 effects[4];    // per MCLY layer: index into GrassEffect[], kNoGrassEffect = none
    u16 cellLayers[8]; // MCNK low-quality texture map: per cell row, 2 bits per column (column 0 lowest) = the layer
    u8 noEffect[8];    // MCNK "no effect doodad": per cell row, bit per column = no grass in that cell
    u16 holes;         // MCNK 4x4 hole mask (bit (row / 2) * 4 + column / 2)
    u16 flags;         // kGrassChunk*
    u16 shadow;        // its MCSH bits in the tile's shadow array, kNoGrassShadow = none
    u16 reserved;
};
static_assert(sizeof(GrassChunk) == 40, "GrassChunk layout");

// MCSH as the client reads it for the grass: 64 rows of 64 bits (8 bytes, least significant bit first), rows along
// the chunk's t axis, without the edge fix the terrain's alpha gets.
constexpr u32 kGrassShadowBytes = 512;

// Blob layout: header, GrassChunk[256], u8 shadows[shadowCount][kGrassShadowBytes]. Sections 16-byte aligned.
struct GrassTileHeader {
    u32 version; // kGrassFormatVersion
    u16 tileX;
    u16 tileY;
    u32 chunksOffset;
    u32 shadowCount;
    u32 shadowsOffset;
    u32 reserved[3];
};
static_assert(sizeof(GrassTileHeader) == 32, "GrassTileHeader layout");

// "grass/<map>/<tx>_<ty>": the map's Map.dbc directory in lowercase. Written into `out` (at least 96 chars) without
// allocating; returns its length.
inline u32 grassTilePath(char* out, u32 capacity, const char* mapLower, u32 tx, u32 ty) {
    u32 n = 0;
    const auto put = [&](char c) {
        if (n + 1 < capacity) {
            out[n++] = c;
        }
    };
    for (const char* p = "grass/"; *p; ++p) {
        put(*p);
    }
    for (const char* p = mapLower; *p; ++p) {
        put(*p);
    }
    put('/');
    const auto number = [&](u32 v) {
        char digits[10];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0 && count < 10);
        while (count > 0) {
            put(digits[--count]);
        }
    };
    number(tx);
    put('_');
    number(ty);
    out[n] = '\0';
    return n;
}

} // namespace vd::pack
