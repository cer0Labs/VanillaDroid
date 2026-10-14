#include "bake/PackWriter.h"
#include "bake/TerrainBaker.h"
#include "bake/TextureBaker.h"
#include "core/Hash.h"
#include "formats/Adt.h"
#include "formats/Blp.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u8;
using vd::usize;

// Goldshire, Elwynn Forest: world X -9066..-9600, Y 0..533.
constexpr u32 kGoldshireX = 31;
constexpr u32 kGoldshireY = 49;

const vd::formats::Adt* goldshire() {
    static const std::unique_ptr<vd::formats::Adt> adt = []() -> std::unique_ptr<vd::formats::Adt> {
        const vd::formats::MpqArchiveSet* mpq = vd::test::sharedMpq();
        if (!mpq) {
            return nullptr;
        }
        vd::Result<std::vector<u8>> bytes = mpq->read(vd::formats::adtPath("Azeroth", kGoldshireX, kGoldshireY));
        if (!bytes) {
            return nullptr;
        }
        vd::Result<vd::formats::Adt> parsed = vd::formats::parseAdt(bytes.value());
        return parsed ? std::make_unique<vd::formats::Adt>(std::move(parsed.value())) : nullptr;
    }();
    return adt.get();
}

#define VD_REQUIRE_GOLDSHIRE(var)                                                                        \
    VD_REQUIRE_MPQ(mpqForGoldshire_);                                                                    \
    static_cast<void>(mpqForGoldshire_);                                                                 \
    const vd::formats::Adt* var = goldshire();                                                          \
    REQUIRE(var != nullptr)

// Implicit tile-local position (s, t) of a tile vertex (pack/TerrainFormat.h), in outer-vertex units
// (exact: outer vertices on integers, inner ones on halves).
void vertexPosition(u32 index, f32& s, f32& t) {
    const u32 chunk = index / vd::pack::kChunkVertices;
    const u32 v = index % vd::pack::kChunkVertices;
    const u32 row = v / 17;
    const u32 k = v % 17;
    const f32 column = k < 9 ? static_cast<f32>(k) : static_cast<f32>(k - 9) + 0.5f;
    const f32 line = k < 9 ? static_cast<f32>(row) : static_cast<f32>(row) + 0.5f;
    s = static_cast<f32>(chunk % 16) * 8.0f + column;
    t = static_cast<f32>(chunk / 16) * 8.0f + line;
}

vd::bake::TerrainTextureIndex indexTextures(const vd::formats::Adt& adt) {
    vd::bake::TerrainTextureIndex index;
    for (const std::string& texture : adt.textures) {
        index.emplace(vd::normalizeAssetPath(texture), static_cast<u32>(index.size()));
    }
    return index;
}

} // namespace

TEST_CASE("Chunk ids match the reversed on-disk magic") {
    const char bytes[4] = {'R', 'E', 'V', 'M'};
    u32 value = 0;
    std::memcpy(&value, bytes, 4);
    CHECK(vd::formats::chunkId("MVER") == value);
}

TEST_CASE("WDT lists the Azeroth tiles") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read(vd::formats::wdtPath("Azeroth"));
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::Wdt> wdt = vd::formats::parseWdt(bytes.value());
    REQUIRE(wdt.ok());
    CHECK(wdt.value().tileCount == 687);
    CHECK(wdt.value().hasTile(kGoldshireX, kGoldshireY));
    CHECK_FALSE(wdt.value().hasTile(kGoldshireY, kGoldshireX));
    CHECK(wdt.value().wmoName.empty());
}

