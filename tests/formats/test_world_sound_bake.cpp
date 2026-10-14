// Phase 14 (sound): what worldsound.vdpk is baked from, against the 1.12.1 client: the terrain's emitters (MCSE),
// the doodads' sound events and the buildings' groups with their floors (WMOAreaTable's rooms).
#include "audio/ModelSoundRules.h"
#include "bake/WorldSoundBaker.h"
#include "core/Hash.h"
#include "formats/AdtSound.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/WorldSoundFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>

using namespace vd;

namespace {

// SoundEntries' rows as the bake sees them (exists, loops).
struct Rows {
    std::unordered_map<u32, u32> flags;
    bake::SoundRowLookup lookup() const {
        return [this](u32 id) {
            const auto it = flags.find(id);
            return bake::SoundRowInfo{it != flags.end(), it != flags.end() && (it->second & 0x200)};
        };
    }
};

Rows soundRows(const formats::MpqArchiveSet& mpq) {
    Rows rows;
    Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\SoundEntries.dbc");
    REQUIRE(bytes);
    Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
    REQUIRE(table);
    for (u32 r = 0; r < table.value()->recordCount(); ++r) {
        rows.flags[table.value()->getU32(r, 0)] = table.value()->getU32(r, 25);
    }
    return rows;
}

bake::ModelSounds soundsOf(const formats::MpqArchiveSet& mpq, const char* path, const Rows& rows) {
    Result<std::vector<u8>> bytes = mpq.read(path);
    REQUIRE(bytes);
    Result<formats::M2Model> model = formats::parseM2(bytes.value());
    REQUIRE(model);
    return bake::modelSounds(model.value(), assetPathHash(path), rows.lookup());
}

} // namespace

TEST_CASE("World sound bake: the terrain's emitters (MCSE) of the 1.12 client") {
    VD_REQUIRE_MPQ(mpq);
    // Three test emitters east of Goldshire, naming a SoundEntries row the client does not have (1487).
    Result<std::vector<u8>> bytes = mpq->read("World\\Maps\\Azeroth\\Azeroth_32_49.adt");
    REQUIRE(bytes);
    Result<std::vector<formats::AdtSoundEmitter>> emitters = formats::parseAdtSoundEmitters(bytes.value());
    REQUIRE(emitters);
    REQUIRE(emitters.value().size() == 3);
    const formats::AdtSoundEmitter& first = emitters.value()[0];
    CHECK(first.id == 103);
    CHECK(first.sound == 1487);
    CHECK(first.chunkX == 3);
    CHECK(first.chunkY == 5);
    CHECK(first.position[0] == doctest::Approx(-9256.3f).epsilon(1e-4));
    CHECK(first.startMinute == 1200);
    CHECK(first.endMinute == 2300);
    CHECK(first.loopCountMin == 1);
    CHECK(first.loopCountMax == 1);
    // The test "clock" of Azeroth_34_33: one emitter per hour (startMinute 60 h), playing the axe h times.
    bytes = mpq->read("World\\Maps\\Azeroth\\Azeroth_34_33.adt");
    REQUIRE(bytes);
    emitters = formats::parseAdtSoundEmitters(bytes.value());
    REQUIRE(emitters);
    REQUIRE(emitters.value().size() == 24);
    for (const formats::AdtSoundEmitter& e : emitters.value()) {
        CHECK(e.sound == 183); // Axe1H_WeaponWood
        CHECK(e.endMinute == 1440);
        const u32 hour = e.startMinute / 60;
        CHECK(e.loopCountMin == (hour == 0 ? 24 : hour));
        CHECK(e.loopCountMax == e.loopCountMin);
    }
    // Most tiles have none (36 in the whole client).
    bytes = mpq->read("World\\Maps\\Azeroth\\Azeroth_32_48.adt");
    REQUIRE(bytes);
    emitters = formats::parseAdtSoundEmitters(bytes.value());
    REQUIRE(emitters);
    CHECK(emitters.value().empty());
    // The light MODF scan sees the Lion's Pride Inn with its name set (2: WMOAreaTable's "Lion's Pride Inn").
    bytes = mpq->read("World\\Maps\\Azeroth\\Azeroth_31_49.adt");
    REQUIRE(bytes);
    Result<formats::AdtPlacements> wmos = formats::parseAdtPlacements(bytes.value());
    REQUIRE(wmos);
    bool inn = false;
    for (const formats::AdtWmoPlacement& p : wmos.value().placements) {
        if (p.uniqueId == 71414) {
            inn = true;
            CHECK(p.nameSet == 2);
            CHECK(normalizeAssetPath(wmos.value().wmos[p.nameId]).find("goldshireinn.wmo") != std::string::npos);
        }
    }
    CHECK(inn);
}

