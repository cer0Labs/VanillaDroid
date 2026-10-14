#pragma once

#include "core/Types.h"
#include "pack/GrassFormat.h"

#include <span>

namespace vd::grass {

// Phase 14 (grass): where the 1.12 client puts the terrain's detail doodads, as WoW.exe does it (no Vulkan, no
// allocation: the runtime calls it as chunks come near, the tests and the baker's census call it too).
//
// CMapChunk::CreateDetailDoodads (WoW.exe 0x6BFC10), for a chunk whose ground effect models are all loaded:
//  1. The random generator is seeded with the chunk's indices on the map: ((ty * 16 + row) << 16) | (tx * 16 + col).
//  2. frillDensity times (the CVar "frillDensity", "Terrain frill density", 1..256, default 16) a cell of the 8x8 is
//     drawn (column = random & 7, row = next random & 7; the same cell may come up again).
//  3. For each drawn cell in that order: skipped if its "no effect doodad" bit is set, it lies in a hole, its layer has
//     no ground effect row; otherwise `density` doodads (the row's; 0 means 8): a random point of the cell (two
//     randoms), the doodad slot ((cell draw + doodad) & 3, an empty slot skips it after those two randoms), a random
//     scale 0.9..1.1 and a random turn about Z; its height is the terrain triangle under it (the quad's fan of four
//     around its center vertex) and its normal that triangle's, and it is shaded (vertex color 0xC0C0C0) where the
//     chunk's MCSH bit under it is set.
//  4. The doodads go into at most 4 batches of one texture each, of at most min(frillDensity * 64, 8192) vertices
//     and as many indices (CDetailDoodadInst::Add, 0x6B2920): what does not fit is dropped.
// Rendering (0x6B2B80): alpha tested at 128 (the CVar detailDoodadAlpha), lit like the terrain with that normal,
// two-sided, never animated (the vertices are baked into the chunk's buffer), faded out with the view depth from
// 52.5 to 70 yards by a texture ramp multiplied into the alpha. Chunks get them while their bounding sphere is
// nearer than 70 yards in depth.
constexpr u32 kDefaultFrillDensity = 16;
constexpr u32 kMaxFrillDensity = 256;
constexpr u32 kCellsPerSide = 8;
constexpr f32 kCellSize = 4.1666665f;  // 100 / 24 yards, the float WoW.exe uses
constexpr f32 kFadeStart = 52.5f;      // 0.75 * 70: the ramp's first texel (opaque)
constexpr f32 kFadeEnd = 70.0f;        // the ramp's last texel (transparent); also the chunk range
constexpr f32 kAlphaRef = 128.0f / 255.0f;
constexpr f32 kShadowShade = 192.0f / 255.0f; // the vertex color of doodads in shadow
constexpr u32 kMaxBatches = 4;

// WoW.exe's CRndSeed (SetSeed 0x44EA80, Next 0x4531E0): an additive generator over four lagged tables of 47, 53, 59
// and 61 words of one constant table.
class ClientRandom {
public:
    explicit ClientRandom(u32 seed = 0) { setSeed(seed); }
    void setSeed(u32 seed);
    u32 next();
    // The float WoW.exe makes of a random word: the low 23 bits as a mantissa in [1, 2), mirrored by the sign bit
    // into [-1, 1].
    static f32 toSigned(u32 random);

private:
    u32 m_sum = 0;
    u32 m_indices = 0; // byte offsets into the table, one per lag (high byte: the 47-word one)
};

constexpr u32 chunkSeed(u32 tx, u32 ty, u32 column, u32 row) {
    return ((ty * 16 + row) << 16) | (tx * 16 + column);
}

// One placed doodad. Positions are relative to the chunk's north-west corner along the world axes (x along +X, so
// -t; y along +Y, so -s; see pack/TerrainFormat.h), z absolute.
struct PlacedGrass {
    f32 position[3];
    f32 cosine; // its turn about Z...
    f32 sine;
    f32 scale;  // ...and its uniform scale
    f32 normal[3]; // the terrain triangle's, unit, pointing up
    u16 model;     // pack::GrassModel index
    u8 shaded;     // 1: in the chunk's shadow
    u8 batch;      // which of the chunk's texture batches holds it (0..3)
};

struct ChunkInput {
    const pack::GrassChunk* chunk = nullptr;
    const u8* shadowBits = nullptr; // kGrassShadowBytes, or null: no shadow
    const f32* heights = nullptr;   // the chunk's 145 terrain heights (absolute), MCVT order
    u32 seed = 0;                   // chunkSeed()
};

struct PlacementStats {
    u32 cellsDrawn = 0;   // cells the generator drew (frillDensity)
    u32 cellsGrowing = 0; // of those, the ones that placed doodads
    u32 dropped = 0;      // doodads the batches had no room for
};

// The doodads 1.12 places on a chunk with `frillDensity`, into `out` (in the client's order); returns how many. Fewer
// than the client when `out` is too small. A chunk without kGrassChunkGrows places nothing.
u32 placeChunk(const ChunkInput& input, std::span<const pack::GrassEffect> effects,
               std::span<const pack::GrassModel> models, u32 frillDensity, std::span<PlacedGrass> out,
               PlacementStats* stats = nullptr);

// The most doodads placeChunk() can return for `frillDensity` with these effects (density 20 is the data's largest).
u32 maxPerChunk(u32 frillDensity, u32 maxDensity);

// The ramp the client multiplies into the alpha: 1 (252/255) up to kFadeStart of view depth, 0 at kFadeEnd.
f32 fadeAt(f32 depth, f32 fadeStart = kFadeStart, f32 fadeEnd = kFadeEnd);

} // namespace vd::grass
