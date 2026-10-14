#include "bake/TerrainBaker.h"

#include "bake/ModelBaker.h"
#include "core/Hash.h"
#include "formats/Wmo.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace vd::bake {
namespace {

using formats::AdtChunk;

u32 alignUp(u32 value, u32 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

template <typename T>
void store(std::vector<u8>& blob, u32 offset, const T* items, usize count) {
    std::memcpy(blob.data() + offset, items, sizeof(T) * count);
}

// A WMO placement (ADT or WDT MODF: the same record) as the runtime reads it.
template <typename Placement>
pack::TerrainWmoInstance wmoInstance(const Placement& p, const std::string& model) {
    pack::TerrainWmoInstance wmo{};
    wmo.model = assetPathHash(normalizeAssetPath(model));
    const std::array<f32, 12> m = placementTransform(p.position, p.rotation, 1.0f);
    std::memcpy(wmo.transform, m.data(), sizeof(wmo.transform));
    const std::array<f32, 3> a = placementToWorld({p.extentsMin[0], p.extentsMin[1], p.extentsMin[2]});
    const std::array<f32, 3> b = placementToWorld({p.extentsMax[0], p.extentsMax[1], p.extentsMax[2]});
    for (u32 axis = 0; axis < 3; ++axis) {
        wmo.boundsMin[axis] = std::min(a[axis], b[axis]);
        wmo.boundsMax[axis] = std::max(a[axis], b[axis]);
    }
    wmo.uniqueId = p.uniqueId;
    wmo.doodadSet = p.doodadSet;
    return wmo;
}

// The 4 triangles of quad (column, row), counter-clockwise seen from above (+Z). With s = -Y and
// t = -X, going TL -> TR -> BR -> BL is clockwise from above, so each fan triangle is (C, next, prev).
void appendQuad(std::vector<u16>& out, u32 base, u32 column, u32 row) {
    const u16 tl = static_cast<u16>(base + AdtChunk::outerVertex(column, row));
    const u16 tr = static_cast<u16>(base + AdtChunk::outerVertex(column + 1, row));
    const u16 bl = static_cast<u16>(base + AdtChunk::outerVertex(column, row + 1));
    const u16 br = static_cast<u16>(base + AdtChunk::outerVertex(column + 1, row + 1));
    const u16 c = static_cast<u16>(base + AdtChunk::innerVertex(column, row));
    const u16 triangles[12] = {c, tr, tl, c, br, tr, c, bl, br, c, tl, bl};
    out.insert(out.end(), std::begin(triangles), std::end(triangles));
}

struct LiquidMesh {
    std::vector<pack::LiquidVertex> vertices;
    std::vector<u16> indices; // translucent first
    u32 opaqueFirst = 0;
    f32 maxZ = -FLT_MAX;
};

// MCLQ instances -> one mesh per tile. Every instance keeps its 81 vertices (heights of unused ones are
// FLT_MAX in the file: replaced by the instance's height range); quads whose tile flag low nibble is
// 0xF are not rendered. Triangles are counter-clockwise seen from above.
LiquidMesh buildLiquids(const formats::Adt& adt) {
    LiquidMesh mesh;
    std::vector<u16> opaque;
    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        for (const formats::AdtLiquid& liquid : adt.chunks[c].liquids) {
            const bool translucent =
                liquid.type == formats::LiquidType::River || liquid.type == formats::LiquidType::Ocean;
            std::vector<u16>& out = translucent ? mesh.indices : opaque;
            const u32 base = static_cast<u32>(mesh.vertices.size());
            bool used = false;
            for (u32 quad = 0; quad < 64; ++quad) {
                if ((liquid.tiles[quad] & 0x0F) == 0x0F) {
                    continue;
                }
                used = true;
                const u32 column = quad % 8;
                const u32 row = quad / 8;
                const u16 tl = static_cast<u16>(base + row * pack::kLiquidGrid + column);
                const u16 tr = static_cast<u16>(tl + 1);
                const u16 bl = static_cast<u16>(tl + pack::kLiquidGrid);
                const u16 br = static_cast<u16>(bl + 1);
                const u16 triangles[6] = {tl, bl, br, tl, br, tr};
                out.insert(out.end(), std::begin(triangles), std::end(triangles));
            }
            if (!used) {
                continue;
            }
            const f32 fallback = liquid.minHeight < 1e30f ? (liquid.minHeight + liquid.maxHeight) * 0.5f : 0.0f;
            for (u32 v = 0; v < pack::kLiquidGrid * pack::kLiquidGrid; ++v) {
                pack::LiquidVertex vertex{};
                vertex.height = liquid.heights[v] < 1e30f ? liquid.heights[v] : fallback;
                vertex.grid = static_cast<u16>(c * pack::kLiquidGrid * pack::kLiquidGrid + v);
                vertex.depth = liquid.depth[v];
                vertex.kind = static_cast<u8>(liquid.type);
                mesh.maxZ = std::max(mesh.maxZ, vertex.height);
                mesh.vertices.push_back(vertex);
            }
        }
    }
    mesh.opaqueFirst = static_cast<u32>(mesh.indices.size());
    mesh.indices.insert(mesh.indices.end(), opaque.begin(), opaque.end());
    return mesh;
}

// Coarse quad [x0, x0 + k] x [y0, y0 + k] of outer vertices in chunk (cx, cy): a fan around the outer
// vertex at its center. Its perimeter runs TL -> TR -> BR -> BL (clockwise seen from above) with only the
// corners, except on sides lying on the tile border, where every outer vertex is kept.
void appendCoarseQuad(std::vector<u16>& out, u32 base, u32 cx, u32 cy, u32 x0, u32 y0, u32 k) {
    const bool north = cy == 0 && y0 == 0;
    const bool south = cy == 15 && y0 + k == 8;
    const bool west = cx == 0 && x0 == 0;
    const bool east = cx == 15 && x0 + k == 8;
    auto vertex = [&](u32 x, u32 y) { return static_cast<u16>(base + AdtChunk::outerVertex(x, y)); };
    u16 ring[32];
    u32 n = 0;
    for (u32 x = x0; x < x0 + k; x += north ? 1 : k) {
        ring[n++] = vertex(x, y0);
    }
    for (u32 y = y0; y < y0 + k; y += east ? 1 : k) {
        ring[n++] = vertex(x0 + k, y);
    }
    for (u32 x = x0 + k; x > x0; x -= south ? 1 : k) {
        ring[n++] = vertex(x, y0 + k);
    }
    for (u32 y = y0 + k; y > y0; y -= west ? 1 : k) {
        ring[n++] = vertex(x0, y);
    }
    const u16 center = vertex(x0 + k / 2, y0 + k / 2);
    for (u32 i = 0; i < n; ++i) {
        const u16 triangle[3] = {center, ring[(i + 1) % n], ring[i]};
        out.insert(out.end(), std::begin(triangle), std::end(triangle));
    }
}

bool coarseQuadDropped(const AdtChunk& chunk, u32 x0, u32 y0, u32 k) {
    if (chunk.holes == 0) {
        return false;
    }
    // Hole blocks are 2x2 quads: bit (by * 4 + bx).
    u32 blocks = 0;
    u32 holes = 0;
    for (u32 by = y0 / 2; by < (y0 + k) / 2; ++by) {
        for (u32 bx = x0 / 2; bx < (x0 + k) / 2; ++bx) {
            ++blocks;
            holes += (chunk.holes >> (by * 4 + bx)) & 1u;
        }
    }
    return k == 2 ? holes > 0 : holes == blocks;
}

} // namespace