TEST_CASE("World sound bake: the doodads' sound events ($DSL / $DSO)") {
    VD_REQUIRE_MPQ(mpq);
    const Rows rows = soundRows(*mpq);
    // A forge's fire: a looping row at the start of its Stand animation: a loop.
    const bake::ModelSounds forge =
        soundsOf(*mpq, "World\\Generic\\DWARF\\PASSIVE DOODADS\\Forgebonfire\\Forgebonfire.m2", rows);
    REQUIRE(forge.events.size() == 1);
    CHECK(forge.events[0].sound == 3347); // CampFireSmallLoop
    CHECK(forge.events[0].kind == pack::kModelSoundLoop);
    // Bellows: two $DSL naming one-shot rows, breathing out at 0 ms and in at 1100 ms of the Stand cycle.
    const bake::ModelSounds bellows =
        soundsOf(*mpq, "World\\Generic\\Human\\Passive Doodads\\Bellows\\bellows.m2", rows);
    REQUIRE(bellows.events.size() == 2);
    CHECK(bellows.cycleMs > 1100);
    for (const pack::ModelSoundEvent& e : bellows.events) {
        CHECK(e.kind == pack::kModelSoundTimed);
        CHECK((e.sound == 8673 ? e.timeMs == 0u : e.timeMs == 1100u)); // BellowOut, BellowIN
    }
    // Banners name a row the client lacks (15721): nothing.
    CHECK(soundsOf(*mpq, "World\\Generic\\DWARF\\PASSIVE DOODADS\\Banners\\IronForgeBanner01.m2", rows).events.empty());
    // A windmill of the Tauren: its loop.
    const bake::ModelSounds windmill =
        soundsOf(*mpq, "World\\Generic\\TAUREN\\PASSIVE DOODADS\\TaurenWindmills\\TaurenWindmillA.m2", rows);
    REQUIRE(windmill.events.size() == 1);
    CHECK(windmill.events[0].sound == 1596); // TaurenWindmillRegularLoop
    // Placed on a map: the events at their world points (here the forge 300 yards from the bellows), sorted by
    // the 32-yard cells the runtime looks in.
    std::vector<pack::DoodadSound> records;
    bake::placeDoodadSounds(bellows, {1, 0, 0, -9460.0f, 0, 1, 0, 94.0f, 0, 0, 1, 56.5f}, records);
    bake::placeDoodadSounds(forge, {1, 0, 0, -9460.0f, 0, 1, 0, -206.0f, 0, 0, 1, 56.5f}, records);
    REQUIRE(records.size() == 3);
    CHECK(records[0].position[0] == doctest::Approx(-9460.0f));
    CHECK(records[0].position[2] > 60.0f); // the bellows' points are up on the model (z 5.9 and 6.3)
    CHECK(records[0].cycleMs == bellows.cycleMs);
    CHECK(records[2].kind == pack::kModelSoundLoop);
    CHECK(records[2].cell == pack::doodadSoundCell(-9460.0, -206.0));
    const std::vector<u8> blob = bake::bakeDoodadSounds(records);
    pack::DoodadSoundsHeader h;
    std::memcpy(&h, blob.data(), sizeof(h));
    REQUIRE(h.count == 3);
    const auto* sorted = reinterpret_cast<const pack::DoodadSound*>(blob.data() + sizeof(h));
    CHECK(sorted[0].cell <= sorted[1].cell);
    CHECK(sorted[1].cell <= sorted[2].cell);
    CHECK(sorted[2].sound == 3347); // the forge's cell comes last (a smaller Y: a larger column)
    // Cells: 32 yards, rows from X, columns from Y, from the map's corner.
    CHECK(pack::doodadSoundCell(0.0, 0.0) == 533u * pack::kDoodadSoundCells + 533u);
    CHECK(pack::doodadSoundCell(1e9, -1e9) == pack::kDoodadSoundCells - 1);
}

