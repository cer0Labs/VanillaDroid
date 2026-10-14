// Phase 14 (sound): the rooms of the buildings (world/WmoAreas): WMOAreaTable's rows, and where a point is in the
// baked "wmoareas/azeroth" (VD_PACKS_DIR: db.vdpk, worldsound.vdpk): the Lion's Pride Inn, Stormwind's districts
// and taverns, Ironforge's Great Forge.
#include "core/Clock.h"
#include "pack/PackReader.h"
#include "pack/WorldSoundFormat.h"
#include "world/WmoAreas.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace vd;

namespace {

struct Areas {
    pack::PackReader db;
    WmoAreas areas;
    bool ready = false;

    explicit Areas(const char* map) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() || !areas.init(db, dir)) {
            return;
        }
        pack::PackReader pack;
        if (!pack.open(std::filesystem::path(dir) / pack::kWorldSoundPack).ok()) {
            return;
        }
        const std::optional<pack::PackAsset> asset = pack.find(pack::wmoAreasPath(map));
        if (!asset) {
            return;
        }
        auto data = std::make_unique<u8[]>(asset->rawSize);
        REQUIRE(pack::PackReader::read(*asset, data.get(), asset->rawSize).ok());
        ready = areas.load(std::move(data), asset->rawSize);
    }
};

} // namespace

TEST_CASE("WMO areas: WMOAreaTable rows of the 1.12.1 client (VD_PACKS_DIR)") {
    Areas a("Azeroth");
    if (!a.areas.rowsReady()) {
        MESSAGE("VD_PACKS_DIR not set: skipping");
        return;
    }
    // The Lion's Pride Inn: the generic inn (WMO 53) placed with name set 2; its rooms take the root row's music,
    // ambience and name; the cellar (group 903) has its own.
    const WmoAreaRow* root = a.areas.row(53, 2, -1);
    REQUIRE(root);
    CHECK(root->name == "Lion's Pride Inn");
    CHECK(root->zoneMusic == 156); // Zone-TavernAlliance
    CHECK(root->ambience == 170);  // Tavern
    CHECK(root->areaId == 0);
    WmoAreaHit hit;
    hit.wmoId = 53;
    hit.nameSet = 2;
    hit.groupId = 894;
    a.areas.resolve(hit);
    CHECK(hit.valid());
    CHECK(hit.name == "Lion's Pride Inn");
    CHECK(hit.zoneMusic == 156);
    CHECK(hit.ambience == 170);
    hit.groupId = 903;
    a.areas.resolve(hit);
    CHECK(hit.ambience == 27);   // Crypt
    CHECK(hit.zoneMusic == 183); // Moment-Haunted02
    CHECK(hit.name == "Lion's Pride Inn");
    // The same building in Menethil (name set 1): the Deepwater Tavern, with an area of its own.
    hit.nameSet = 1;
    hit.groupId = 894;
    a.areas.resolve(hit);
    CHECK(hit.name == "Deepwater Tavern");
    CHECK(hit.areaId == 2104);
    // Ironforge's Great Forge: the hall's ambience and name.
    hit.wmoId = 208;
    hit.nameSet = 0;
    hit.groupId = 13193; // the hall (group 66 of ironforge.wmo)
    a.areas.resolve(hit);
    CHECK(hit.name == "The Great Forge");
    CHECK(hit.ambience == 130);
    CHECK(hit.zoneMusic == 232); // the city's (root row)
    CHECK(hit.areaId == 1537);
    // Nothing for a building without rows.
    hit.wmoId = 999999;
    a.areas.resolve(hit);
    CHECK_FALSE(hit.valid());
}