TEST_CASE("ADT parser reads Goldshire") {
    VD_REQUIRE_GOLDSHIRE(adt);
    REQUIRE(adt->chunks.size() == 256);
    CHECK(adt->textures.size() == 7);
    CHECK(adt->textures[2] == "Tileset\\Elwynn\\ElwynnGrassBase.blp");
    CHECK_FALSE(adt->models.empty());
    CHECK_FALSE(adt->doodads.empty());
    CHECK_FALSE(adt->wmoPlacements.empty());

    const vd::formats::AdtChunk& first = adt->chunk(0, 0);
    CHECK(first.layerCount == 4);
    CHECK((first.layers[1].flags & vd::formats::kLayerUseAlpha) != 0);
    CHECK(first.areaId == 1617);
    CHECK(first.position[2] == doctest::Approx(69.1404f));
    REQUIRE(first.liquids.size() == 1);
    CHECK(first.liquids[0].type == vd::formats::LiquidType::River);

    // Chunk positions are implicit in the tile/chunk indices (the runtime relies on it).
    for (u32 cy = 0; cy < 16; ++cy) {
        for (u32 cx = 0; cx < 16; ++cx) {
            const vd::formats::AdtChunk& chunk = adt->chunk(cx, cy);
            CHECK(chunk.indexX == cx);
            CHECK(chunk.indexY == cy);
            // The files round to ~0.001 yards.
            const f32 x = vd::pack::tileCornerX(kGoldshireY) - static_cast<f32>(cy) * vd::pack::kChunkSize;
            const f32 y = vd::pack::tileCornerY(kGoldshireX) - static_cast<f32>(cx) * vd::pack::kChunkSize;
            CHECK(std::abs(chunk.position[0] - x) < 0.01f);
            CHECK(std::abs(chunk.position[1] - y) < 0.01f);
        }
    }
}