TEST_CASE("World sound bake: the Goldshire inn's groups and floors (WMOAreaTable 53)") {
    VD_REQUIRE_MPQ(mpq);
    const char* path = "World\\wmo\\Azeroth\\Buildings\\GoldshireInn\\GoldshireInn.wmo";
    Result<std::vector<u8>> bytes = mpq->read(path);
    REQUIRE(bytes);
    Result<formats::WmoRoot> root = formats::parseWmoRoot(bytes.value());
    REQUIRE(root);
    std::vector<formats::WmoGroup> groups;
    for (u32 g = 0; g < root.value().groups.size(); ++g) {
        Result<std::vector<u8>> groupBytes = mpq->read(formats::wmoGroupPath(path, g));
        REQUIRE(groupBytes);
        Result<formats::WmoGroup> group = formats::parseWmoGroup(groupBytes.value());
        REQUIRE(group);
        groups.push_back(std::move(group.value()));
    }
    const bake::WmoAreaModelData inn = bake::wmoAreaModel(root.value(), groups, assetPathHash(path));
    CHECK(inn.wmoId == 53);
    REQUIRE(inn.groups.size() == 12);
    std::set<i32> ids;
    u32 withFloor = 0;
    for (const bake::WmoAreaGroupData& g : inn.groups) {
        ids.insert(g.group.groupId);
        withFloor += std::any_of(g.cells.begin(), g.cells.end(), [](u8 c) { return c != pack::kWmoNoFloor; }) ? 1 : 0;
        CHECK(g.group.floorStep >= pack::kWmoFloorStep);
    }
    CHECK(*ids.begin() == 893); // WMOAreaTable's rows 893..904
    CHECK(*ids.rbegin() == 904);
    CHECK(withFloor == 12);
    // The floor under the middle of the first floor's hall is about the building's origin height.
    const bake::WmoAreaGroupData* hall = nullptr;
    for (const bake::WmoAreaGroupData& g : inn.groups) {
        if ((g.group.flags & 0x2000) != 0 &&
            (!hall || (g.group.boundsMax[0] - g.group.boundsMin[0]) * (g.group.boundsMax[1] - g.group.boundsMin[1]) >
                          (hall->group.boundsMax[0] - hall->group.boundsMin[0]) *
                              (hall->group.boundsMax[1] - hall->group.boundsMin[1]))) {
            hall = &g;
        }
    }
    REQUIRE(hall);
    f32 lowest = 0.0f, highest = 0.0f;
    const f32 x = (hall->group.boundsMin[0] + hall->group.boundsMax[0]) * 0.5f;
    const f32 y = (hall->group.boundsMin[1] + hall->group.boundsMax[1]) * 0.5f;
    REQUIRE(bake::wmoFloorAt(*hall, x, y, lowest, highest));
    CHECK(lowest >= hall->group.boundsMin[2] - 0.01f);
    CHECK(lowest < hall->group.boundsMin[2] + 2.0f);
    // A map's table with the inn placed: valid, the placement's world box around it.
    bake::WmoAreaPlacementData placement;
    placement.uniqueId = 71414;
    placement.nameSet = 2;
    placement.transform = {1, 0, 0, -9464.2f, 0, 1, 0, 24.4f, 0, 0, 1, 56.5f};
    const std::vector<u8> blob = bake::bakeWmoAreas({placement}, {inn});
    pack::WmoAreasHeader h;
    std::memcpy(&h, blob.data(), sizeof(h));
    CHECK(h.placementCount == 1);
    CHECK(h.groupCount == 12);
    const auto* p = reinterpret_cast<const pack::WmoAreaPlacement*>(blob.data() + h.placementsOffset);
    CHECK(p->nameSet == 2);
    CHECK(p->boundsMin[0] < -9464.2f);
    CHECK(p->boundsMax[0] > -9464.2f);
}
