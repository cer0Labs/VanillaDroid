// Phase 18 (world): opening a map off the game thread. The texture staging layout, the asset streamer's ring, the map
// loader against the real packs ($VD_PACKS_DIR) and the collision index opened on a thread.

#include "app/MapTravelTest.h"
#include "core/Hash.h"
#include "pack/PackFormat.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"
#include "physics/CollisionWorld.h"
#include "render/TextureStaging.h"
#include "world/AssetStreamer.h"
#include "world/TerrainMapLoader.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace vd;

namespace {

// A pack texture blob: header, then each mip of every layer back to back, filled with a pattern.
std::vector<u8> makeBlob(pack::TextureFormat format, u16 size, u16 layers, u16 mips) {
    pack::TextureHeader h{};
    h.format = static_cast<u16>(format);
    h.width = size;
    h.height = size;
    h.mipCount = mips;
    h.layers = layers;
    const u32 block = pack::astcBlockSize(format);
    u32 offset = (sizeof(h) + 15) & ~15u;
    for (u32 level = 0; level < mips; ++level) {
        const u32 side = std::max(1u, u32{size} >> level);
        const u32 bytes = block ? ((side + block - 1) / block) * ((side + block - 1) / block) * 16 : side * side * 4;
        h.mipOffsets[level] = offset;
        h.mipSizes[level] = bytes * std::max<u32>(1, layers);
        offset = (offset + h.mipSizes[level] + 15) & ~15u;
    }
    std::vector<u8> blob(offset);
    std::memcpy(blob.data(), &h, sizeof(h));
    for (usize i = sizeof(h); i < blob.size(); ++i) {
        blob[i] = static_cast<u8>(i * 7);
    }
    return blob;
}

template <typename Done>
bool waitFor(Done&& done, u32 ms = 5000) {
    for (u32 waited = 0; waited < ms; waited += 2) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

} // namespace

TEST_CASE("Map open (phase 18): the staged texture layout") {
    const std::vector<u8> astc = makeBlob(pack::TextureFormat::Astc6x6, 256, 5, 9);
    pack::TextureHeader header;
    std::memcpy(&header, astc.data(), sizeof(header));

    // A GPU with ASTC takes the blob as it is: same header, as many bytes as the blob's last mip ends at.
    u64 bytes = 0;
    pack::TextureHeader native = stagedTextureLayout(header, true, bytes);
    CHECK(std::memcmp(&native, &header, sizeof(header)) == 0);
    CHECK(bytes == u64{header.mipOffsets[8]} + header.mipSizes[8]);

    // Without ASTC: RGBA8 from offset 0, every mip 16-byte aligned, all the layers of a mip together.
    pack::TextureHeader rgba = stagedTextureLayout(header, false, bytes);
    CHECK(rgba.format == static_cast<u16>(pack::TextureFormat::Rgba8));
    CHECK(rgba.mipOffsets[0] == 0);
    CHECK(rgba.mipSizes[0] == 256u * 256u * 4u * 5u);
    CHECK(rgba.mipSizes[8] == 1u * 1u * 4u * 5u);
    for (u32 level = 1; level < 9; ++level) {
        CHECK(rgba.mipOffsets[level] % 16 == 0);
        CHECK(rgba.mipOffsets[level] >= rgba.mipOffsets[level - 1] + rgba.mipSizes[level - 1]);
    }
    CHECK(bytes >= u64{rgba.mipOffsets[8]} + rgba.mipSizes[8]);

    // An RGBA8 (or R8) texture never changes, GPU or not.
    const std::vector<u8> plain = makeBlob(pack::TextureFormat::Rgba8, 64, 0, 3);
    std::memcpy(&header, plain.data(), sizeof(header));
    native = stagedTextureLayout(header, false, bytes);
    CHECK(std::memcmp(&native, &header, sizeof(header)) == 0);
}

TEST_CASE("Map open (phase 18): staging a texture as it is copies the blob, layer by layer") {
    const std::vector<u8> blob = makeBlob(pack::TextureFormat::Astc4x4, 64, 3, 4);
    pack::TextureHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    u64 bytes = 0;
    const pack::TextureHeader staged = stagedTextureLayout(header, true, bytes);
    std::vector<u8> out(bytes, 0xCD);
    std::atomic<u32> units{0};
    REQUIRE(stageTexture(blob.data(), blob.size(), staged, out.data(), nullptr, &units));
    CHECK(units.load() == 3u * 4u); // one per layer and mip
    CHECK(std::memcmp(out.data(), blob.data(), sizeof(header)) == 0);
    for (u32 level = 0; level < 4; ++level) {
        CHECK(std::memcmp(out.data() + staged.mipOffsets[level], blob.data() + header.mipOffsets[level],
                          header.mipSizes[level]) == 0);
    }
    // Stopped between two layers: false, nothing more written.
    std::atomic<bool> cancel{true};
    CHECK_FALSE(stageTexture(blob.data(), blob.size(), staged, out.data(), &cancel));
    // A blob shorter than its mips say is refused.
    CHECK_FALSE(stageTexture(blob.data(), blob.size() - 64, staged, out.data()));
}

TEST_CASE("Asset streamer (phase 18): an empty ring takes an asset larger than the space before its head") {
    // The ring's counters count the gap a wrap skips as in use: with everything released, an asset larger than the
    // part of the ring before the head waited forever (Stormwind's 29 MB WMO in the 48 MB ring of the world objects).
    constexpr u64 kCapacity = 4096;
    std::vector<u8> ring(kCapacity);
    std::vector<u8> small(1000, 1);
    std::vector<u8> large(3500, 2);
    StagingBlock block;
    block.id = 1;
    block.data = ring.data();
    block.size = kCapacity;
    auto asset = [](const std::vector<u8>& bytes) {
        pack::PackAsset a;
        a.data = bytes.data();
        a.size = static_cast<u32>(bytes.size());
        a.rawSize = a.size;
        a.type = pack::AssetType::Model;
        return a;
    };
    AssetStreamer streamer;
    streamer.start(block, "vd-test");
    REQUIRE(streamer.request(1, asset(small)));
    AssetStreamer::Done done;
    REQUIRE(waitFor([&] { return streamer.poll(done); }));
    CHECK(done.ok);
    streamer.release(done, 0); // nothing uploaded: free at once
    streamer.update(0);
    REQUIRE(streamer.request(2, asset(large))); // 1024 + 3500 > 4096: wraps, and only an empty ring holds it
    AssetStreamer::Done second;
    REQUIRE(waitFor([&] { return streamer.poll(second); }, 2000));
    CHECK(second.ok);
    CHECK(second.tag == 2);
    CHECK(second.size == large.size());
    CHECK(std::memcmp(ring.data() + second.stagingOffset, large.data(), large.size()) == 0);
    streamer.release(second, 0);
    streamer.stop();
}

TEST_CASE("Map open (phase 18): the map loader prepares and fills a map off the game thread (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir) {
        MESSAGE("VD_PACKS_DIR not set: skipping");
        return;
    }
    pack::PackReader world;
    if (!world.open(std::filesystem::path(dir) / pack::worldPackFileName("DeadminesInstance")).ok()) {
        MESSAGE("world_deadminesinstance.vdpk missing: skipping");
        return;
    }
    const std::optional<pack::PackAsset> textures = world.find("world/maps/deadminesinstance/terraintextures");
    REQUIRE(textures);