std::vector<u16> buildTerrainLodIndices(const formats::Adt* adt, u32 level) {
    const u32 k = pack::kLodStep[level];
    std::vector<u16> indices;
    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        for (u32 y0 = 0; y0 < 8; y0 += k) {
            for (u32 x0 = 0; x0 < 8; x0 += k) {
                if (adt && coarseQuadDropped(adt->chunks[c], x0, y0, k)) {
                    continue;
                }
                appendCoarseQuad(indices, c * pack::kChunkVertices, c % 16, c / 16, x0, y0, k);
            }
        }
    }
    return indices;
}

std::vector<u16> buildTerrainLodChain(const formats::Adt* adt, pack::LodRange (&ranges)[pack::kLodLevels]) {
    std::vector<u16> chain = buildTerrainIndices(adt, nullptr);
    ranges[0] = {0, static_cast<u32>(chain.size())};
    for (u32 level = 1; level < pack::kLodLevels; ++level) {
        const std::vector<u16> indices = buildTerrainLodIndices(adt, level);
        ranges[level] = {static_cast<u32>(chain.size()), static_cast<u32>(indices.size())};
        chain.insert(chain.end(), indices.begin(), indices.end());
    }
    return chain;
}

std::vector<u16> buildTerrainIndices(const formats::Adt* adt, pack::TerrainChunk* chunks) {
    std::vector<u16> indices;
    indices.reserve(pack::kTileIndices);
    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        const u32 first = static_cast<u32>(indices.size());
        for (u32 row = 0; row < 8; ++row) {
            for (u32 column = 0; column < 8; ++column) {
                if (adt && adt->chunks[c].isHole(column, row)) {
                    continue;
                }
                appendQuad(indices, c * pack::kChunkVertices, column, row);
            }
        }
        if (chunks) {
            chunks[c].firstIndex = first;
            chunks[c].indexCount = static_cast<u32>(indices.size()) - first;
        }
    }
    return indices;
}

