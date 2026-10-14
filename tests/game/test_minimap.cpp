// The minimap (parity pass): the zoom tables, where world points fall on the map and the rim clamping, the
// blips the game state puts on it (party members, the corpse, quest givers with a quest to turn in, tracked
// creatures by type and reaction, tracked resources by their chest's lock), MSG_MINIMAP_PING byte for byte as
// vmangos-core reads and writes it (Server/Packets/Group.cpp, Handlers/GroupHandler.cpp), truncated ones
// rejected, the ping's life through game::Gameplay, the day / night dial and its clock texts, and the client
// tables (Lock.dbc, Spell.dbc's tracking spells) checked against the real data (VD_PACKS_DIR).
#include "TestData.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Minimap.h"
#include "game/MinimapData.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/MinimapMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::BlipKind;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kAeryn = 0x22; // a party member
constexpr u64 kBrakk = 0x23; // a player of the other faction
constexpr u64 kGiver = 0xF130000000000101ull;
constexpr u64 kWolf = 0xF130000000000102ull;
constexpr u64 kGuard = 0xF130000000000103ull;
constexpr u64 kHerb = 0xF110000000000201ull;
constexpr u64 kVein = 0xF110000000000202ull;
constexpr u32 kGiverEntry = 197, kWolfEntry = 299, kGuardEntry = 68, kHerbEntry = 1617, kVeinEntry = 1731;
constexpr u32 kHerbLock = 29, kVeinLock = 38; // Lock.dbc: Herbalism 0, Mining 0
constexpr u32 kLockHerbalism = 1u << 1, kLockMining = 1u << 2;
constexpr u32 kTrackBeasts = 1u << 0, kTrackHumanoids = 1u << 6;

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Gameplay over its own entity store, what it sends captured; the player at (100, 100) on map 0.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
    u32 now = 1000;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    void creature(u32 entry, u32 type, const char* name) {
        net::CreatureQueryInfo info;
        info.entry = entry;
        info.found = true;
        info.name = name;
        info.type = type;
        play.queries().onCreature(std::move(info));
    }
    void chest(u32 entry, u32 lock, const char* name) {
        net::GameObjectQueryInfo info;
        info.entry = entry;
        info.found = true;
        info.type = game::kGameObjectChest;
        info.name = name;
        info.data[0] = lock;
        play.queries().onGameObject(info);
    }
};

