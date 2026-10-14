// Phase 14 (grass): WoW.exe's detail doodad placement (world/GrassPlacement), without data, and the setting.

#include "app/QualityGovernor.h"
#include "app/SettingsFile.h"
#include "pack/GrassFormat.h"
#include "pack/TerrainFormat.h"
#include "world/GrassPlacement.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace vd;

namespace {

// One model of 8 vertices and 12 indices (the Elwynn grass) on texture 1, and one effect using it in every slot.
struct Fixture {
    std::vector<pack::GrassModel> models{pack::GrassModel{1, 0, 12, 0, 8, 0.5f, 0, 0}};
    std::vector<pack::GrassEffect> effects{pack::GrassEffect{505, {0, 0, 0, 0}, 8, 0}};
    pack::GrassChunk chunk{};
    std::array<f32, pack::kChunkVertices> heights{};

    Fixture() {
        std::fill(std::begin(chunk.effects), std::end(chunk.effects), pack::kNoGrassEffect);
        chunk.effects[0] = 0; // layer 0 everywhere (cellLayers all 0)
        chunk.flags = pack::kGrassChunkGrows;
        chunk.shadow = pack::kNoGrassShadow;
        heights.fill(10.0f);
    }
    grass::ChunkInput input(const u8* shadow = nullptr) const {
        grass::ChunkInput in;
        in.chunk = &chunk;
        in.heights = heights.data();
        in.shadowBits = shadow;
        in.seed = grass::chunkSeed(31, 49, 3, 7);
        return in;
    }
};

} // namespace

TEST_CASE("grass: the client's random generator") {
    // Reference values worked out from the inline copy of the generator in CMapChunk::CreateDetailDoodads
    // (0x6BFD10), independently of the function this implements (0x4531E0).
    grass::ClientRandom chunk(grass::chunkSeed(31, 49, 0, 0));
    const u32 expected[] = {0x16fc48c1, 0xcc20a13b, 0x967dff0a, 0x41f8e0e0, 0xc5571d3c, 0x7ce8f171, 0x9f182383,
                            0x90b7ca91};
    for (const u32 value : expected) {
        CHECK(chunk.next() == value);
    }
    grass::ClientRandom zero(0);
    CHECK(zero.next() == 0x16be8aa9u);
    CHECK(zero.next() == 0xba39c860u);
    CHECK(grass::chunkSeed(31, 49, 3, 7) == ((49u * 16 + 7) << 16 | (31u * 16 + 3)));

    // Mantissa in [1, 2), the sign bit mirrors it: [-1, 1].
    CHECK(grass::ClientRandom::toSigned(0) == doctest::Approx(-1.0f));
    CHECK(grass::ClientRandom::toSigned(0x80000000u) == doctest::Approx(1.0f));
    CHECK(grass::ClientRandom::toSigned(0x00400000u) == doctest::Approx(-0.5f));
    CHECK(grass::ClientRandom::toSigned(0x80400000u) == doctest::Approx(0.5f));
}

TEST_CASE("grass: a flat chunk gets frillDensity cells of density doodads, on the ground") {
    Fixture f;
    std::vector<grass::PlacedGrass> out(grass::maxPerChunk(16, 20));
    grass::PlacementStats stats;
    const u32 count = grass::placeChunk(f.input(), f.effects, f.models, 16, out, &stats);
    CHECK(count == 16 * 8); // every drawn cell grows (the batch budget, 1024 indices, holds 85 of these)
    CHECK(stats.cellsDrawn == 16);
    CHECK(stats.cellsGrowing == 16);
    CHECK(stats.dropped == 0);
    for (u32 i = 0; i < count; ++i) {
        const grass::PlacedGrass& p = out[i];
        CHECK(p.position[0] <= 0.0f); // the chunk spans -33.3..0 from its north-west corner on both axes
        CHECK(p.position[0] >= -pack::kChunkSize - 0.01f);
        CHECK(p.position[1] <= 0.0f);
        CHECK(p.position[1] >= -pack::kChunkSize - 0.01f);
        CHECK(p.position[2] == doctest::Approx(10.0f));
        CHECK(p.normal[2] == doctest::Approx(1.0f));
        CHECK(p.scale >= 0.9f - 1e-6f);
        CHECK(p.scale <= 1.1f + 1e-6f);
        CHECK(p.cosine * p.cosine + p.sine * p.sine == doctest::Approx(1.0f));
        CHECK(p.shaded == 0);
    }
    // Stable: the same seed places the same doodads.
    std::vector<grass::PlacedGrass> again(out.size());
    REQUIRE(grass::placeChunk(f.input(), f.effects, f.models, 16, again, nullptr) == count);
    CHECK(std::memcmp(out.data(), again.data(), count * sizeof(grass::PlacedGrass)) == 0);
    // Half the density draws other cells (the generator draws all of them before placing): not a prefix.
    std::vector<grass::PlacedGrass> half(out.size());
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 8, half, nullptr) == 8 * 8);
}

TEST_CASE("grass: switched-off cells, holes and empty slots") {
    Fixture f;
    std::vector<grass::PlacedGrass> out(512);
    std::memset(f.chunk.noEffect, 0xFF, sizeof(f.chunk.noEffect));
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 16, out) == 0);
    std::memset(f.chunk.noEffect, 0, sizeof(f.chunk.noEffect));
    f.chunk.holes = 0xFFFF;
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 16, out) == 0);
    f.chunk.holes = 0;
    // A layer without an effect.
    for (u16& row : f.chunk.cellLayers) {
        row = 0x5555; // every cell on layer 1, which has none
    }
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 16, out) == 0);
    for (u16& row : f.chunk.cellLayers) {
        row = 0;
    }
    // Slots 1 and 3 empty: doodad j of the i-th cell takes slot (i + j) & 3, so half of them stay.
    f.effects[0].models[1] = pack::kNoGrassModel;
    f.effects[0].models[3] = pack::kNoGrassModel;
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 16, out) == 16 * 4);
    // A chunk the baker found not growing.
    f.chunk.flags = 0;
    CHECK(grass::placeChunk(f.input(), f.effects, f.models, 16, out) == 0);
}