Result<BakedTerrainTile> bakeTerrainTile(const formats::Adt& adt, u32 tx, u32 ty, const TerrainTextureIndex& textures) {
    if (adt.chunks.size() != pack::kTileChunks) {
        return Error{"ADT without 256 chunks"};
    }
    BakedTerrainTile out;
    out.summary.flags = pack::kMapTileExists;
    out.summary.minZ = FLT_MAX;
    out.summary.maxZ = -FLT_MAX;

    std::vector<pack::TerrainChunk> chunks(pack::kTileChunks);
    std::vector<pack::TerrainVertex> vertices(pack::kTileVertices);
    std::vector<u16> alpha(usize{pack::kTerrainAlphaSize} * pack::kTerrainAlphaSize);
    bool hasHoles = false;

    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        const AdtChunk& src = adt.chunks[c];
        pack::TerrainChunk& dst = chunks[c];
        hasHoles |= src.holes != 0;
        std::memset(dst.layers, pack::kNoTerrainLayer, sizeof(dst.layers));
        for (u32 l = 0; l < src.layerCount; ++l) {
            const std::string path = normalizeAssetPath(adt.textures[src.layers[l].textureId]);
            const auto it = textures.find(path);
            if (it == textures.end() || it->second >= pack::kNoTerrainLayer) {
                return Error{"Terrain texture without a layer: " + path};
            }
            dst.layers[l] = static_cast<u8>(it->second);
            dst.layerFlags[l] = static_cast<u8>(src.layers[l].flags & 0xFF);
        }
        dst.areaId = src.areaId;
        dst.flags = src.flags;

        dst.minZ = FLT_MAX;
        dst.maxZ = -FLT_MAX;
        for (u32 v = 0; v < pack::kChunkVertices; ++v) {
            pack::TerrainVertex& vertex = vertices[c * pack::kChunkVertices + v];
            vertex.height = src.position[2] + src.heights[v];
            vertex.normal[0] = src.normals[v][0];
            vertex.normal[1] = src.normals[v][1];
            vertex.normal[2] = src.normals[v][2];
            vertex.normal[3] = 0;
            dst.minZ = std::min(dst.minZ, vertex.height);
            dst.maxZ = std::max(dst.maxZ, vertex.height);
        }
        out.summary.minZ = std::min(out.summary.minZ, dst.minZ);
        out.summary.maxZ = std::max(out.summary.maxZ, dst.maxZ);

        // Alpha of layers 1..3 (4 bits each, as stored by the client) and the shadow bit.
        const u32 cx = c % 16;
        const u32 cy = c / 16;
        for (u32 row = 0; row < formats::kAlphaSize; ++row) {
            u16* line = alpha.data() + usize{cy * formats::kAlphaSize + row} * pack::kTerrainAlphaSize +
                        cx * formats::kAlphaSize;
            for (u32 column = 0; column < formats::kAlphaSize; ++column) {
                const u32 t = row * formats::kAlphaSize + column;
                const u32 r = src.layerCount > 1 ? src.alpha[0][t] / 17u : 0u;
                const u32 g = src.layerCount > 2 ? src.alpha[1][t] / 17u : 0u;
                const u32 b = src.layerCount > 3 ? src.alpha[2][t] / 17u : 0u;
                const u32 a = src.hasShadow && src.shadow[t] ? 15u : 0u;
                line[column] = static_cast<u16>((b << 12) | (g << 8) | (r << 4) | a);
            }
        }
    }

    const LiquidMesh liquids = buildLiquids(adt);
    if (liquids.vertices.size() > 0xFFFF) {
        return Error{"Too many liquid vertices in a tile"};
    }
    if (!liquids.indices.empty()) {
        out.summary.maxZ = std::max(out.summary.maxZ, liquids.maxZ);
    }

    std::vector<u16> indices;
    pack::LodRange lod[pack::kLodLevels]{};
    buildTerrainIndices(hasHoles ? &adt : nullptr, chunks.data()); // level 0 chunk ranges
    if (hasHoles) {
        indices = buildTerrainLodChain(&adt, lod);
        out.summary.flags |= pack::kMapTileHasHoles;
    }

    pack::TerrainTileHeader header{};
    header.version = pack::kTerrainFormatVersion;
    header.tileX = static_cast<u16>(tx);
    header.tileY = static_cast<u16>(ty);
    header.flags = hasHoles ? pack::kTerrainTileHasHoles : 0u;
    header.minZ = out.summary.minZ;
    header.maxZ = out.summary.maxZ;
    std::memcpy(header.lod, lod, sizeof(lod));
    header.chunksOffset = alignUp(static_cast<u32>(sizeof(header)), 64);
    header.verticesOffset = alignUp(header.chunksOffset + sizeof(pack::TerrainChunk) * pack::kTileChunks, 64);
    u32 end = header.verticesOffset + sizeof(pack::TerrainVertex) * pack::kTileVertices;
    if (hasHoles) {
        header.indicesOffset = alignUp(end, 64);
        header.indexCount = static_cast<u32>(indices.size());
        end = header.indicesOffset + header.indexCount * 2;
    }
    header.alphaOffset = alignUp(end, 64);
    header.alphaSize = static_cast<u32>(alpha.size() * 2);
    end = header.alphaOffset + header.alphaSize;
    if (!liquids.indices.empty()) {
        header.liquidVerticesOffset = alignUp(end, 64);
        header.liquidVertexCount = static_cast<u32>(liquids.vertices.size());
        header.liquidIndicesOffset =
            alignUp(header.liquidVerticesOffset + header.liquidVertexCount * sizeof(pack::LiquidVertex), 64);
        header.liquidIndexCount = static_cast<u32>(liquids.indices.size());
        header.liquidOpaqueFirst = liquids.opaqueFirst;
        end = header.liquidIndicesOffset + header.liquidIndexCount * 2;
    }

    // Object placements (CPU-side section).
    std::vector<pack::TerrainDoodadInstance> doodads;
    for (const formats::AdtDoodadPlacement& p : adt.doodads) {
        if (p.nameId >= adt.models.size()) {
            continue;
        }
        pack::TerrainDoodadInstance doodad{};
        doodad.model = assetPathHash(normalizeAssetPath(formats::m2Path(adt.models[p.nameId])));
        const std::array<f32, 12> m = placementTransform(p.position, p.rotation, static_cast<f32>(p.scale) / 1024.0f);
        std::memcpy(doodad.transform, m.data(), sizeof(doodad.transform));
        doodad.uniqueId = p.uniqueId;
        doodads.push_back(doodad);
    }
    std::vector<pack::TerrainWmoInstance> wmos;
    for (const formats::AdtWmoPlacement& p : adt.wmoPlacements) {
        if (p.nameId < adt.wmos.size()) {
            wmos.push_back(wmoInstance(p, adt.wmos[p.nameId]));
        }
    }
    if (!doodads.empty()) {
        header.doodadsOffset = alignUp(end, 64);
        header.doodadCount = static_cast<u32>(doodads.size());
        end = header.doodadsOffset + header.doodadCount * static_cast<u32>(sizeof(pack::TerrainDoodadInstance));
    }
    if (!wmos.empty()) {
        header.wmosOffset = alignUp(end, 64);
        header.wmoCount = static_cast<u32>(wmos.size());
        end = header.wmosOffset + header.wmoCount * static_cast<u32>(sizeof(pack::TerrainWmoInstance));
    }

    if (end > pack::kMaxTerrainTileBytes) {
        return Error{"Tile blob of " + std::to_string(end) + " bytes exceeds kMaxTerrainTileBytes"};
    }
    out.blob.resize(end);
    store(out.blob, 0, &header, 1);
    store(out.blob, header.chunksOffset, chunks.data(), chunks.size());
    store(out.blob, header.verticesOffset, vertices.data(), vertices.size());
    if (hasHoles) {
        store(out.blob, header.indicesOffset, indices.data(), indices.size());
    }
    store(out.blob, header.alphaOffset, alpha.data(), alpha.size());
    if (!liquids.indices.empty()) {
        store(out.blob, header.liquidVerticesOffset, liquids.vertices.data(), liquids.vertices.size());
        store(out.blob, header.liquidIndicesOffset, liquids.indices.data(), liquids.indices.size());
    }
    if (!doodads.empty()) {
        store(out.blob, header.doodadsOffset, doodads.data(), doodads.size());
    }
    if (!wmos.empty()) {
        store(out.blob, header.wmosOffset, wmos.data(), wmos.size());
    }
    return out;
}