TEST_CASE("WMO areas: where a point is in the baked buildings of Azeroth (VD_PACKS_DIR)") {
    Areas a("Azeroth");
    if (!a.ready) {
        MESSAGE("VD_PACKS_DIR without worldsound.vdpk: skipping");
        return;
    }
    CHECK(a.areas.placementCount() > 100);
    // Innkeeper Farley's floor in the Lion's Pride Inn (the inn's origin is at -9464.2, 24.4, 56.5).
    const WmoAreaHit inn = a.areas.locate(glm::dvec3(-9462.7, 16.2, 57.0), std::nullopt);
    REQUIRE(inn.valid());
    CHECK(inn.uniqueId == 71414);
    CHECK(inn.wmoId == 53);
    CHECK(inn.nameSet == 2);
    CHECK(inn.name == "Lion's Pride Inn");
    CHECK(inn.zoneMusic == 156);
    CHECK(inn.ambience == 170);
    // Goldshire's road, outside: not in a building.
    CHECK_FALSE(a.areas.locate(glm::dvec3(-9440.0, 60.0, 56.4), std::nullopt).valid());
    // The terrain over a floor found below (a cellar under the ground): outdoors.
    CHECK_FALSE(a.areas.locate(glm::dvec3(-9462.7, 16.2, 57.0), 59.0f).valid());
    // Stormwind (one WMO, its streets are its groups): a street of the Trade District.
    const WmoAreaHit trade = a.areas.locate(glm::dvec3(-8834.0, 642.0, 94.0), std::nullopt);
    REQUIRE(trade.valid());
    CHECK(trade.wmoId == 61);
    CHECK(trade.name == "Trade District");
    CHECK(trade.areaId == 1519);
    // A tavern of Old Town (group 2679): the district's name, a tavern's music and ambience.
    const WmoAreaHit tavern = a.areas.locate(glm::dvec3(-8614.7, 382.8, 103.2), std::nullopt);
    REQUIRE(tavern.valid());
    CHECK(tavern.groupId == 2679);
    CHECK(tavern.name == "Old Town");
    CHECK(tavern.zoneMusic == 156);
    CHECK(tavern.ambience == 170);
    // Ironforge's Great Forge: the hall (group 66, 13193 in WMOAreaTable) with its own ambience.
    const WmoAreaHit forge = a.areas.locate(glm::dvec3(-4830.4, -1065.5, 502.5), std::nullopt);
    REQUIRE(forge.valid());
    CHECK(forge.wmoId == 208);
    CHECK(forge.groupId == 13193);
    CHECK(forge.name == "The Great Forge");
    CHECK(forge.ambience == 130);
    CHECK(forge.floorZ == doctest::Approx(502.4f).epsilon(0.01));
    // Cost: a point in Stormwind's streets (the most groups of any building), uncached.
    const Clock::time_point start = Clock::now();
    u32 found = 0;
    for (u32 i = 0; i < 1000; ++i) {
        found += a.areas.locate(glm::dvec3(-8834.0 + (i % 10), 642.0 + (i % 7), 94.0), std::nullopt).valid() ? 1 : 0;
    }
    const f64 us = secondsBetween(start, Clock::now()) * 1e6 / 1000.0;
    MESSAGE("WmoAreas::locate in Stormwind: " << us << " us per point, " << found << " of 1000 in a room");
    CHECK(us < 200.0);
    // The cache: the same answer for a point within a quarter of a yard.
    const WmoAreaHit& cached = a.areas.at(glm::dvec3(-9462.7, 16.2, 57.0), std::nullopt);
    CHECK(cached.key() == inn.key());
    CHECK(a.areas.at(glm::dvec3(-9462.6, 16.2, 57.0), std::nullopt).key() == inn.key());
}

TEST_CASE("WMO areas: the room of a group the world found (VD_PACKS_DIR)") {
    Areas a("Azeroth");
    if (!a.ready) {
        MESSAGE("VD_PACKS_DIR without worldsound.vdpk: skipping");
        return;
    }
    // The Lion's Pride Inn (placement 71414, GoldshireInn.wmo: 12 groups): every group answers with the inn's rows.
    const WmoAreaHit inn = a.areas.inGroup(71414, 5, 12); // "kitchen", where Innkeeper Farley stands
    REQUIRE(inn.valid());
    CHECK(inn.wmoId == 53);
    CHECK(inn.nameSet == 2);
    CHECK(inn.name == "Lion's Pride Inn");
    CHECK(inn.zoneMusic == 156);
    CHECK(inn.uniqueId == 71414);
    CHECK(a.areas.inGroup(71414, 11, 12).name == "Lion's Pride Inn"); // the porch (an exterior group): the root row
    // A model whose group count is not the baked one is not trusted; a group past the end or an unknown placement:
    // nothing.
    CHECK_FALSE(a.areas.inGroup(71414, 5, 13).valid());
    CHECK_FALSE(a.areas.inGroup(71414, 12, 12).valid());
    CHECK_FALSE(a.areas.inGroup(1, 0, 12).valid());
    // The same place again is the cached answer.
    CHECK(&a.areas.inGroup(1, 0, 12) == &a.areas.inGroup(1, 0, 12));
}