    TerrainMapLoader loader;
    loader.prepare(dir, "DeadminesInstance", true, 0);
    CHECK(loader.busy());
    REQUIRE(waitFor([&] { return loader.done(); }));
    REQUIRE(loader.succeeded());
    TerrainMapLoader::Map& map = loader.map();
    REQUIRE(map.header != nullptr);
    CHECK(map.header->version == pack::kTerrainFormatVersion);
    CHECK(map.tiles != nullptr);
    CHECK(map.sharedIndices != nullptr);
    CHECK(map.newLiquids); // nothing on the GPU yet
    CHECK(map.liquidKey != 0);
    CHECK(map.terrainOffset == 0);
    CHECK(map.liquidOffset >= textures->size);
    CHECK(map.stagingBytes >= map.liquidOffset);
    std::vector<u8> staging(map.stagingBytes);
    const u64 liquidKey = map.liquidKey;
    const u64 liquidOffset = map.liquidOffset;
    pack::PackReader pack = std::move(map.pack); // the game thread takes the pack; its mapping stays where it was
    loader.finish();
    CHECK_FALSE(loader.busy());

    loader.fill(staging.data());
    REQUIRE(waitFor([&] { return loader.done(); }));
    REQUIRE(loader.succeeded());
    CHECK(loader.fillProgress() == doctest::Approx(1.0f));
    // A GPU with ASTC gets the pack's texture blob as it is.
    CHECK(std::memcmp(staging.data(), textures->data, textures->size) == 0);
    loader.finish();

