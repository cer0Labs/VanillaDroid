#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/ChunkReader.h"
#include "pack/TerrainFormat.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace vd::formats {

// Parsers for the 1.12 terrain files (WDT, ADT version 18). PC only: the baker turns them into
// TerrainTile blobs (pack/TerrainFormat.h), the runtime never sees these structures.
//
// Axes and tile/chunk/vertex addressing are described in pack/TerrainFormat.h (verified against the
// data by `vd_baker scan-terrain`: MCNR normals match the MCVT slopes only with those axes).

using pack::kChunkSize;
using pack::kChunkVertices; // 9x9 outer + 8x8 inner, interleaved in 17 rows (9, 8, 9, ...)
using pack::kMapHalfSize;
using pack::kMapTiles;
using pack::kTileSize;
using pack::kUnitSize;
constexpr u32 kChunksPerTile = pack::kTileChunks;
constexpr u32 kAlphaSize = 64; // alpha/shadow map texels per chunk side


// --- WDT ------------------------------------------------------------------------------------------

struct WdtWmoPlacement {
    u32 nameId = 0;
    u32 uniqueId = 0;
    f32 position[3]{}; // placement axes, see AdtWmoPlacement; a WDT at (0, y, 0) is moved to the map center (vMaNGOS)
    f32 rotation[3]{};
    f32 extentsMin[3]{};
    f32 extentsMax[3]{};
    u16 flags = 0;
    u16 doodadSet = 0;
    u16 nameSet = 0; // phase 14: WMOAreaTable's NameSetID
};

struct Wdt {
    u32 flags = 0; // MPHD flags
    // 1 where Map_<tx>_<ty>.adt exists; index ty * 64 + tx.
    std::array<u8, kMapTiles * kMapTiles> tiles{};
    u32 tileCount = 0;
    // Maps made of a single WMO (most instances): no ADTs.
    std::string wmoName;
    bool hasWmoPlacement = false;
    WdtWmoPlacement wmoPlacement;

    bool hasTile(u32 tx, u32 ty) const { return tx < kMapTiles && ty < kMapTiles && tiles[ty * kMapTiles + tx]; }
};

Result<Wdt> parseWdt(std::span<const u8> data);

// --- WDL: low-resolution heights of the whole map (distant horizon) --------------------------------

constexpr u32 kWdlOuter = 17; // 17x17 heights at the chunk corners of a tile
constexpr u32 kWdlInner = 16; // then 16x16 at the chunk centers
constexpr u32 kWdlSamples = kWdlOuter * kWdlOuter + kWdlInner * kWdlInner;

struct Wdl {
    // Per tile (index ty * 64 + tx): absolute heights in yards, rows along -X like the ADT (row 0 = the
    // tile's north edge, column 0 = its west edge). Empty for tiles without data.
    std::array<std::vector<i16>, kMapTiles * kMapTiles> tiles;
    u32 tileCount = 0;
};

Result<Wdl> parseWdl(std::span<const u8> data);

// --- ADT ------------------------------------------------------------------------------------------

// MCNK flags.
constexpr u32 kChunkHasShadow = 0x1;
constexpr u32 kChunkImpassable = 0x2;
constexpr u32 kChunkLiquidRiver = 0x4;
constexpr u32 kChunkLiquidOcean = 0x8;
constexpr u32 kChunkLiquidMagma = 0x10;
constexpr u32 kChunkLiquidSlime = 0x20;

// MCLY flags.
constexpr u32 kLayerAnimationMask = 0x7F; // rotation (0x7), speed (0x38), enabled (0x40)
constexpr u32 kLayerOverbright = 0x80;
constexpr u32 kLayerUseAlpha = 0x100;

struct AdtLayer {
    u32 textureId = 0; // index into Adt::textures
    u32 flags = 0;
    u32 alphaOffset = 0; // into MCAL
    u32 effectId = 0;    // GroundEffectTexture.dbc (grass doodads)
};

enum class LiquidType : u8 { River, Ocean, Magma, Slime };

// One MCLQ instance: a 9x9 height grid over the chunk and an 8x8 per-quad mask.
struct AdtLiquid {
    LiquidType type = LiquidType::River;
    f32 minHeight = 0.0f;
    f32 maxHeight = 0.0f;
    std::array<f32, 81> heights{};
    std::array<u8, 81> depth{}; // water/ocean: depth; magma/slime: 0
    // Low nibble: 0xF = quad not rendered. 0x40 / 0x80: fishable / fatigue flags.
    std::array<u8, 64> tiles{};
};