Fields playerFields(Fields extra = {}) {
    Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                {uf::UNIT_FIELD_BYTES_0, 1 /* human */},
                {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                {uf::UNIT_FIELD_LEVEL, 10}};
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

std::vector<game::MinimapBlip> blips(Harness& h, const game::MinimapData* data, game::BlipQuery q) {
    std::array<game::MinimapBlip, 64> out;
    const u32 n = game::collectMinimapBlips(h.store, h.play, data, nullptr, q, out);
    return {out.begin(), out.begin() + n};
}

const game::MinimapBlip* findBlip(const std::vector<game::MinimapBlip>& list, u64 guid) {
    for (const game::MinimapBlip& b : list) {
        if (b.guid == guid) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Minimap: zoom levels, outdoors and indoors") {
    // Diameters 466.7 .. 133.3 yards outdoors, 300 .. 50 indoors; the default level 3 (CVar defaults of
    // WoW.exe).
    CHECK(game::minimapRadius(false, 0) == doctest::Approx(233.333f));
    CHECK(game::minimapRadius(false, game::kMinimapDefaultZoom) == doctest::Approx(133.333f));
    CHECK(game::minimapRadius(false, 5) == doctest::Approx(66.667f));
    CHECK(game::minimapRadius(true, 0) == doctest::Approx(150.0f));
    CHECK(game::minimapRadius(true, game::kMinimapDefaultZoom) == doctest::Approx(60.0f));
    CHECK(game::minimapRadius(true, 5) == doctest::Approx(25.0f));
    CHECK(game::minimapRadius(false, 99) == game::minimapRadius(false, 5)); // clamped
    for (u32 z = 1; z < game::kMinimapZoomLevels; ++z) {
        CHECK(game::kMinimapOutdoorYards[z] < game::kMinimapOutdoorYards[z - 1]); // in = closer
        CHECK(game::kMinimapIndoorYards[z] < game::kMinimapIndoorYards[z - 1]);
    }
    // Minimap_ZoomInClick / ZoomOutClick within 0..5 (the buttons disable at the ends).
    CHECK(game::minimapZoomStep(3, 1) == 4);
    CHECK(game::minimapZoomStep(3, -1) == 2);
    CHECK(game::minimapZoomStep(5, 1) == 5);
    CHECK(game::minimapZoomStep(0, -1) == 0);
    CHECK(game::minimapZoomStep(2, 10) == 5);
    CHECK(game::minimapZoomStep(9, -1) == 4); // an out-of-range setting is clamped first
}

TEST_CASE("Minimap: world points on the map, north up and west left, clamped to the rim") {
    // World +X is north (up), +Y is west (left); offsets in radii, y down.
    game::MinimapPoint north = game::minimapPoint(100.0f, 100.0f, 100.0f, 150.0f, 100.0f);
    CHECK(north.x == doctest::Approx(0.0f));
    CHECK(north.y == doctest::Approx(-0.5f));
    CHECK(north.angle == doctest::Approx(0.0f));
    CHECK_FALSE(north.outside);
    game::MinimapPoint west = game::minimapPoint(100.0f, 100.0f, 100.0f, 100.0f, 160.0f);
    CHECK(west.x == doctest::Approx(-0.6f));
    CHECK(west.y == doctest::Approx(0.0f));
    CHECK(west.angle == doctest::Approx(-1.5707963f)); // clockwise from up: west is a quarter turn back
    game::MinimapPoint east = game::minimapPoint(100.0f, 100.0f, 100.0f, 100.0f, 70.0f);
    CHECK(east.x == doctest::Approx(0.3f));
    CHECK(east.angle == doctest::Approx(1.5707963f));
    game::MinimapPoint south = game::minimapPoint(100.0f, 100.0f, 100.0f, 20.0f, 100.0f);
    CHECK(south.y == doctest::Approx(0.8f));
    CHECK(std::fabs(south.angle) == doctest::Approx(3.1415927f));

    // Beyond the map: clamped to the rim along the same direction (the rim arrows).
    game::MinimapPoint far = game::minimapPoint(0.0f, 0.0f, 100.0f, 300.0f, -400.0f);
    CHECK(far.outside);
    CHECK(std::sqrt(far.x * far.x + far.y * far.y) == doctest::Approx(1.0f));
    CHECK(far.x == doctest::Approx(0.8f));
    CHECK(far.y == doctest::Approx(-0.6f));
    game::MinimapPoint inner = game::minimapPoint(0.0f, 0.0f, 100.0f, 300.0f, -400.0f, 0.9f);
    CHECK(inner.x == doctest::Approx(0.72f));
    CHECK(inner.y == doctest::Approx(-0.54f));
    CHECK(inner.angle == doctest::Approx(far.angle));
    // Exactly on the rim is still on the map.
    CHECK_FALSE(game::minimapPoint(0.0f, 0.0f, 100.0f, 100.0f, 0.0f).outside);
    // No radius: nothing moves (and nothing divides by zero).
    CHECK(game::minimapPoint(0.0f, 0.0f, 0.0f, 10.0f, 10.0f).x == 0.0f);

    // A tap's offset back to the world: the inverse.
    f32 x = 0.0f, y = 0.0f;
    game::minimapWorldPoint(-8913.0f, -130.0f, 133.333f, 0.25f, -0.5f, x, y);
    const game::MinimapPoint back = game::minimapPoint(-8913.0f, -130.0f, 133.333f, x, y);
    CHECK(back.x == doctest::Approx(0.25f));
    CHECK(back.y == doctest::Approx(-0.5f));
    CHECK(x == doctest::Approx(-8913.0f + 66.6665f)); // half a radius north
    CHECK(y == doctest::Approx(-130.0f - 33.33325f)); // a quarter east
}

TEST_CASE("Minimap: blip art follows the evidence") {
    CHECK(game::blipCell(BlipKind::PartyMember) == 4);     // blue
    CHECK(game::blipCell(BlipKind::QuestReward) == 0);     // "yellow dot on minimap" (DIALOG_STATUS_REWARD2)
    CHECK(game::blipCell(BlipKind::QuestRewardOld) == 1);  // "red dot on minimap" (DIALOG_STATUS_REWARD_OLD)
    CHECK(game::blipCell(BlipKind::TrackedHostile) == 1);  // red
    CHECK(game::blipCell(BlipKind::TrackedNeutral) == 0);  // yellow
    CHECK(game::blipCell(BlipKind::TrackedFriendly) == 2); // green
    CHECK(game::blipCell(BlipKind::TrackedResource) == 3); // the second yellow
    CHECK(game::blipCell(BlipKind::Corpse) == game::kNoBlipCell);
    CHECK(game::blipOnRim(BlipKind::PartyMember));
    CHECK(game::blipOnRim(BlipKind::Corpse));
    CHECK_FALSE(game::blipOnRim(BlipKind::QuestReward));
    CHECK_FALSE(game::blipOnRim(BlipKind::TrackedResource));
}

TEST_CASE("Minimap: the blips of the game state") {
    Harness h;
    game::MinimapData data;
    data.set({{kHerbLock, kLockHerbalism}, {kVeinLock, kLockMining}}, {2383, 2580, 1494});
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields(), 100.0f, 100.0f));
    // A quest giver with a quest to turn in (20 yards north), one with only a quest to give, a wolf, a guard,
    // a player of the other faction, an herb and a vein; one wolf out of the map.
    h.apply(created(kGiver, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, kGiverEntry},
                     {uf::UNIT_FIELD_HEALTH, 50},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagQuestGiver}},
                    120.0f, 100.0f));
    h.apply(created(kGuard, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, kGuardEntry},
                     {uf::UNIT_FIELD_HEALTH, 50},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagQuestGiver}},
                    90.0f, 110.0f));
    h.apply(created(kWolf, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, kWolfEntry}, {uf::UNIT_FIELD_HEALTH, 30}}, 100.0f, 60.0f));
    h.apply(created(kBrakk, game::ObjectType::Player, playerFields({{uf::UNIT_FIELD_BYTES_0, 2 /* orc */}}),
                    70.0f, 100.0f));
    h.apply(created(kHerb, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, kHerbEntry}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectChest}},
                    130.0f, 130.0f));
    h.apply(created(kVein, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, kVeinEntry}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectChest}},
                    60.0f, 60.0f));
    Bytes status;
    net::ByteWriter(status).u64v(kGiver).u32v(7); // DIALOG_STATUS_REWARD2
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_STATUS, status) == game::Gameplay::Handled::Yes);
    status.clear();
    net::ByteWriter(status).u64v(kGuard).u32v(5); // DIALOG_STATUS_AVAILABLE: no blip in 1.12
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_STATUS, status) == game::Gameplay::Handled::Yes);
    h.creature(kGiverEntry, 7, "Marshal McBride");
    h.creature(kGuardEntry, 7, "Stormwind Guard");
    h.creature(kWolfEntry, 1, "Young Wolf");
    h.chest(kHerbEntry, kHerbLock, "Peacebloom");
    h.chest(kVeinEntry, kVeinLock, "Copper Vein");

    game::BlipQuery q;
    q.x = 100.0f;
    q.y = 100.0f;
    q.radius = 133.333f;
    q.nowMs = h.now;

    // Nothing tracked: only the quest to turn in.
    std::vector<game::MinimapBlip> list = blips(h, &data, q);
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == BlipKind::QuestReward);
    CHECK(list[0].guid == kGiver);
    CHECK(list[0].x == doctest::Approx(120.0f));
    CHECK(game::minimapBlipName(h.store, h.play, list[0]) == "Marshal McBride");

    // Track Beasts: the wolf (neutral without reaction tables); Find Herbs: the herb, not the vein.
    q.trackCreatures = kTrackBeasts;
    q.trackResources = kLockHerbalism;
    list = blips(h, &data, q);
    CHECK(list.size() == 3);
    REQUIRE(findBlip(list, kWolf));
    CHECK(findBlip(list, kWolf)->kind == BlipKind::TrackedNeutral);
    REQUIRE(findBlip(list, kHerb));
    CHECK(findBlip(list, kHerb)->kind == BlipKind::TrackedResource);
    CHECK(game::minimapBlipName(h.store, h.play, *findBlip(list, kHerb)) == "Peacebloom");
    CHECK_FALSE(findBlip(list, kVein));
    CHECK_FALSE(findBlip(list, kGuard)); // a humanoid, not tracked; its "!" has no blip
    // Track Humanoids: the guard and the other player too (players are humanoids); a quest giver keeps its
    // quest blip. Find Minerals: the vein.
    q.trackCreatures = kTrackHumanoids;
    q.trackResources = kLockMining;
    list = blips(h, &data, q);
    REQUIRE(findBlip(list, kGuard));
    CHECK(findBlip(list, kGuard)->kind == BlipKind::TrackedNeutral);
    REQUIRE(findBlip(list, kBrakk));
    CHECK(findBlip(list, kGiver)->kind == BlipKind::QuestReward);
    CHECK_FALSE(findBlip(list, kWolf));
    CHECK(findBlip(list, kVein));
    CHECK_FALSE(findBlip(list, kHerb));
    // Without the tables nothing is a resource (and nothing breaks).
    CHECK_FALSE(findBlip(blips(h, nullptr, q), kVein));

    // A closer zoom leaves out what is beyond the map (the wolf at 40 yards, the herb at 42).
    q.trackCreatures = kTrackBeasts;
    q.trackResources = kLockHerbalism;
    q.radius = 35.0f;
    list = blips(h, &data, q);
    CHECK_FALSE(findBlip(list, kWolf));
    CHECK_FALSE(findBlip(list, kHerb));
    CHECK(findBlip(list, kGiver)); // 20 yards
    // Dead creatures are not tracked; a creature whose template is not known yet waits for it.
    q.radius = 133.333f;
    h.apply(values(kWolf, {{uf::UNIT_FIELD_HEALTH, 0}}));
    CHECK_FALSE(findBlip(blips(h, &data, q), kWolf));
    h.apply(values(kWolf, {{uf::UNIT_FIELD_HEALTH, 30}, {uf::OBJECT_FIELD_ENTRY, 9999}}));
    CHECK_FALSE(findBlip(blips(h, &data, q), kWolf));

    // A party member: out of sight, where the server last put it; on the rim beyond the map (the view clamps
    // it).
    Bytes list2;
    net::ByteWriter(list2)
        .u8v(0)
        .u8v(0)
        .u32v(1)
        .cstring("Aeryn")
        .u64v(kAeryn)
        .u8v(net::kMemberOnline)
        .u8v(0)
        .u64v(kPlayer)
        .u8v(3)
        .u64v(0)
        .u8v(2)
        .u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, list2) == game::Gameplay::Handled::Yes);
    Bytes stats;
    net::ByteWriter(stats)
        .packedGuid(kAeryn)
        .u32v(net::kGroupUpdateStatus | net::kGroupUpdatePosition)
        .u8v(net::kMemberOnline | net::kMemberDead)
        .u16v(static_cast<u16>(static_cast<i16>(600)))
        .u16v(static_cast<u16>(static_cast<i16>(-300)));
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS, stats) == game::Gameplay::Handled::Yes);
    list = blips(h, &data, q);
    REQUIRE(!list.empty());
    CHECK(list[0].kind == BlipKind::PartyMember); // members first
    CHECK(list[0].guid == kAeryn);
    CHECK(list[0].dead);
    CHECK(list[0].x == doctest::Approx(600.0f));
    CHECK(game::minimapPoint(q.x, q.y, q.radius, list[0].x, list[0].y).outside);
    CHECK(game::minimapBlipName(h.store, h.play, list[0]) == "Aeryn");
    // A member in sight is not also a tracked humanoid.
    h.apply(created(kAeryn, game::ObjectType::Player, playerFields(), 110.0f, 90.0f));
    q.trackCreatures = kTrackHumanoids;
    list = blips(h, &data, q);
    CHECK(std::count_if(list.begin(), list.end(),
                        [](const game::MinimapBlip& b) { return b.guid == kAeryn; }) == 1);
    CHECK(findBlip(list, kAeryn)->kind == BlipKind::PartyMember);
    CHECK(findBlip(list, kAeryn)->x == doctest::Approx(110.0f));

    // The capacity is respected.
    std::array<game::MinimapBlip, 2> two;
    CHECK(game::collectMinimapBlips(h.store, h.play, &data, nullptr, q, two) == 2);
}