TEST_CASE("MCNR normals follow the documented axes") {
    VD_REQUIRE_GOLDSHIRE(adt);
    using vd::formats::AdtChunk;
    // Slope from heights (column -> -Y, row -> -X) vs the stored normal, on every interior outer vertex.
    double sum = 0.0;
    u32 samples = 0;
    for (const AdtChunk& chunk : adt->chunks) {
        auto h = [&](u32 c, u32 r) { return chunk.heights[AdtChunk::outerVertex(c, r)]; };
        for (u32 row = 1; row < 8; ++row) {
            for (u32 column = 1; column < 8; ++column) {
                const auto& n = chunk.normals[AdtChunk::outerVertex(column, row)];
                const f32 nx = (h(column, row + 1) - h(column, row - 1)) / (2.0f * vd::pack::kUnitSize);
                const f32 ny = (h(column + 1, row) - h(column - 1, row)) / (2.0f * vd::pack::kUnitSize);
                const f32 a = std::sqrt(nx * nx + ny * ny + 1.0f);
                const f32 b = std::sqrt(f32(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
                sum += (nx * n[0] + ny * n[1] + n[2]) / (a * b);
                ++samples;
            }
        }
    }
    CHECK(sum / samples > 0.98);
}

TEST_CASE("MCAL alpha maps are row-major along the vertex rows") {
    VD_REQUIRE_GOLDSHIRE(adt);
    using vd::formats::AdtChunk;
    // Where two neighbouring chunks blend the same texture, the alpha must continue across the shared
    // edge. Compare the edge texels under the documented layout ([row][column], rows along -X like the
    // vertices) and under the transposed one: only the right layout is continuous.
    auto at = [](const AdtChunk& chunk, u32 layer, u32 row, u32 column) {
        return static_cast<double>(chunk.alpha[layer][row * 64 + column]);
    };
    double documented = 0.0;
    double transposed = 0.0;
    u32 samples = 0;
    for (u32 cy = 0; cy < 16; ++cy) {
        for (u32 cx = 0; cx + 1 < 16; ++cx) {
            const AdtChunk& a = adt->chunk(cx, cy);
            const AdtChunk& b = adt->chunk(cx + 1, cy); // east neighbour: shares a's last column
            for (u32 la = 1; la < a.layerCount; ++la) {
                for (u32 lb = 1; lb < b.layerCount; ++lb) {
                    if (a.layers[la].textureId != b.layers[lb].textureId) {
                        continue;
                    }
                    for (u32 i = 0; i < 63; ++i) {
                        documented += std::abs(at(a, la - 1, i, 62) - at(b, lb - 1, i, 0));
                        transposed += std::abs(at(a, la - 1, 62, i) - at(b, lb - 1, 0, i));
                        ++samples;
                    }
                }
            }
        }
    }
    REQUIRE(samples > 1000);
    MESSAGE("alpha edge mismatch: documented " << documented / samples << ", transposed " << transposed / samples);
    CHECK(documented < transposed * 0.5);
}

TEST_CASE("Terrain indices: counter-clockwise from above, holes removed") {
    const std::vector<u16> shared = vd::bake::buildTerrainIndices(nullptr, nullptr);
    REQUIRE(shared.size() == vd::pack::kTileIndices);
    for (usize i = 0; i < shared.size(); i += 3) {
        f32 s[3], t[3];
        for (u32 k = 0; k < 3; ++k) {
            REQUIRE(shared[i + k] < vd::pack::kTileVertices);
            vertexPosition(shared[i + k], s[k], t[k]);
        }
        // World X = -t, Y = -s: signed area in the XY plane must be positive.
        const f32 ax = -(t[1] - t[0]), ay = -(s[1] - s[0]);
        const f32 bx = -(t[2] - t[0]), by = -(s[2] - s[0]);
        REQUIRE(ax * by - ay * bx > 0.0f);
    }

    VD_REQUIRE_GOLDSHIRE(adt);
    // Goldshire already has holes (under buildings): count them, then punch two more blocks.
    u32 holeBlocks = 0;
    u32 solid = 0xFFFFFFFFu;
    for (u32 c = 0; c < vd::pack::kTileChunks; ++c) {
        holeBlocks += static_cast<u32>(std::popcount(adt->chunks[c].holes));
        if (adt->chunks[c].holes == 0 && solid == 0xFFFFFFFFu) {
            solid = c;
        }
    }
    CHECK(holeBlocks > 0);
    REQUIRE(solid + 1 < vd::pack::kTileChunks);
    constexpr u32 kBlockIndices = 4 * 4 * 3; // 2x2 quads, 4 triangles each
    CHECK(vd::bake::buildTerrainIndices(adt, nullptr).size() == vd::pack::kTileIndices - holeBlocks * kBlockIndices);

    vd::formats::Adt holed = *adt;
    holed.chunks[solid].holes = 0x0001 | 0x8000; // two more 2x2 blocks
    std::vector<vd::pack::TerrainChunk> chunks(vd::pack::kTileChunks);
    const std::vector<u16> indices = vd::bake::buildTerrainIndices(&holed, chunks.data());
    CHECK(indices.size() == vd::pack::kTileIndices - (holeBlocks + 2) * kBlockIndices);
    CHECK(chunks[solid].indexCount == vd::pack::kChunkIndices - 2 * kBlockIndices);
    CHECK(chunks[solid + 1].firstIndex == chunks[solid].firstIndex + chunks[solid].indexCount);
}

TEST_CASE("LOD levels: counter-clockwise, full-resolution tile border, fewer triangles") {
    // Unit-grid position of a tile vertex (outer vertices land on integers).
    auto position = [](u16 index, f32& s, f32& t) { vertexPosition(index, s, t); };
    usize previous = vd::pack::kTileIndices + 1;
    for (u32 level = 0; level < vd::pack::kLodLevels; ++level) {
        CAPTURE(level);
        const std::vector<u16> indices =
            level == 0 ? vd::bake::buildTerrainIndices(nullptr, nullptr) : vd::bake::buildTerrainLodIndices(nullptr, level);
        CHECK(indices.size() < previous);
        previous = indices.size();
        u32 borderEdges = 0;
        bool unitBorder = true;
        for (usize i = 0; i < indices.size(); i += 3) {
            f32 s[3], t[3];
            for (u32 k = 0; k < 3; ++k) {
                position(indices[i + k], s[k], t[k]);
            }
            const f32 ax = -(t[1] - t[0]), ay = -(s[1] - s[0]);
            const f32 bx = -(t[2] - t[0]), by = -(s[2] - s[0]);
            REQUIRE(ax * by - ay * bx > 0.0f);
            for (u32 k = 0; k < 3; ++k) {
                const u32 j = (k + 1) % 3;
                const bool onBorder = (s[k] == 0.0f && s[j] == 0.0f) || (s[k] == 128.0f && s[j] == 128.0f) ||
                                      (t[k] == 0.0f && t[j] == 0.0f) || (t[k] == 128.0f && t[j] == 128.0f);
                if (onBorder) {
                    ++borderEdges;
                    unitBorder &= std::abs(s[j] - s[k]) + std::abs(t[j] - t[k]) == 1.0f;
                }
            }
        }
        // 4 sides x 128 unit segments, each used by exactly one triangle, at every level.
        CHECK(borderEdges == 4 * 128);
        CHECK(unitBorder);
    }
}

TEST_CASE("Baked terrain tile matches the ADT") {
    VD_REQUIRE_GOLDSHIRE(adt);
    const vd::bake::TerrainTextureIndex textures = indexTextures(*adt);
    vd::Result<vd::bake::BakedTerrainTile> baked = vd::bake::bakeTerrainTile(*adt, kGoldshireX, kGoldshireY, textures);
    REQUIRE(baked.ok());
    const std::vector<u8>& blob = baked.value().blob;

    vd::pack::TerrainTileHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.version == vd::pack::kTerrainFormatVersion);
    CHECK(header.tileX == kGoldshireX);
    CHECK(header.tileY == kGoldshireY);
    CHECK((header.flags & vd::pack::kTerrainTileHasHoles) != 0); // Goldshire has holes: own index buffer
    CHECK(header.indicesOffset != 0);
    CHECK(header.lod[0].first == 0);
    CHECK(header.lod[0].count == vd::bake::buildTerrainIndices(adt, nullptr).size());
    CHECK(header.lod[3].first + header.lod[3].count == header.indexCount);
    CHECK(header.alphaSize == vd::pack::kTerrainAlphaSize * vd::pack::kTerrainAlphaSize * 2);
    // Goldshire has a river (chunk 0); the object placements close the blob.
    REQUIRE(header.liquidIndexCount > 0);
    CHECK(header.liquidOpaqueFirst == header.liquidIndexCount); // no magma/slime here
    CHECK(header.liquidIndexCount % 3 == 0);
    CHECK(header.liquidIndicesOffset + header.liquidIndexCount * 2 <= header.doodadsOffset);
    CHECK(header.doodadCount > 0);
    CHECK(header.wmoCount > 0); // the inn, the smithy...
    CHECK(header.wmosOffset + header.wmoCount * sizeof(vd::pack::TerrainWmoInstance) == blob.size());
    CHECK(header.minZ < header.maxZ);

    const auto* chunks = reinterpret_cast<const vd::pack::TerrainChunk*>(blob.data() + header.chunksOffset);
    const auto* vertices = reinterpret_cast<const vd::pack::TerrainVertex*>(blob.data() + header.verticesOffset);
    const auto* alpha = reinterpret_cast<const u16*>(blob.data() + header.alphaOffset);

    const u32 c = 3 * 16 + 7; // chunk (7, 3)
    const vd::formats::AdtChunk& src = adt->chunk(7, 3);
    for (u32 l = 0; l < 4; ++l) {
        const u8 expected = l < src.layerCount ? static_cast<u8>(src.layers[l].textureId) : vd::pack::kNoTerrainLayer;
        CHECK(chunks[c].layers[l] == expected);
    }
    CHECK(chunks[c].indexCount == vd::pack::kChunkIndices - std::popcount(src.holes) * 4 * 4 * 3);
    for (u32 v = 0; v < vd::pack::kChunkVertices; ++v) {
        const vd::pack::TerrainVertex& vertex = vertices[c * vd::pack::kChunkVertices + v];
        CHECK(vertex.height == src.position[2] + src.heights[v]);
        CHECK(vertex.normal[2] == src.normals[v][2]);
        CHECK(vertex.height >= chunks[c].minZ);
        CHECK(vertex.height <= chunks[c].maxZ);
    }
    // Texel (column 10, row 20) of chunk (7, 3).
    const u32 t = 20 * 64 + 10;
    const u16 texel = alpha[(3 * 64 + 20) * vd::pack::kTerrainAlphaSize + 7 * 64 + 10];
    CHECK(((texel >> 4) & 0xF) * 17 == (src.layerCount > 1 ? src.alpha[0][t] : 0));
    CHECK(((texel >> 8) & 0xF) * 17 == (src.layerCount > 2 ? src.alpha[1][t] : 0));
    CHECK((texel & 0xF) == (src.hasShadow && src.shadow[t] ? 15 : 0));
}

TEST_CASE("Liquid mesh lies on the river bed it belongs to") {
    VD_REQUIRE_GOLDSHIRE(adt);
    vd::Result<vd::bake::BakedTerrainTile> baked =
        vd::bake::bakeTerrainTile(*adt, kGoldshireX, kGoldshireY, indexTextures(*adt));
    REQUIRE(baked.ok());
    const std::vector<u8>& blob = baked.value().blob;
    vd::pack::TerrainTileHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    const auto* vertices = reinterpret_cast<const vd::pack::LiquidVertex*>(blob.data() + header.liquidVerticesOffset);
    const auto* indices = reinterpret_cast<const u16*>(blob.data() + header.liquidIndicesOffset);

    // Where the file says the water is deep, the terrain under that grid point (same row/column as the
    // outer MCVT vertices) must be well below the surface; this fails if the grid were transposed.
    u32 checked = 0;
    for (u32 i = 0; i < header.liquidIndexCount; ++i) {
        const vd::pack::LiquidVertex& v = vertices[indices[i]];
        REQUIRE(v.height < 1e30f);
        if (v.depth < 16) {
            continue;
        }
        const u32 chunk = v.grid / 81;
        const u32 row = (v.grid % 81) / 9;
        const u32 column = v.grid % 9;
        const vd::formats::AdtChunk& src = adt->chunks[chunk];
        const f32 ground = src.position[2] + src.heights[vd::formats::AdtChunk::outerVertex(column, row)];
        CHECK(ground < v.height - 1.0f);
        ++checked;
    }
    CHECK(checked > 0);
    // Triangles counter-clockwise seen from above, like the terrain.
    for (u32 i = 0; i < header.liquidIndexCount; i += 3) {
        f32 s[3], t[3];
        for (u32 k = 0; k < 3; ++k) {
            const u16 grid = vertices[indices[i + k]].grid;
            s[k] = static_cast<f32>((grid / 81) % 16 * 8 + grid % 9);
            t[k] = static_cast<f32>((grid / 81) / 16 * 8 + (grid % 81) / 9);
        }
        const f32 ax = -(t[1] - t[0]), ay = -(s[1] - s[0]);
        const f32 bx = -(t[2] - t[0]), by = -(s[2] - s[0]);
        REQUIRE(ax * by - ay * bx > 0.0f);
    }
}

TEST_CASE("PackWriter compresses with LZ4 and PackReader restores the bytes") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vd_test_lz4.vdpk";
    std::vector<u8> data(200000);
    for (usize i = 0; i < data.size(); ++i) {
        data[i] = static_cast<u8>((i / 100) % 7); // compressible
    }
    {
        vd::bake::PackWriter writer;
        REQUIRE(writer.create(path, 1).ok());
        REQUIRE(writer.add("a/compressed.bin", vd::pack::AssetType::TerrainTile, data, true).ok());
        REQUIRE(writer.finish().ok());
    }
    vd::pack::PackReader reader;
    REQUIRE(reader.open(path).ok());
    const auto asset = reader.find("a/compressed.bin");
    REQUIRE(asset.has_value());
    CHECK(asset->compression == vd::pack::Compression::Lz4);
    CHECK(asset->rawSize == data.size());
    CHECK(asset->size < data.size() / 10);
    std::vector<u8> restored(asset->rawSize);
    REQUIRE(vd::pack::PackReader::read(*asset, restored.data(), restored.size()).ok());
    CHECK(restored == data);
    CHECK_FALSE(vd::pack::PackReader::read(*asset, restored.data(), restored.size() - 1).ok());
    reader.close();
    std::filesystem::remove(path);
}