std::vector<u8> bakeTerrainHorizon(const formats::Wdl& wdl) {
    std::vector<u16> slots(pack::kMapTiles * pack::kMapTiles, pack::kNoHorizon);
    std::vector<i16> heights;
    for (u32 i = 0; i < slots.size(); ++i) {
        if (wdl.tiles[i].empty()) {
            continue;
        }
        slots[i] = static_cast<u16>(heights.size() / pack::kHorizonStride);
        heights.insert(heights.end(), wdl.tiles[i].begin(), wdl.tiles[i].end());
        heights.push_back(0); // padding
    }
    // One tile: 16x16 quads, each a fan of 4 triangles around its inner sample (like the terrain).
    std::vector<u16> indices;
    for (u32 row = 0; row < pack::kHorizonInner; ++row) {
        for (u32 column = 0; column < pack::kHorizonInner; ++column) {
            const u16 tl = static_cast<u16>(row * pack::kHorizonOuter + column);
            const u16 tr = static_cast<u16>(tl + 1);
            const u16 bl = static_cast<u16>(tl + pack::kHorizonOuter);
            const u16 br = static_cast<u16>(bl + 1);
            const u16 c = static_cast<u16>(pack::kHorizonOuter * pack::kHorizonOuter + row * pack::kHorizonInner + column);
            const u16 triangles[12] = {c, tr, tl, c, br, tr, c, bl, br, c, tl, bl};
            indices.insert(indices.end(), std::begin(triangles), std::end(triangles));
        }
    }

    pack::HorizonHeader header{};
    header.version = pack::kTerrainFormatVersion;
    header.tileCount = static_cast<u32>(heights.size() / pack::kHorizonStride);
    header.slotsOffset = alignUp(static_cast<u32>(sizeof(header)), 64);
    header.heightsOffset = alignUp(header.slotsOffset + static_cast<u32>(slots.size() * 2), 64);
    header.indicesOffset = alignUp(header.heightsOffset + static_cast<u32>(heights.size() * 2), 64);
    header.indexCount = static_cast<u32>(indices.size());
    std::vector<u8> blob(header.indicesOffset + indices.size() * 2);
    store(blob, 0, &header, 1);
    store(blob, header.slotsOffset, slots.data(), slots.size());
    store(blob, header.heightsOffset, heights.data(), heights.size());
    store(blob, header.indicesOffset, indices.data(), indices.size());
    return blob;
}