TEST_CASE("Minimap: the corpse while a ghost, on this map only") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields(), 100.0f, 100.0f));
    h.apply(values(kPlayer,
                   {{uf::UNIT_FIELD_HEALTH, 0}, {uf::PLAYER_FIELD_BYTES, game::kPlayerBytesReleaseTimer}}));
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_HEALTH, 1}, {uf::PLAYER_FLAGS, game::kPlayerFlagGhost}}));
    REQUIRE(h.play.death().ghost);
    game::BlipQuery q;
    q.x = 100.0f;
    q.y = 100.0f;
    q.radius = 133.333f;
    q.nowMs = h.now;
    CHECK(blips(h, nullptr, q).empty()); // not known yet
    Bytes corpse;
    net::ByteWriter(corpse).u8v(1).u32v(0).f32v(-400.0f).f32v(100.0f).f32v(0.0f).u32v(0);
    REQUIRE(h.packet(Opcode::MSG_CORPSE_QUERY, corpse) == game::Gameplay::Handled::Yes);
    std::vector<game::MinimapBlip> list = blips(h, nullptr, q);
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == BlipKind::Corpse);
    CHECK(list[0].x == doctest::Approx(-400.0f));
    const game::MinimapPoint p = game::minimapPoint(q.x, q.y, q.radius, list[0].x, list[0].y, 0.9f);
    CHECK(p.outside); // 500 yards south: on the rim, pointing down
    CHECK(std::fabs(p.angle) == doctest::Approx(3.1415927f));
    CHECK(p.y == doctest::Approx(0.9f));
    q.map = 1; // another map: no arrow
    CHECK(blips(h, nullptr, q).empty());
}