TEST_CASE("Terrain texture array: resampled layers, one blob") {
    VD_REQUIRE_MPQ(mpq);
    const char* paths[] = {"Tileset\\Elwynn\\ElwynnGrassBase.blp", "Tileset\\Elwynn\\ElwynnDirtBase2.blp"};
    std::vector<std::vector<u8>> layers;
    for (const char* path : paths) {
        vd::Result<std::vector<u8>> bytes = mpq->read(path);
        REQUIRE(bytes.ok());
        vd::Result<vd::formats::BlpImage> image = vd::formats::decodeBlp(bytes.value());
        REQUIRE(image.ok());
        // Force a real resample on the first one: 256 -> 128 -> 256.
        vd::formats::BlpImage source = image.value();
        if (layers.empty()) {
            source = vd::bake::resampleSquare(source, 128);
            CHECK(source.mips.size() == 8);
        }
        const vd::formats::BlpImage square = vd::bake::resampleSquare(source, vd::pack::kTerrainTextureSize);
        CHECK(square.width == 256);
        CHECK(square.mips.size() == 9);
        vd::Result<std::vector<u8>> blob = vd::bake::bakeTexture(square, vd::pack::TextureFormat::Astc6x6);
        REQUIRE(blob.ok());
        layers.push_back(std::move(blob.value()));
    }
    vd::Result<std::vector<u8>> array = vd::bake::buildTextureArray(layers);
    REQUIRE(array.ok());
    vd::pack::TextureHeader header;
    std::memcpy(&header, array.value().data(), sizeof(header));
    CHECK(header.layers == 2);
    CHECK(header.mipCount == 9);

    for (u32 level : {0u, 4u}) {
        vd::Result<std::vector<u8>> fromArray =
            vd::bake::decodeTextureBlob(array.value().data(), array.value().size(), level, 1);
        vd::Result<std::vector<u8>> fromLayer = vd::bake::decodeTextureBlob(layers[1].data(), layers[1].size(), level);
        REQUIRE(fromArray.ok());
        REQUIRE(fromLayer.ok());
        CHECK(fromArray.value() == fromLayer.value());
    }

    layers[1].resize(layers[1].size() / 2); // truncated layer
    CHECK_FALSE(vd::bake::buildTextureArray(layers).ok());
}

