#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/Adt.h"
#include "pack/TerrainFormat.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace vd::bake {

// Canonical path of a terrain texture -> layer in the map's terrain texture array.
using TerrainTextureIndex = std::unordered_map<std::string, u32>;

struct BakedTerrainTile {
    std::vector<u8> blob; // raw TerrainTile blob (the pack stores it LZ4-compressed)
    pack::TerrainMapTile summary{};
};

// Index buffer of one tile (u16, chunk order, triangles counter-clockwise seen from above), without the
// quads inside holes. adt == nullptr builds the shared buffer of a tile without holes. If `chunks` is
// given, each chunk's firstIndex/indexCount is filled in.
std::vector<u16> buildTerrainIndices(const formats::Adt* adt, pack::TerrainChunk* chunks);

// Coarser level (pack::kLodStep[level], level >= 1) over the same vertices, with a full-resolution tile
// border. Holes: at step 2 a quad is dropped if its hole block is set; at larger steps only if every
// block it covers is a hole (distant holes shrink rather than grow).
std::vector<u16> buildTerrainLodIndices(const formats::Adt* adt, u32 level);

// Every level back to back, with the range of each.
std::vector<u16> buildTerrainLodChain(const formats::Adt* adt, pack::LodRange (&ranges)[pack::kLodLevels]);

Result<BakedTerrainTile> bakeTerrainTile(const formats::Adt& adt, u32 tx, u32 ty, const TerrainTextureIndex& textures);

// Horizon blob (pack::HorizonHeader) from the map's WDL.
std::vector<u8> bakeTerrainHorizon(const formats::Wdl& wdl);

// TerrainMap blob. tiles: index ty * 64 + tx, flags 0 where the map has no tile.
std::vector<u8> bakeTerrainMap(const std::array<pack::TerrainMapTile, pack::kMapTiles * pack::kMapTiles>& tiles,
                               u32 textureCount);

// Phase 12: the TerrainObjects blob of a map's global WMO (WDT MODF); empty if the WDT places none.
std::vector<u8> bakeMapObjects(const formats::Wdt& wdt);

} // namespace vd::bake