TEST_CASE("Minimap: MSG_MINIMAP_PING as vmangos-core reads and writes it") {
    // Client -> server (MinimapPing::ReadFromWorldPacket: float x, float y): the world point.
    Bytes body;
    net::writeMinimapPing(body, -8913.5f, -130.25f);
    Bytes expected;
    net::ByteWriter(expected).f32v(-8913.5f).f32v(-130.25f);
    CHECK(body == expected);
    CHECK(body.size() == 8);

    // Server -> client (HandleMinimapPingOpcode: the sender's raw GUID, float x, float y).
    Bytes relayed;
    net::ByteWriter(relayed).u64v(kAeryn).f32v(12.5f).f32v(-7.0f);
    net::MinimapPing ping;
    REQUIRE(net::parseMinimapPing(relayed, ping));
    CHECK(ping.sender == kAeryn);
    CHECK(ping.x == 12.5f);
    CHECK(ping.y == -7.0f);
    checkTruncations(relayed, [](std::span<const u8> p) {
        net::MinimapPing out;
        return net::parseMinimapPing(p, out);
    });
    // Coordinates that are not numbers are refused.
    Bytes bad;
    net::ByteWriter(bad).u64v(kAeryn).f32v(std::numeric_limits<f32>::quiet_NaN()).f32v(1.0f);
    CHECK_FALSE(net::parseMinimapPing(bad, ping));
    bad.clear();
    net::ByteWriter(bad).u64v(kAeryn).f32v(1.0f).f32v(std::numeric_limits<f32>::infinity());
    CHECK_FALSE(net::parseMinimapPing(bad, ping));
}