namespace {

// A WDT chunk: id as stored (chunkId of the reversed magic), size, payload.
void appendChunk(std::vector<u8>& out, const char (&magic)[5], const std::vector<u8>& payload) {
    const u32 id = vd::formats::chunkId(magic);
    const u32 size = static_cast<u32>(payload.size());
    out.insert(out.end(), reinterpret_cast<const u8*>(&id), reinterpret_cast<const u8*>(&id) + 4);
    out.insert(out.end(), reinterpret_cast<const u8*>(&size), reinterpret_cast<const u8*>(&size) + 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

// The baked placement of a map's global WMO (TerrainObjects blob).
vd::pack::TerrainWmoInstance mapObject(const std::vector<u8>& blob) {
    vd::pack::TerrainTileHeader header{};
    REQUIRE(blob.size() >= sizeof(header));
    std::memcpy(&header, blob.data(), sizeof(header));
    REQUIRE(header.version == vd::pack::kTerrainFormatVersion);
    REQUIRE(header.wmoCount == 1);
    REQUIRE(header.doodadCount == 0);
    REQUIRE(header.wmosOffset + sizeof(vd::pack::TerrainWmoInstance) <= blob.size());
    vd::pack::TerrainWmoInstance wmo{};
    std::memcpy(&wmo, blob.data() + header.wmosOffset, sizeof(wmo));
    return wmo;
}

bool insideBounds(const vd::pack::TerrainWmoInstance& wmo, f32 x, f32 y, f32 z) {
    return x >= wmo.boundsMin[0] && x <= wmo.boundsMax[0] && y >= wmo.boundsMin[1] && y <= wmo.boundsMax[1] &&
           z >= wmo.boundsMin[2] && z <= wmo.boundsMax[2];
}

} // namespace

TEST_CASE("Phase 12: a WDT's WMO placed at 0 sits at the map center, as vMaNGOS places it") {
    std::vector<u8> wdt;
    appendChunk(wdt, "MVER", {18, 0, 0, 0});
    appendChunk(wdt, "MPHD", std::vector<u8>(32, 0));
    appendChunk(wdt, "MAIN", std::vector<u8>(vd::pack::kMapTiles * vd::pack::kMapTiles * 8, 0));
    const std::string name = "World\\wmo\\Dungeon\\Test\\Test.wmo";
    std::vector<u8> names(name.begin(), name.end());
    names.push_back(0);
    appendChunk(wdt, "MWMO", names);
    std::vector<u8> modf(64, 0);
    const u32 uniqueId = 7;
    const f32 extents[6] = {-10.0f, -2.0f, -30.0f, 20.0f, 5.0f, 40.0f}; // placement axes: x, y (up), z
    std::memcpy(modf.data() + 4, &uniqueId, 4);
    std::memcpy(modf.data() + 32, extents, sizeof(extents));
    appendChunk(wdt, "MODF", modf);

    vd::Result<vd::formats::Wdt> parsed = vd::formats::parseWdt(wdt);
    REQUIRE(parsed.ok());
    const vd::formats::WdtWmoPlacement& p = parsed.value().wmoPlacement;
    CHECK(parsed.value().tileCount == 0);
    CHECK(parsed.value().hasWmoPlacement);
    CHECK(p.position[0] == vd::pack::kMapHalfSize);
    CHECK(p.position[1] == 0.0f);
    CHECK(p.position[2] == vd::pack::kMapHalfSize);
    CHECK(p.extentsMin[0] == doctest::Approx(vd::pack::kMapHalfSize - 10.0f));
    CHECK(p.extentsMax[2] == doctest::Approx(vd::pack::kMapHalfSize + 40.0f));

    // Baked: the building's origin is the world's (no translation; without rotation only the model axes'
    // flips, as for any WMO), its box in world axes (X = -placement z, Y = -placement x, Z = placement y).
    const vd::pack::TerrainWmoInstance wmo = mapObject(vd::bake::bakeMapObjects(parsed.value()));
    CHECK(wmo.model == vd::assetPathHash(vd::normalizeAssetPath(name)));
    CHECK(wmo.uniqueId == uniqueId);
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            const f32 expected = column == row ? 1.0f : 0.0f;
            CHECK(std::fabs(wmo.transform[row * 4 + column]) == doctest::Approx(expected).epsilon(1e-4));
        }
    }
    CHECK(wmo.boundsMin[0] == doctest::Approx(-40.0f));
    CHECK(wmo.boundsMax[0] == doctest::Approx(30.0f));
    CHECK(wmo.boundsMin[1] == doctest::Approx(-20.0f));
    CHECK(wmo.boundsMax[1] == doctest::Approx(10.0f));
    CHECK(wmo.boundsMin[2] == doctest::Approx(-2.0f));
    CHECK(wmo.boundsMax[2] == doctest::Approx(5.0f));

