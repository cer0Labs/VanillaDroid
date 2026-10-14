// Phase 14 (grass): the ground effect tables and records against the 1.12.1 client's data (VD_WOW_DIR).

#include "bake/GrassBaker.h"
#include "core/Hash.h"
#include "formats/Adt.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "pack/GrassFormat.h"
#include "pack/TerrainFormat.h"
#include "world/GrassPlacement.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace vd;

namespace {

bake::GroundEffectTables readTables(const formats::MpqArchiveSet& mpq, u32& textureFields, u32& doodadFields) {
    Result<std::vector<u8>> textureBytes = mpq.read("DBFilesClient\\GroundEffectTexture.dbc");
    Result<std::vector<u8>> doodadBytes = mpq.read("DBFilesClient\\GroundEffectDoodad.dbc");
    REQUIRE(textureBytes);
    REQUIRE(doodadBytes);
    Result<formats::DbcFile> textures = formats::DbcFile::parse(std::move(textureBytes.value()));
    Result<formats::DbcFile> doodads = formats::DbcFile::parse(std::move(doodadBytes.value()));
    REQUIRE(textures);
    REQUIRE(doodads);
    textureFields = textures.value()->fieldCount();
    doodadFields = doodads.value()->fieldCount();
    return bake::readGroundEffectTables(textures.value().view(), doodads.value().view());
}

Result<bake::GrassModelGeometry> loadModel(const formats::MpqArchiveSet& mpq, const bake::GroundEffectTables& tables,
                                           u32 tag) {
    Result<std::vector<u8>> bytes = mpq.read(bake::groundEffectModelPath(tables.doodadNames.at(tag)));
    if (!bytes) {
        return bytes.error();
    }
    Result<formats::M2Model> model = formats::parseM2(bytes.value());
    if (!model) {
        return model.error();
    }
    return bake::convertGrassModel(model.value());
}

} // namespace

TEST_CASE("Grass bake: GroundEffectTexture and GroundEffectDoodad of 1.12.1") {
    VD_REQUIRE_MPQ(mpq);
    u32 textureFields = 0;
    u32 doodadFields = 0;
    const bake::GroundEffectTables tables = readTables(*mpq, textureFields, doodadFields);
    CHECK(textureFields == 7); // ID, DoodadId[4], Density, TerrainType: no weights before 2.x
    CHECK(doodadFields == 3);  // ID, the tag, the model's name
    CHECK(tables.doodadRows == 443);
    // Elwynn's meadow: four grass models, 8 per cell.
    REQUIRE(tables.effects.count(505));
    const bake::GroundEffectRow& meadow = tables.effects.at(505);
    CHECK(meadow.doodads == std::array<u32, 4>{3, 4, 6, 8});
    CHECK(meadow.density == 8);
    // The references are GroundEffectDoodad's second column (WoW.exe indexes its table by it), not its ID: tag 0 is
    // valid (effect 507), there is no ID 0.
    CHECK(tables.doodadNames.at(0) == "ElwFlo01.mdl");
    CHECK(tables.doodadNames.at(3) == "ElwGra01.mdl");
    CHECK(tables.effects.at(507).doodads[0] == 0);
    CHECK(bake::groundEffectModelPath("ElwGra01.mdl") == "World\\NoDXT\\Detail\\ElwGra01.m2");
    CHECK(bake::groundEffectModelPath("DurGra01.mdx") == "World\\NoDXT\\Detail\\DurGra01.m2");
}

TEST_CASE("Grass bake: a detail doodad model and the tables") {
    VD_REQUIRE_MPQ(mpq);
    u32 textureFields = 0;
    u32 doodadFields = 0;
    const bake::GroundEffectTables rows = readTables(*mpq, textureFields, doodadFields);
    Result<bake::GrassModelGeometry> grass = loadModel(*mpq, rows, 3); // ElwGra01: two crossed quads
    REQUIRE(grass);
    CHECK(grass.value().vertices.size() == 8);
    CHECK(grass.value().indices.size() == 12);
    CHECK(normalizeAssetPath(grass.value().texture) == "world/nodxt/detail/newelynngrassflowerrock128.blp");
    CHECK(grass.value().clamp);
    CHECK(grass.value().height == doctest::Approx(0.4158f).epsilon(0.01));

    bake::GrassTables tables;
    tables.build(rows, [&](u32 tag) { return loadModel(*mpq, rows, tag); });
    CHECK(tables.effects().size() == 730); // every row with a doodad
    CHECK(tables.models().size() >= 300);
    CHECK(tables.effectIndex(505) != pack::kNoGrassEffect);
    CHECK(tables.effectIndex(0) == pack::kNoGrassEffect);
    CHECK(tables.effectIndex(11759) == pack::kNoGrassEffect); // a row without doodads (the footsteps' terrain type)
    const pack::GrassEffect& meadow = tables.effects()[tables.effectIndex(505)];
    CHECK(meadow.density == 8);
    CHECK((meadow.flags & pack::kGrassEffectBroken) == 0);
    for (const u16 model : meadow.models) {
        REQUIRE(model < tables.models().size());
        CHECK(tables.models()[model].indexCount == 12);
    }
    // Models are sorted by texture: Elwynn's share one.
    CHECK(tables.models()[meadow.models[0]].texture == tables.models()[meadow.models[1]].texture);
    // A row whose density is 0 grows 8 a cell (WoW.exe).
    CHECK(tables.effects()[tables.effectIndex(515)].density == 8);

    // The blob reads back.
    const std::vector<u8> blob = tables.modelsBlob();
    pack::GrassModelsHeader header;
    REQUIRE(blob.size() >= sizeof(header));
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.version == pack::kGrassFormatVersion);
    CHECK(header.modelCount == tables.models().size());
    CHECK(header.effectCount == tables.effects().size());
    CHECK(header.indicesOffset + header.indexCount * 2 <= blob.size());
}