TEST_CASE("Minimap: pings through Gameplay, shown five and a half seconds (the client's fade clamps away)") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields(), 100.0f, 100.0f));
    const game::Minimap& minimap = h.play.minimap();
    CHECK(minimap.lastPing().serial == 0);
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now) == 0.0f);

    // Alone: shown at once, nothing sent (the server only relays within a group).
    CHECK_FALSE(h.play.minimap().ping(110.0f, 95.0f, false, h.now));
    CHECK(h.sentWith(Opcode::MSG_MINIMAP_PING).empty());
    CHECK(minimap.lastPing().serial == 1);
    CHECK(minimap.lastPing().sender == kPlayer);
    CHECK(minimap.lastPing().x == 110.0f);
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now) == 1.0f);
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now + 4999) == 1.0f);
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now + 5250) == 1.0f); // SetAlpha(127.5): 1
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now + 5499) == doctest::Approx(0.51f));
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now + 5500) == 0.0f);

    // In a group: sent as the server reads it.
    REQUIRE(h.play.minimap().ping(-20.0f, 30.0f, true, h.now));
    const std::vector<Bytes> sent = h.sentWith(Opcode::MSG_MINIMAP_PING);
    REQUIRE(sent.size() == 1);
    Bytes expected;
    net::ByteWriter(expected).f32v(-20.0f).f32v(30.0f);
    CHECK(sent[0] == expected);
    CHECK(minimap.lastPing().serial == 2);
    // Not a number: neither shown nor sent.
    CHECK_FALSE(h.play.minimap().ping(std::numeric_limits<f32>::quiet_NaN(), 0.0f, true, h.now));
    CHECK(minimap.lastPing().serial == 2);

    // A member's ping relayed by the server.
    h.now += 1000;
    Bytes relayed;
    net::ByteWriter(relayed).u64v(kAeryn).f32v(150.0f).f32v(90.0f);
    CHECK(h.packet(Opcode::MSG_MINIMAP_PING, relayed) == game::Gameplay::Handled::Yes);
    CHECK(minimap.lastPing().serial == 3);
    CHECK(minimap.lastPing().sender == kAeryn);
    CHECK(minimap.lastPing().startMs == h.now);
    CHECK(minimap.lastPing().x == 150.0f);
    CHECK(h.packet(Opcode::MSG_MINIMAP_PING, Bytes(relayed.begin(), relayed.begin() + 12)) ==
          game::Gameplay::Handled::Malformed);
    CHECK(minimap.lastPing().serial == 3);

    // A new world: the old ping is gone (its serial keeps counting).
    h.play.enterWorld(kPlayer, 1, false, h.now);
    CHECK(game::minimapPingAlpha(minimap.lastPing(), h.now) == 0.0f);
    CHECK(minimap.lastPing().serial == 3);
}