    vd::formats::Wdt terrainOnly;
    CHECK(vd::bake::bakeMapObjects(terrainOnly).empty());
}

TEST_CASE("Phase 12: dungeons made of one WMO hold their entrances (Wailing Caverns, Dire Maul)") {
    VD_REQUIRE_MPQ(mpq);
    struct Case {
        const char* map;
        f32 x, y, z; // areatrigger_teleport target of the entrance (vMaNGOS world database)
    };
    for (const Case& c : {Case{"WailingCaverns", -158.441f, 131.601f, -74.2552f},
                          Case{"DireMaul", 47.4501f, -153.665f, -2.71439f},
                          Case{"OrgrimmarInstance", 0.797643f, -8.23429f, -15.5288f}}) {
        CAPTURE(c.map);
        vd::Result<std::vector<u8>> bytes = mpq->read(vd::formats::wdtPath(c.map));
        REQUIRE(bytes.ok());
        vd::Result<vd::formats::Wdt> wdt = vd::formats::parseWdt(bytes.value());
        REQUIRE(wdt.ok());
        CHECK(wdt.value().tileCount == 0);
        REQUIRE(wdt.value().hasWmoPlacement);
        const vd::pack::TerrainWmoInstance wmo = mapObject(vd::bake::bakeMapObjects(wdt.value()));
        CHECK(wmo.transform[3] == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(wmo.transform[7] == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(wmo.transform[11] == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(insideBounds(wmo, c.x, c.y, c.z));
    }
}