TEST_CASE("grass: the batches' budget drops what does not fit") {
    Fixture f;
    f.models[0].indexCount = 300; // more than frillDensity 1 allows a batch (64): each one fills a new batch
    std::vector<grass::PlacedGrass> out(64);
    grass::PlacementStats stats;
    const u32 count = grass::placeChunk(f.input(), f.effects, f.models, 1, out, &stats);
    CHECK(count == 4); // four batches of one texture, one doodad each
    CHECK(stats.dropped == 4);
}

TEST_CASE("grass: heights and normals follow the terrain's triangle under each doodad") {
    Fixture f;
    // A slope rising towards the south (t, i.e. -X): z = 10 + 0.5 t.
    for (u32 row = 0; row < 17; ++row) {
        const bool inner = row % 2 == 1;
        const u32 columns = inner ? 8 : 9;
        for (u32 c = 0; c < columns; ++c) {
            const f32 t = (static_cast<f32>(row / 2) + (inner ? 0.5f : 0.0f)) * pack::kUnitSize;
            f.heights[(row / 2) * 17 + (inner ? 9 + c : c)] = 10.0f + 0.5f * t;
        }
    }
    std::vector<grass::PlacedGrass> out(256);
    const u32 count = grass::placeChunk(f.input(), f.effects, f.models, 16, out);
    REQUIRE(count > 0);
    const f32 expected[3] = {0.5f / std::sqrt(1.25f), 0.0f, 1.0f / std::sqrt(1.25f)}; // rises along -X
    for (u32 i = 0; i < count; ++i) {
        const grass::PlacedGrass& p = out[i];
        CHECK(p.position[2] == doctest::Approx(10.0f - 0.5f * p.position[0]).epsilon(1e-4));
        CHECK(p.normal[0] == doctest::Approx(expected[0]).epsilon(1e-3));
        CHECK(p.normal[1] == doctest::Approx(expected[1]).epsilon(1e-3));
        CHECK(p.normal[2] == doctest::Approx(expected[2]).epsilon(1e-3));
    }
}

TEST_CASE("grass: the chunk's shadow bits shade the doodads under them") {
    Fixture f;
    std::array<u8, pack::kGrassShadowBytes> all{};
    all.fill(0xFF);
    std::vector<grass::PlacedGrass> out(256);
    const u32 count = grass::placeChunk(f.input(all.data()), f.effects, f.models, 16, out);
    REQUIRE(count > 0);
    for (u32 i = 0; i < count; ++i) {
        CHECK(out[i].shaded == 1);
    }
    // Only the northern half (t < 16.7 yards: shadow rows 0..31) in shadow.
    std::array<u8, pack::kGrassShadowBytes> north{};
    std::fill(north.begin(), north.begin() + pack::kGrassShadowBytes / 2, 0xFF);
    grass::placeChunk(f.input(north.data()), f.effects, f.models, 16, out);
    for (u32 i = 0; i < count; ++i) {
        const f32 t = -out[i].position[0];
        if (t < 16.4f) {
            CHECK(out[i].shaded == 1);
        } else if (t > 16.9f) {
            CHECK(out[i].shaded == 0);
        }
    }
}

TEST_CASE("grass: the depth ramp") {
    CHECK(grass::fadeAt(0.0f) == doctest::Approx(252.0f / 255.0f));
    CHECK(grass::fadeAt(52.5f) == doctest::Approx(252.0f / 255.0f));
    CHECK(grass::fadeAt(61.25f) == doctest::Approx(0.5f * 252.0f / 255.0f));
    CHECK(grass::fadeAt(70.0f) == doctest::Approx(0.0f));
    CHECK(grass::fadeAt(90.0f) == doctest::Approx(0.0f));
}

TEST_CASE("grass: tile asset paths") {
    char path[96];
    CHECK(pack::grassTilePath(path, sizeof(path), "azeroth", 31, 49) == 19);
    CHECK(std::strcmp(path, "grass/azeroth/31_49") == 0);
}

TEST_CASE("grass: the ground clutter setting and the presets") {
    SettingsView v;
    CHECK(v.groundClutter == 2); // 1.12's frillDensity by default
    applyQualityPreset(v, static_cast<u32>(QualityPreset::Low));
    CHECK(v.groundClutter == 0);
    applyQualityPreset(v, static_cast<u32>(QualityPreset::Medium));
    CHECK(v.groundClutter == 1);
    applyQualityPreset(v, static_cast<u32>(QualityPreset::Ultra));
    CHECK(v.groundClutter == 2);
    applyQualityPreset(v, static_cast<u32>(QualityPreset::High));
    CHECK(currentQualityPreset(v) == static_cast<u32>(QualityPreset::High));
    v.groundClutter = 3;
    CHECK(currentQualityPreset(v) == static_cast<u32>(QualityPreset::Custom));
    // Saved and read back; out of range is clamped.
    SettingsView read;
    parseSettings(formatSettings(v), read);
    CHECK(read.groundClutter == 3);
    parseSettings("ground_clutter = 9\n", read);
    CHECK(read.groundClutter == 3);
}