std::vector<u8> bakeTerrainMap(const std::array<pack::TerrainMapTile, pack::kMapTiles * pack::kMapTiles>& tiles,
                               u32 textureCount) {
    pack::TerrainMapHeader header{};
    const std::vector<u16> shared = buildTerrainLodChain(nullptr, header.lod);
    header.version = pack::kTerrainFormatVersion;
    header.textureCount = textureCount;
    for (const pack::TerrainMapTile& tile : tiles) {
        header.tileCount += (tile.flags & pack::kMapTileExists) ? 1u : 0u;
    }
    header.tilesOffset = alignUp(static_cast<u32>(sizeof(header)), 64);
    header.sharedIndicesOffset =
        alignUp(header.tilesOffset + static_cast<u32>(sizeof(pack::TerrainMapTile) * tiles.size()), 64);
    header.sharedIndexCount = static_cast<u32>(shared.size());

    std::vector<u8> blob(header.sharedIndicesOffset + shared.size() * 2);
    store(blob, 0, &header, 1);
    store(blob, header.tilesOffset, tiles.data(), tiles.size());
    store(blob, header.sharedIndicesOffset, shared.data(), shared.size());
    return blob;
}

std::vector<u8> bakeMapObjects(const formats::Wdt& wdt) {
    if (!wdt.hasWmoPlacement || wdt.wmoName.empty()) {
        return {};
    }
    const pack::TerrainWmoInstance wmo = wmoInstance(wdt.wmoPlacement, wdt.wmoName);
    pack::TerrainTileHeader header{};
    header.version = pack::kTerrainFormatVersion;
    header.tileX = header.tileY = 0xFFFF;
    header.minZ = wmo.boundsMin[2];
    header.maxZ = wmo.boundsMax[2];
    header.wmosOffset = alignUp(static_cast<u32>(sizeof(header)), 64);
    header.wmoCount = 1;
    std::vector<u8> blob(header.wmosOffset + sizeof(wmo));
    store(blob, 0, &header, 1);
    store(blob, header.wmosOffset, &wmo, 1);
    return blob;
}

} // namespace vd::bake