    // The same liquid frames already on the GPU: not staged again.
    loader.prepare(dir, "DeadminesInstance", true, liquidKey);
    REQUIRE(waitFor([&] { return loader.done(); }));
    REQUIRE(loader.succeeded());
    CHECK_FALSE(loader.map().newLiquids);
    CHECK(loader.map().stagingBytes < liquidOffset + 64);
    loader.finish();

    // A map without a world pack fails with a reason, off the game thread too.
    loader.prepare(dir, "NoSuchMap", true, 0);
    REQUIRE(waitFor([&] { return loader.done(); }));
    CHECK_FALSE(loader.succeeded());
    CHECK(loader.map().error.find("NoSuchMap") != std::string::npos);
    loader.finish();
}

TEST_CASE("Collision (phase 18): the tile index opened on a thread matches the synchronous one (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir) {
        MESSAGE("VD_PACKS_DIR not set: skipping");
        return;
    }
    physics::CollisionWorld sync;
    if (!sync.openMap(dir, "DeadminesInstance").ok()) {
        MESSAGE("collision_deadminesinstance.vdpk missing: skipping");
        return;
    }
    // The entrance of the Deadmines (walk_deadmines' first point).
    const glm::dvec3 at(-14.57, -385.48, 62.46);
    physics::CollisionWorld async;
    async.beginOpenMap(dir, "DeadminesInstance");
    CHECK(async.isOpen());
    CHECK_FALSE(async.ready(at.x, at.y, 2.0f)); // which tiles exist is not known yet: the character waits
    REQUIRE(waitFor([&] {
        async.update(at, 96.0f);
        sync.update(at, 96.0f);
        return async.ready(at.x, at.y, 2.0f) && sync.ready(at.x, at.y, 2.0f);
    }));
    const std::optional<f64> a = async.groundHeight(at.x, at.y, at.z + 2.0, at.z - 10.0);
    const std::optional<f64> b = sync.groundHeight(at.x, at.y, at.z + 2.0, at.z - 10.0);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == doctest::Approx(*b));
    CHECK(*a == doctest::Approx(at.z).epsilon(0.02));
    // Another map asked for while the index is still being read: the first is dropped, the second taken.
    async.beginOpenMap(dir, "Azeroth");
    async.beginOpenMap(dir, "DeadminesInstance");
    CHECK(async.mapDirectory() == "DeadminesInstance");
    REQUIRE(waitFor([&] {
        async.update(at, 96.0f);
        return async.ready(at.x, at.y, 2.0f);
    }));
    async.closeMap();
    CHECK_FALSE(async.isOpen());
}

TEST_CASE("Map travel (phase 18): VD_WORLD_TRAVEL stops, with the desktop's and the phones' separators") {
    for (const char* spec : {"DeadminesInstance:-14.6,-385.5,62.5;Azeroth:-9457,65.7,56@5",
                             "DeadminesInstance:-14.6/-385.5/62.5+Azeroth:-9457/65.7/56@5"}) {
#if defined(_WIN32)
        _putenv_s("VD_WORLD_TRAVEL", spec);
#else
        setenv("VD_WORLD_TRAVEL", spec, 1);
#endif
        MapTravelTest travel;
        REQUIRE(travel.init());
        // The first stop as soon as the start has settled a second; the next one 5 s after the arrival settles.
        CHECK(travel.update(0.5f, true) == nullptr);
        const MapTravelTest::Stop* first = travel.update(0.6f, true);
        REQUIRE(first != nullptr);
        CHECK(first->map == "DeadminesInstance");
        CHECK(first->position.y == doctest::Approx(-385.5));
        CHECK(travel.measuring());
        travel.record(16.0f);
        CHECK(travel.update(0.016f, true) == nullptr); // arrived
        CHECK_FALSE(travel.measuring());
        CHECK(travel.update(4.0f, true) == nullptr);
        const MapTravelTest::Stop* second = travel.update(1.5f, true);
        REQUIRE(second != nullptr);
        CHECK(second->map == "Azeroth");
        CHECK(second->position.z == doctest::Approx(56.0));
    }
#if defined(_WIN32)
    _putenv_s("VD_WORLD_TRAVEL", "");
#else
    unsetenv("VD_WORLD_TRAVEL");
#endif
}