TEST_CASE("Minimap: the day / night dial and its clock") {
    // GameTime.lua: the moon from 21:00 to 5:30.
    CHECK(game::gameTimeNight(0));
    CHECK(game::gameTimeNight(5 * 60 + 29));
    CHECK_FALSE(game::gameTimeNight(5 * 60 + 30));
    CHECK_FALSE(game::gameTimeNight(12 * 60));
    CHECK_FALSE(game::gameTimeNight(20 * 60 + 59));
    CHECK(game::gameTimeNight(21 * 60));
    CHECK(game::gameTimeNight(23 * 60 + 59));
    CHECK(game::gameTimeNight(24 * 60 + 60)); // a day later: 1:00

    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping the clock texts");
        return;
    }
    char buffer[32];
    CHECK(game::gameTimeText(buffer, 21 * 60 + 5, false) == "9:05 PM");
    CHECK(game::gameTimeText(buffer, 0, false) == "12:00 AM");
    CHECK(game::gameTimeText(buffer, 12 * 60 + 30, false) == "12:30 PM");
    CHECK(game::gameTimeText(buffer, 11 * 60 + 59, false) == "11:59 AM");
    CHECK(game::gameTimeText(buffer, 21 * 60 + 5, true) == "21:05");
    CHECK(game::gameTimeText(buffer, 7, true) == "0:07");
    CHECK(text::get("VD_MINIMAP_TWENTYFOUR_HOUR_TIME") == "0"); // English: AM / PM
    CHECK(text::get("CORPSE") == "Corpse");
    CHECK(text::get("HAVE_MAIL") == "You have unread mail");
}

TEST_CASE("Minimap: Lock.dbc and the tracking spells of the real data") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::MinimapData data;
    REQUIRE(data.init(db));
    CHECK(data.lockTypeMask(kHerbLock) == kLockHerbalism); // Peacebloom: Herbalism
    CHECK(data.lockTypeMask(kVeinLock) == kLockMining);    // Copper Vein: Mining
    CHECK(data.lockTypeMask(57) ==
          ((1u << 4) | (1u << 5)));    // a chest: Open and Treasure (Find Treasure's type 6)
    CHECK(data.lockTypeMask(15) == 0); // a key item: no skill
    CHECK(data.lockTypeMask(999999) == 0);
    // Find Herbs, Find Minerals, Find Treasure, Track Beasts, Track Humanoids (druid), Sense Undead: tracking
    // spells.
    for (const u32 spell : {2383u, 8387u, 2580u, 2481u, 1494u, 5225u, 5502u, 19883u}) {
        CHECK(data.isTrackingSpell(spell));
    }
    CHECK_FALSE(data.isTrackingSpell(133));   // Fireball
    CHECK_FALSE(data.isTrackingSpell(19885)); // Track Hidden: aura 151 (stealth), not a minimap tracking
    CHECK(data.trackingSpellCount() >= 30);
}