struct AdtChunk {
    u32 flags = 0;
    u32 indexX = 0; // cx
    u32 indexY = 0; // cy
    u32 areaId = 0;
    u16 holes = 0;  // 4x4 mask, bit (row * 4 + col) removes a 2x2 block of quads
    // Phase 14 (grass): which layer owns each of the 8x8 cells (MCNK's low-quality texture map: per row a u16, 2 bits
    // per column, column 0 lowest) and the cells without detail doodads (a byte per row, bit per column).
    std::array<u16, 8> cellLayers{};
    std::array<u8, 8> noEffectDoodad{};
    f32 position[3]{}; // world X, Y, Z of the north-west corner; heights are relative to Z
    std::array<f32, kChunkVertices> heights{};
    std::array<std::array<i8, 3>, kChunkVertices> normals{}; // world X, Y, Z; 127 = 1.0
    u32 layerCount = 0;
    std::array<AdtLayer, 4> layers{};
    // Layer 1..3 alpha, 8-bit (the file's 4-bit values * 17), row-major [row][column], with the
    // client's edge fix applied (last row/column copied from the previous one).
    std::array<std::array<u8, kAlphaSize * kAlphaSize>, 3> alpha{};
    bool hasShadow = false;
    std::array<u8, kAlphaSize * kAlphaSize> shadow{}; // 1 = in shadow, same layout and fix as alpha
    // Phase 14 (grass): MCSH as stored (64 rows of 8 bytes, least significant bit first, no edge fix): what the
    // client reads to darken the detail doodads in shadow. Zero without a shadow.
    std::array<u8, kAlphaSize * kAlphaSize / 8> shadowBits{};
    std::vector<u32> doodadRefs; // into Adt::doodads
    std::vector<u32> wmoRefs;    // into Adt::wmoPlacements
    std::vector<AdtLiquid> liquids;

    // Chunk-local vertex indices into heights/normals.
    static constexpr u32 outerVertex(u32 column, u32 row) { return row * 17 + column; }     // 0..8
    static constexpr u32 innerVertex(u32 column, u32 row) { return row * 17 + 9 + column; } // 0..7
    // Quad (0..7, 0..7) inside a hole.
    bool isHole(u32 column, u32 row) const { return (holes >> ((row / 2) * 4 + column / 2)) & 1u; }
};

// MDDF: M2 doodad placement. Position/rotation use the placement axes: (kMapHalfSize - Y, Z,
// kMapHalfSize - X) in world terms; rotation in degrees.
struct AdtDoodadPlacement {
    u32 nameId = 0; // into Adt::models
    u32 uniqueId = 0;
    f32 position[3]{};
    f32 rotation[3]{};
    u16 scale = 1024; // 1024 = 1.0
    u16 flags = 0;
};

// MODF: WMO placement, same axes as MDDF.
struct AdtWmoPlacement {
    u32 nameId = 0; // into Adt::wmos
    u32 uniqueId = 0;
    f32 position[3]{};
    f32 rotation[3]{};
    f32 extentsMin[3]{};
    f32 extentsMax[3]{};
    u16 flags = 0;
    u16 doodadSet = 0;
    u16 nameSet = 0;
};

struct Adt {
    std::vector<std::string> textures; // MTEX
    std::vector<std::string> models;   // MMDX, in MMID order
    std::vector<std::string> wmos;     // MWMO, in MWID order
    std::vector<AdtDoodadPlacement> doodads;
    std::vector<AdtWmoPlacement> wmoPlacements;
    std::vector<AdtChunk> chunks; // kChunksPerTile, index cy * 16 + cx

    const AdtChunk& chunk(u32 cx, u32 cy) const { return chunks[cy * 16 + cx]; }
};

Result<Adt> parseAdt(std::span<const u8> data);

// "World\Maps\Azeroth\Azeroth_31_49.adt" style paths.
std::string wdtPath(std::string_view mapDirectory);
std::string wdlPath(std::string_view mapDirectory);
std::string adtPath(std::string_view mapDirectory, u32 tx, u32 ty);

} // namespace vd::formats