TEST_CASE("Grass bake: Goldshire's meadow chunk (Azeroth_31_49)") {
    VD_REQUIRE_MPQ(mpq);
    u32 textureFields = 0;
    u32 doodadFields = 0;
    const bake::GroundEffectTables rows = readTables(*mpq, textureFields, doodadFields);
    bake::GrassTables tables;
    tables.build(rows, [&](u32 tag) { return loadModel(*mpq, rows, tag); });
    Result<std::vector<u8>> bytes = mpq->read(formats::adtPath("Azeroth", 31, 49));
    REQUIRE(bytes);
    Result<formats::Adt> adt = formats::parseAdt(bytes.value());
    REQUIRE(adt);
    // The chunk under (-9464, 62), south-west of Goldshire: row 11, column 14.
    const formats::AdtChunk& chunk = adt.value().chunk(14, 11);
    CHECK(chunk.indexX == 14);
    CHECK(chunk.indexY == 11);
    // Every cell's layer (low-quality texture map, 2 bits) is one of the chunk's layers.
    for (u32 row = 0; row < 8; ++row) {
        for (u32 column = 0; column < 8; ++column) {
            CHECK(((chunk.cellLayers[row] >> (column * 2)) & 3u) < chunk.layerCount);
        }
    }
    const pack::GrassChunk record = tables.chunkRecord(chunk, 0);
    REQUIRE((record.flags & pack::kGrassChunkGrows) != 0);

    // WoW.exe's placement over it.
    f32 heights[pack::kChunkVertices];
    f32 low = 1e9f;
    f32 high = -1e9f;
    for (u32 v = 0; v < pack::kChunkVertices; ++v) {
        heights[v] = chunk.position[2] + chunk.heights[v];
        low = std::min(low, heights[v]);
        high = std::max(high, heights[v]);
    }
    grass::ChunkInput input;
    input.chunk = &record;
    input.heights = heights;
    input.shadowBits = chunk.hasShadow ? chunk.shadowBits.data() : nullptr;
    input.seed = grass::chunkSeed(31, 49, 14, 11);
    std::vector<grass::PlacedGrass> placed(grass::maxPerChunk(16, 20));
    grass::PlacementStats stats;
    const u32 count = grass::placeChunk(input, tables.effects(), tables.models(), 16, placed, &stats);
    MESSAGE("Goldshire chunk: ", count, " doodads in ", stats.cellsGrowing, " of 16 cells drawn");
    CHECK(count > 0);
    CHECK(count <= 16 * 20);
    for (u32 i = 0; i < count; ++i) {
        CHECK(placed[i].position[2] >= low - 0.01f);
        CHECK(placed[i].position[2] <= high + 0.01f);
        CHECK(placed[i].model < tables.models().size());
    }

    // The tile's blob: header, 256 records, the shadows of the chunks that grow.
    u32 growing = 0;
    const std::vector<u8> blob = tables.tileBlob(adt.value(), 31, 49, &growing);
    pack::GrassTileHeader header;
    REQUIRE(blob.size() >= sizeof(header));
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.tileX == 31);
    CHECK(header.tileY == 49);
    CHECK(growing > 128); // Elwynn: most of the tile
    CHECK(header.chunksOffset + pack::kTileChunks * sizeof(pack::GrassChunk) <= blob.size());
    CHECK(header.shadowsOffset + header.shadowCount * pack::kGrassShadowBytes <= blob.size());
    pack::GrassChunk baked;
    std::memcpy(&baked, blob.data() + header.chunksOffset + (11 * 16 + 14) * sizeof(pack::GrassChunk), sizeof(baked));
    CHECK(std::memcmp(baked.cellLayers, record.cellLayers, sizeof(baked.cellLayers)) == 0);
    CHECK(std::memcmp(baked.effects, record.effects, sizeof(baked.effects)) == 0);
}
