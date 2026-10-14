// Travel (phase 8): the travel packets byte for byte as vmangos-core writes / reads them
// (Server/Packets/Misc, Taxi, Npc, Movement), truncated ones rejected; area trigger shapes and the enter-once
// logic; flight routes and their cost; the travel flows through game::Gameplay with a capturing sink
// (exploration, the taxi map and a flight, innkeepers, far teleports); and the client tables (AreaTable,
// AreaTrigger, Map, WorldMapArea, WorldMapOverlay, TaxiNodes, TaxiPath) checked against the real data
// (VD_PACKS_DIR); the texts the player reads, by key and in English.
#include "TestData.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "game/AreaData.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/TaxiData.h"
#include "game/Transports.h"
#include "game/Travel.h"
#include "game/UnitInfo.h"
#include "game/WorldMapData.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/TravelMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kFlightMaster = 0xF130000000000352ull; // a unit GUID (HIGHGUID_UNIT)
constexpr u64 kInnkeeper = 0xF130000000000295ull;

// Every prefix shorter than the whole payload must be rejected by `parse`.
template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

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
Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f,
              f32 z = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(z).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Gameplay (and its Travel) over its own entity store, what it sends captured; small hand-made tables.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::AreaData areas;
    game::TaxiData taxi;
    std::vector<Sent> sent;
    u32 now = 1000;
    u32 eventCursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        // Elwynn Forest with Goldshire (Alliance), Stranglethorn with Booty Bay (contested), the Battle Ring
        // (free-for-all) and a dungeon.
        game::AreaInfo elwynn{12, 0, 0, 126, 0x40, 0, game::kAreaTeamAlliance, "Elwynn Forest"};
        game::AreaInfo goldshire{87, 0, 12, 548, 0x40, 5, 0, "Goldshire"};
        game::AreaInfo stranglethorn{33, 0, 0, 140, 0x40, 0, 0, "Stranglethorn Vale"};
        game::AreaInfo ring{2177, 0, 33, 819, 0xD0, 0, 0, "Battle Ring"};
        game::AreaInfo deadmines{1581, 36, 0, 900, 0, 0, 0, "The Deadmines"};
        game::AreaTriggerInfo inn{562, 0, -9462.0f, 16.0f, 50.0f, 0.0f, 20.0f, 10.0f, 8.0f, 0.0f};
        game::AreaTriggerInfo portal{78, 0, -11208.5f, 1685.3f, 25.8f, 7.0f};
        game::MapInfo azeroth{0, "Azeroth", "Eastern Kingdoms", 0, 0, assetPathHash("x.blp")};
        game::MapInfo dungeon{36, "DeadminesInstance", "Deadmines", 1, 0, 0};
        areas.set({elwynn, goldshire, stranglethorn, ring, deadmines}, {inn, portal}, {azeroth, dungeon});
        // Stormwind (2) - Sentinel Hill (4) - Booty Bay (19), and Stormwind - Lakeshire (5).
        std::vector<game::TaxiNodeInfo> nodes = {
            {2, 0, -8840.0f, 489.0f, 109.0f, "Stormwind, Elwynn", 0, 541},
            {4, 0, -10628.0f, 1036.0f, 34.0f, "Sentinel Hill, Westfall", 0, 541},
            {5, 0, -9429.0f, -2231.0f, 68.0f, "Lakeshire, Redridge", 0, 541},
            {19, 0, -14473.0f, 464.0f, 36.0f, "Booty Bay, Stranglethorn", 0, 541}};
        std::vector<game::TaxiPathInfo> paths = {
            {6, 2, 4, 110}, {7, 4, 2, 110}, {8, 2, 5, 210}, {50, 4, 19, 410}, {51, 19, 4, 410}};
        std::vector<game::TaxiPathPoint> points;
        auto line = [&](u32 path, glm::vec3 a, glm::vec3 b) {
            points.push_back({path, 0, 0, a});
            points.push_back({path, 1, 0, (a + b) * 0.5f});
            points.push_back({path, 2, 0, b});
        };
        line(6, {-8840, 489, 109}, {-10628, 1036, 34});
        line(7, {-10628, 1036, 34}, {-8840, 489, 109});
        line(8, {-8840, 489, 109}, {-9429, -2231, 68});
        line(50, {-10628, 1036, 34}, {-14473, 464, 36});
        line(51, {-14473, 464, 36}, {-10628, 1036, 34});
        taxi.set(std::move(nodes), std::move(paths), std::move(points));
        play.travel().setData(&areas, &taxi);
        play.enterWorld(kPlayer, 0, true, now);
        apply(Opcode::SMSG_UPDATE_OBJECT,
              created(kPlayer, game::ObjectType::Player,
                      {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}}));
        eventCursor = play.events().end();
    }

    game::Gameplay::Handled packet(Opcode opcode, const Bytes& payload) {
        return play.handle(opcode, payload, now);
    }
    void apply(Opcode opcode, const Bytes& payload) {
        if (opcode == Opcode::SMSG_UPDATE_OBJECT) {
            std::vector<u64> guids;
            game::UpdateStats stats;
            stats.createdGuids = &guids;
            REQUIRE(store.applyUpdateObject(payload, now, &stats));
            play.afterUpdate(guids, now);
        } else {
            REQUIRE(packet(opcode, payload) == game::Gameplay::Handled::Yes);
        }
    }
    const game::GameEvent* nextEvent(GameEventKind kind) {
        while (const game::GameEvent* e = play.events().next(eventCursor)) {
            if (e->kind == kind) {
                return e;
            }
        }
        return nullptr;
    }
    u32 count(Opcode opcode) const {
        return static_cast<u32>(
            std::count_if(sent.begin(), sent.end(), [&](const Sent& s) { return s.opcode == opcode; }));
    }
};

} // namespace

TEST_CASE("Travel packets parse as vmangos writes them and reject truncation") {
    Bytes b;
    net::ByteWriter w(b);

    // ExplorationExperience::AppendBodyTo: area, experience.
    w.u32v(87).u32v(55);
    net::ExplorationExperience explored;
    REQUIRE(net::parseExplorationExperience(b, explored));
    CHECK(explored.areaId == 87);
    CHECK(explored.experience == 55);
    checkTruncations(b, [](std::span<const u8> p) {
        net::ExplorationExperience e;
        return net::parseExplorationExperience(p, e);
    });

    // ShowTaxiNodes::AppendBodyTo: u32 1, guid, current node, 8 mask words.
    b.clear();
    w.u32v(1).u64v(kFlightMaster).u32v(2);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(i == 0 ? 0x14u : 0u);
    }
    net::ShowTaxiNodes nodes;
    REQUIRE(net::parseShowTaxiNodes(b, nodes));
    CHECK(nodes.showUi == 1);
    CHECK(nodes.flightMaster == kFlightMaster);
    CHECK(nodes.currentNode == 2);
    CHECK(nodes.knownNodes[0] == 0x14);
    CHECK(b.size() == 48);
    checkTruncations(b, [](std::span<const u8> p) {
        net::ShowTaxiNodes n;
        return net::parseShowTaxiNodes(p, n);
    });

    // TaxiNodeStatus::AppendBodyTo: guid, bool.
    b.clear();
    w.u64v(kFlightMaster).u8v(1);
    u64 guid = 0;
    bool known = false;
    REQUIRE(net::parseTaxiNodeStatus(b, guid, known));
    CHECK(guid == kFlightMaster);
    CHECK(known);
    checkTruncations(b, [](std::span<const u8> p) {
        u64 g;
        bool k;
        return net::parseTaxiNodeStatus(p, g, k);
    });

    // ActivateTaxiReply: u32.
    b.clear();
    w.u32v(3);
    u32 reply = 0;
    REQUIRE(net::parseActivateTaxiReply(b, reply));
    CHECK(reply == static_cast<u32>(net::TaxiReply::NotEnoughMoney));
    CHECK(std::string_view(net::taxiReplyName(reply)) == "NOT_ENOUGH_MONEY");
    checkTruncations(b, [](std::span<const u8> p) {
        u32 r;
        return net::parseActivateTaxiReply(p, r);
    });

    // BindpointUpdate::AppendBodyTo: x, y, z, map, area.
    b.clear();
    w.f32v(-9459.0f).f32v(42.0f).f32v(57.0f).u32v(0).u32v(87);
    net::BindPointUpdate bind;
    REQUIRE(net::parseBindPointUpdate(b, bind));
    CHECK(bind.x == -9459.0f);
    CHECK(bind.y == 42.0f);
    CHECK(bind.z == 57.0f);
    CHECK(bind.map == 0);
    CHECK(bind.areaId == 87);
    checkTruncations(b, [](std::span<const u8> p) {
        net::BindPointUpdate u;
        return net::parseBindPointUpdate(p, u);
    });

    // PlayerBound::AppendBodyTo: binder guid, area.
    b.clear();
    w.u64v(kInnkeeper).u32v(87);
    u64 binder = 0;
    u32 area = 0;
    REQUIRE(net::parsePlayerBound(b, binder, area));
    CHECK(binder == kInnkeeper);
    CHECK(area == 87);
    checkTruncations(b, [](std::span<const u8> p) {
        u64 g;
        u32 a;
        return net::parsePlayerBound(p, g, a);
    });

    // TransferPending::AppendBodyTo: map (+ transport entry, old map on a transport).
    b.clear();
    w.u32v(1);
    net::TransferPending pending;
    REQUIRE(net::parseTransferPending(b, pending));
    CHECK(pending.map == 1);
    CHECK_FALSE(pending.onTransport);
    w.u32v(164871).u32v(0);
    REQUIRE(net::parseTransferPending(b, pending));
    CHECK(pending.onTransport);
    CHECK(pending.transportEntry == 164871);
    CHECK(pending.oldMap == 0);
    CHECK_FALSE(net::parseTransferPending(Bytes{1, 0}, pending));

    // TransferAborted: u8.
    u8 reason = 0;
    REQUIRE(net::parseTransferAborted(Bytes{1}, reason));
    CHECK(reason == net::kTransferAbortMaxPlayers);
    CHECK_FALSE(net::parseTransferAborted(Bytes{}, reason));
}

TEST_CASE("Travel client packets are written as vmangos reads them") {
    Bytes b;
    net::writeAreaTrigger(b, 562);
    CHECK(b == Bytes{0x32, 0x02, 0, 0});

    // Taxi::ActivateTaxi::ReadFromWorldPacket: guid, node1, node2.
    b.clear();
    net::writeActivateTaxi(b, kFlightMaster, 2, 4);
    Bytes expected;
    net::ByteWriter(expected).u64v(kFlightMaster).u32v(2).u32v(4);
    CHECK(b == expected);

    // Taxi::ActivateTaxiExpress::ReadFromWorldPacket: guid, total cost, count, nodes.
    b.clear();
    const u32 route[] = {2, 4, 19};
    net::writeActivateTaxiExpress(b, kFlightMaster, 520, route);
    expected.clear();
    net::ByteWriter(expected).u64v(kFlightMaster).u32v(520).u32v(3).u32v(2).u32v(4).u32v(19);
    CHECK(b == expected);

    // Movement::MoveSplineDone::ReadFromWorldPacket: MovementInfo, spline id, a skipped float.
    b.clear();
    net::MovementInfo info;
    info.time = 1234;
    info.position = {-10628.0f, 1036.0f, 34.0f, 1.5f};
    net::writeMoveSplineDone(b, info, 77);
    expected.clear();
    net::ByteWriter e(expected);
    net::writeMovementInfo(e, info);
    e.u32v(77).f32v(0.0f);
    CHECK(b == expected);
    CHECK(b.size() == 4 + 4 + 16 + 4 + 8);
}

TEST_CASE("Area triggers: spheres and turned boxes as the server tests them") {
    game::AreaTriggerInfo sphere{1, 0, 10.0f, 20.0f, 30.0f, 5.0f};
    CHECK(sphere.contains(10.0f, 20.0f, 30.0f));
    CHECK(sphere.contains(14.9f, 20.0f, 30.0f));
    CHECK_FALSE(sphere.contains(15.1f, 20.0f, 30.0f));
    CHECK(sphere.contains(15.1f, 20.0f, 30.0f, 5.0f)); // the server's tolerance
    CHECK_FALSE(sphere.contains(10.0f, 20.0f, 36.0f));

    // A box 20 long (x) and 4 wide (y), turned 90 degrees: it now runs along y.
    game::AreaTriggerInfo box{2, 0, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 4.0f, 6.0f, 1.5707964f};
    CHECK(box.contains(0.0f, 9.0f, 0.0f));
    CHECK_FALSE(box.contains(9.0f, 0.0f, 0.0f));
    CHECK(box.contains(1.9f, -9.5f, 2.9f));
    CHECK_FALSE(box.contains(0.0f, 0.0f, 3.5f));
    game::AreaTriggerInfo straight{3, 0, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 4.0f, 6.0f, 0.0f};
    CHECK(straight.contains(9.0f, 0.0f, 0.0f));
    CHECK_FALSE(straight.contains(0.0f, 9.0f, 0.0f));
}

TEST_CASE("Travel: area triggers are sent once per entry and never while the player cannot") {
    Harness h;
    game::Travel& travel = h.play.travel();
    const glm::vec3 outside(-9462.0f, 40.0f, 50.0f);
    const glm::vec3 inn(-9462.0f, 16.0f, 50.0f);

    travel.updatePosition(0, outside, 87, true, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 0);
    // Waiting for the ground (or on a taxi): nothing.
    travel.updatePosition(0, inn, 87, false, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 0);
    // In: once, and only once while inside.
    for (u32 i = 0; i < 30; ++i) {
        h.now += 16;
        travel.updatePosition(0, inn + glm::vec3(0.0f, static_cast<f32>(i % 3), 0.0f), 87, true, h.now);
    }
    REQUIRE(h.count(Opcode::CMSG_AREATRIGGER) == 1);
    CHECK(h.sent.back().body == Bytes{0x32, 0x02, 0, 0});
    CHECK(travel.insideTriggers().size() == 1);
    CHECK(travel.triggersSent() == 1);
    const game::GameEvent* e = h.nextEvent(GameEventKind::AreaTriggerSent);
    REQUIRE(e);
    CHECK(e->id == 562);

    // On the border (the box is 10 yards wide: y 11..21): leaving needs a yard more.
    h.now += 16;
    travel.updatePosition(0, glm::vec3(-9462.0f, 21.5f, 50.0f), 87, true, h.now);
    CHECK(travel.insideTriggers().size() == 1);
    // Out and back in at once: not again within kTriggerRepeatMs.
    h.now += 16;
    travel.updatePosition(0, outside, 87, true, h.now);
    CHECK(travel.insideTriggers().empty());
    h.now += 16;
    travel.updatePosition(0, inn, 87, true, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 1);
    // Out and back in later: a new entry.
    h.now += 16;
    travel.updatePosition(0, outside, 87, true, h.now);
    h.now += game::Travel::kTriggerRepeatMs + 1;
    travel.updatePosition(0, inn, 87, true, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 2);

    // A sphere (an instance portal) on the same map.
    travel.updatePosition(0, glm::vec3(-11208.5f, 1689.0f, 25.8f), 33, true, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 3);
    CHECK(h.sent.back().body == Bytes{78, 0, 0, 0});
    // Another map: its triggers only.
    travel.updatePosition(36, glm::vec3(-9462.0f, 16.0f, 50.0f), 0, true, h.now);
    CHECK(h.count(Opcode::CMSG_AREATRIGGER) == 3);
    CHECK(travel.insideTriggers().empty());
}

TEST_CASE("Travel: the zone under the player, its PvP color and exploration") {
    Harness h;
    game::Travel& travel = h.play.travel();
    travel.updatePosition(0, glm::vec3(0.0f), 87, true, h.now);
    const game::ZoneState& zone = travel.zone();
    CHECK(zone.area == 87);
    CHECK(zone.zone == 12);
    CHECK(zone.zoneName == "Elwynn Forest");
    CHECK(zone.subzoneName == "Goldshire");
    CHECK(zone.pvp == game::ZonePvp::Friendly); // Goldshire has no team: Elwynn's (Alliance)
    CHECK(zone.zoneChanged);
    const game::GameEvent* e = h.nextEvent(GameEventKind::ZoneChanged);
    REQUIRE(e);
    CHECK(e->id == 12);
    CHECK(e->value[0] == 87);
    CHECK(e->value[1] == 1);

    // Into the zone itself: a subzone change only.
    const u32 changes = zone.changes;
    travel.updatePosition(0, glm::vec3(0.0f), 12, true, h.now);
    CHECK(zone.changes == changes + 1);
    CHECK_FALSE(zone.zoneChanged);
    CHECK(zone.subzoneName.empty());
    // An unknown area (its tile not loaded): the last one stays.
    travel.updatePosition(0, glm::vec3(0.0f), 0, true, h.now);
    CHECK(zone.area == 12);
    CHECK(zone.changes == changes + 1);
    // Contested, free-for-all, a dungeon (no terrain: its map's zone).
    travel.updatePosition(0, glm::vec3(0.0f), 33, true, h.now);
    CHECK(zone.pvp == game::ZonePvp::Contested);
    travel.updatePosition(0, glm::vec3(0.0f), 2177, true, h.now);
    CHECK(zone.pvp == game::ZonePvp::Arena);
    CHECK(zone.subzoneName == "Battle Ring");
    travel.updatePosition(36, glm::vec3(0.0f), 0, true, h.now);
    CHECK(zone.zone == 1581);
    CHECK(zone.zoneName == "The Deadmines");
    CHECK(zone.pvp == game::ZonePvp::Normal);
    // A Horde player sees Elwynn as hostile.
    CHECK(h.areas.pvp(87, game::kAreaTeamHorde) == game::ZonePvp::Hostile);

    // SMSG_EXPLORATION_EXPERIENCE, and the explored bits.
    Bytes b;
    net::ByteWriter(b).u32v(87).u32v(55);
    h.apply(Opcode::SMSG_EXPLORATION_EXPERIENCE, b);
    e = h.nextEvent(GameEventKind::AreaExplored);
    REQUIRE(e);
    CHECK(e->id == 87);
    CHECK(e->value[0] == 55);
    CHECK_FALSE(travel.explored(87));
    Bytes update;
    net::ByteWriter w(update);
    w.u32v(1).u8v(0).u8v(0).packedGuid(kPlayer);
    writeValues(w, {{static_cast<u16>(uf::PLAYER_EXPLORED_ZONES_1 + 548 / 32), 1u << (548 % 32)}});
    h.apply(Opcode::SMSG_UPDATE_OBJECT, update);
    CHECK(travel.explored(87));
    CHECK_FALSE(travel.explored(12));
}

TEST_CASE("Taxi routes: the shortest flight through known nodes and its price") {
    Harness h;
    game::TaxiMask known{};
    for (const u32 node : {2u, 4u, 5u, 19u}) {
        known[node / 32] |= 1u << (node % 32);
    }
    game::TaxiRoute route;
    REQUIRE(h.taxi.route(2, 4, known, route));
    CHECK(route.count == 2);
    CHECK(route.cost == 110);
    REQUIRE(h.taxi.route(2, 19, known, route));
    REQUIRE(route.count == 3);
    CHECK(route.nodes[0] == 2);
    CHECK(route.nodes[1] == 4);
    CHECK(route.nodes[2] == 19);
    CHECK(route.cost == 520);
    CHECK(route.length > 5000.0f);
    // Sentinel Hill unknown: no way to Booty Bay; Lakeshire has no way back.
    game::TaxiMask partial = known;
    partial[0] &= ~(1u << 4);
    CHECK_FALSE(h.taxi.route(2, 19, partial, route));
    CHECK_FALSE(h.taxi.route(5, 2, known, route));
    CHECK_FALSE(h.taxi.route(2, 2, known, route));
}

TEST_CASE("Travel: the taxi map, a flight, and the replies") {
    Harness h;
    game::Travel& travel = h.play.travel();
    // A flight master appears: its node is asked about.
    h.apply(Opcode::SMSG_UPDATE_OBJECT,
            created(kFlightMaster, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagFlightMaster}},
                    -8840.0f, 489.0f, 109.0f));
    REQUIRE(h.count(Opcode::CMSG_TAXINODE_STATUS_QUERY) == 1);
    Bytes guid;
    net::writeGuid(guid, kFlightMaster);
    CHECK(h.sent.back().body == guid);
    CHECK(travel.taxiStatus(kFlightMaster) == game::TaxiNodeStatus::Unknown);
    Bytes b;
    net::ByteWriter(b).u64v(kFlightMaster).u8v(0);
    h.apply(Opcode::SMSG_TAXINODE_STATUS, b);
    CHECK(travel.taxiStatus(kFlightMaster) == game::TaxiNodeStatus::NotLearned);

    // Without a gossip menu, a tap asks for its map.
    CHECK(h.play.interact(kFlightMaster) == game::Interaction::FlightMaster);
    CHECK(h.sent.back().opcode == Opcode::CMSG_TAXIQUERYAVAILABLENODES);
    CHECK(h.sent.back().body == guid);
    // First time: "new flight path".
    h.apply(Opcode::SMSG_NEW_TAXI_PATH, {});
    CHECK(h.nextEvent(GameEventKind::TaxiPathLearned));

    // The map: Stormwind, knowing Sentinel Hill and Booty Bay.
    b.clear();
    net::ByteWriter w(b);
    w.u32v(1).u64v(kFlightMaster).u32v(2).u32v((1u << 4) | (1u << 19));
    for (u32 i = 1; i < 8; ++i) {
        w.u32v(0);
    }
    h.apply(Opcode::SMSG_SHOWTAXINODES, b);
    const game::TaxiMapState& map = travel.taxiMap();
    CHECK(map.open);
    CHECK(map.currentNode == 2);
    CHECK(game::taxiNodeKnown(map.known, 2)); // its own node counts as known
    CHECK(travel.taxiStatus(kFlightMaster) == game::TaxiNodeStatus::Learned);
    CHECK(h.nextEvent(GameEventKind::TaxiMapOpened));

    // One path: CMSG_ACTIVATETAXI.
    game::TaxiRoute route;
    REQUIRE(travel.routeTo(4, route));
    REQUIRE(travel.takeFlight(route));
    CHECK(h.sent.back().opcode == Opcode::CMSG_ACTIVATETAXI);
    Bytes expected;
    net::writeActivateTaxi(expected, kFlightMaster, 2, 4);
    CHECK(h.sent.back().body == expected);
    // Two: CMSG_ACTIVATETAXIEXPRESS with the whole route.
    REQUIRE(travel.routeTo(19, route));
    REQUIRE(travel.takeFlight(route));
    CHECK(h.sent.back().opcode == Opcode::CMSG_ACTIVATETAXIEXPRESS);
    expected.clear();
    const u32 nodes[] = {2, 4, 19};
    net::writeActivateTaxiExpress(expected, kFlightMaster, 520, nodes);
    CHECK(h.sent.back().body == expected);
    // Lakeshire is not known.
    CHECK_FALSE(travel.routeTo(5, route));

    // An error keeps the map open; OK closes it.
    b.clear();
    net::ByteWriter(b).u32v(static_cast<u32>(net::TaxiReply::NotEnoughMoney));
    h.apply(Opcode::SMSG_ACTIVATETAXIREPLY, b);
    const game::GameEvent* e = h.nextEvent(GameEventKind::TaxiReply);
    REQUIRE(e);
    CHECK(e->value[0] == 3);
    CHECK(map.open);
    b.clear();
    net::ByteWriter(b).u32v(0);
    h.apply(Opcode::SMSG_ACTIVATETAXIREPLY, b);
    CHECK_FALSE(map.open);
    CHECK_FALSE(travel.takeFlight(route)); // no map, no flight

    // On the taxi: the unit flag.
    CHECK_FALSE(travel.flying());
    Bytes flags;
    net::ByteWriter fw(flags);
    fw.u32v(1).u8v(0).u8v(0).packedGuid(kPlayer);
    writeValues(fw, {{uf::UNIT_FIELD_FLAGS, game::kUnitFlagTaxiFlight | 0x4}});
    h.apply(Opcode::SMSG_UPDATE_OBJECT, flags);
    CHECK(travel.flying());

    // Malformed packets are reported, not applied.
    CHECK(h.packet(Opcode::SMSG_SHOWTAXINODES, Bytes{1, 0, 0}) == game::Gameplay::Handled::Malformed);
    CHECK(h.packet(Opcode::SMSG_TAXINODE_STATUS, Bytes{1}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Travel: innkeepers bind after a confirmation; far teleports name their map") {
    Harness h;
    game::Travel& travel = h.play.travel();
    Bytes guid;
    net::writeGuid(guid, kInnkeeper);
    h.apply(Opcode::SMSG_BINDER_CONFIRM, guid);
    CHECK(travel.binderAsking() == kInnkeeper);
    const game::GameEvent* e = h.nextEvent(GameEventKind::BinderConfirm);
    REQUIRE(e);
    CHECK(e->guid == kInnkeeper);
    // No: nothing is sent.
    travel.answerBinder(false);
    CHECK(travel.binderAsking() == 0);
    CHECK(h.count(Opcode::CMSG_BINDER_ACTIVATE) == 0);
    // Yes: CMSG_BINDER_ACTIVATE with the innkeeper.
    h.apply(Opcode::SMSG_BINDER_CONFIRM, guid);
    travel.answerBinder(true);
    REQUIRE(h.count(Opcode::CMSG_BINDER_ACTIVATE) == 1);
    CHECK(h.sent.back().body == guid);
    // The bind spell's answers.
    Bytes b;
    net::ByteWriter(b).f32v(-9459.0f).f32v(42.0f).f32v(57.0f).u32v(0).u32v(87);
    h.apply(Opcode::SMSG_BINDPOINTUPDATE, b);
    CHECK(travel.bindPoint().known);
    CHECK(travel.bindPoint().areaId == 87);
    b.clear();
    net::ByteWriter(b).u64v(kInnkeeper).u32v(87);
    h.apply(Opcode::SMSG_PLAYERBOUND, b);
    e = h.nextEvent(GameEventKind::PlayerBound);
    REQUIRE(e);
    CHECK(e->id == 87);

    // SMSG_TRANSFER_PENDING: the map of the loading screen; aborted: an event with the reason.
    b.clear();
    net::ByteWriter(b).u32v(1);
    h.apply(Opcode::SMSG_TRANSFER_PENDING, b);
    CHECK(travel.transferPending());
    CHECK(travel.transferMap() == 1);
    h.apply(Opcode::SMSG_TRANSFER_ABORTED, Bytes{2});
    CHECK_FALSE(travel.transferPending());
    e = h.nextEvent(GameEventKind::TransferAborted);
    REQUIRE(e);
    CHECK(e->value[0] == net::kTransferAbortNotFound);
}

TEST_CASE("Travel tables match the 1.12.1 client data") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::AreaData areas;
    REQUIRE(areas.init(db));
    const game::AreaInfo* goldshire = areas.area(87);
    REQUIRE(goldshire);
    CHECK(goldshire->name == "Goldshire");
    CHECK(goldshire->map == 0);
    CHECK(goldshire->parent == 12);
    CHECK(goldshire->exploreBit == 548);
    CHECK(goldshire->level == 5);
    CHECK(areas.zoneOf(87) == 12);
    CHECK(areas.area(12)->name == "Elwynn Forest");
    CHECK(areas.area(12)->team == game::kAreaTeamAlliance);
    CHECK(areas.area(14)->team == game::kAreaTeamHorde); // Durotar
    CHECK(areas.pvp(87, game::kAreaTeamAlliance) == game::ZonePvp::Friendly);
    CHECK(areas.pvp(87, game::kAreaTeamHorde) == game::ZonePvp::Hostile);
    CHECK(areas.pvp(35, game::kAreaTeamAlliance) == game::ZonePvp::Contested); // Booty Bay
    CHECK(areas.pvp(2177, game::kAreaTeamHorde) == game::ZonePvp::Arena);      // the Gurubashi ring
    CHECK((areas.area(1519)->flags & game::kAreaFlagCapital) != 0);            // Stormwind City
    CHECK(areas.areaByExploreBit(548, 0)->id == 87);
    // The Deadmines' instance portal (trigger 78) in Westfall, and Map.dbc's art and names.
    const game::AreaTriggerInfo* deadmines = areas.trigger(78);
    REQUIRE(deadmines);
    CHECK(deadmines->map == 0);
    CHECK(deadmines->radius == doctest::Approx(7.0f));
    CHECK(deadmines->contains(-11208.5f, 1685.3f, 25.8f));
    CHECK(areas.triggers(0).size() > 100);
    const game::MapInfo* kalimdor = areas.map(1);
    REQUIRE(kalimdor);
    CHECK(kalimdor->directory == "Kalimdor");
    CHECK(kalimdor->loadingScreen ==
          assetPathHash("Interface\\Glues\\LoadingScreens\\LoadScreenKalimdor.blp"));
    CHECK(areas.map(36)->instanceType == 1);
    CHECK(areas.map(36)->loadingScreen ==
          assetPathHash("Interface\\Glues\\LoadingScreens\\LoadScreenDeadmines.blp"));
    CHECK(areas.mapByDirectory("azeroth")->id == 0);
    CHECK(areas.area(areas.mapZone(36))->map == 36);
    // Phase 20 (retouches): GameTips.dbc's 74 tips, in row order, without the line breaks that end most.
    REQUIRE(areas.gameTips().size() == 74);
    CHECK(areas.gameTips()[0].starts_with("|cffffd100Tip:|r "));
    CHECK(areas.gameTips()[0].find("questgivers") != std::string_view::npos);
    CHECK(areas.gameTips()[73].starts_with("|cffffd100Tip:|r "));
    CHECK(areas.gameTips()[73].find("tips off") != std::string_view::npos);
    for (const std::string_view tip : areas.gameTips()) {
        CHECK(tip.find_first_of("\r\n") == std::string_view::npos);
    }

    game::WorldMapData maps;
    REQUIRE(maps.init(db));
    const game::WorldMapArea* elwynn = maps.zoneMap(12);
    REQUIRE(elwynn);
    CHECK(elwynn->directory == "Elwynn");
    CHECK(elwynn->map == 0);
    // Goldshire's inn is about 43% across and 66% down the Elwynn map (the client shows 43, 66).
    const glm::vec2 inn = elwynn->toMap(-9459.0, 42.0);
    CHECK(inn.x == doctest::Approx(0.4303).epsilon(0.01));
    CHECK(inn.y == doctest::Approx(0.6565).epsilon(0.01));
    const glm::dvec2 back = elwynn->toWorld(inn);
    CHECK(back.x == doctest::Approx(-9459.0).epsilon(1e-4));
    CHECK(back.y == doctest::Approx(42.0).epsilon(1e-3));
    CHECK(game::WorldMapData::tileTexture("Elwynn", 1) ==
          assetPathHash("Interface\\WorldMap\\Elwynn\\Elwynn1.blp"));
    CHECK(game::WorldMapData::overlayTexture("Elwynn", "GOLDSHIRE", 1) ==
          assetPathHash("Interface\\WorldMap\\Elwynn\\Goldshire1.blp"));
    bool goldshireOverlay = false;
    for (const game::WorldMapOverlay& o : maps.overlays(elwynn->id)) {
        goldshireOverlay =
            goldshireOverlay || std::find(o.areas.begin(), o.areas.end(), 87u) != o.areas.end();
    }
    CHECK(goldshireOverlay);
    const game::WorldMapArea* azeroth = maps.continentMap(0);
    REQUIRE(azeroth);
    CHECK(azeroth->directory == "Azeroth");
    // Tapping Goldshire on the continent finds Elwynn (Stormwind's own map is smaller, but elsewhere).
    const glm::vec2 onContinent = azeroth->toMap(-9459.0, 42.0);
    const game::WorldMapArea* tapped = maps.zoneAt(0, onContinent);
    REQUIRE(tapped);
    CHECK(tapped->area == 12);
    CHECK(maps.zoneAt(0, azeroth->toMap(-8913.0, 554.0))->area == 1519); // Stormwind City
    CHECK(maps.zoneMap(1637)->directory == "Ogrimmar");
    const game::WorldMapContinent* east = maps.continent(0);
    REQUIRE(east);
    const glm::vec2 stormwindTaxi = east->toTaxiMap(-8840.56, 489.7);
    CHECK(stormwindTaxi.x > 0.3f);
    CHECK(stormwindTaxi.x < 0.6f);
    CHECK(stormwindTaxi.y > 0.5f);
    CHECK(stormwindTaxi.y < 0.9f);
    // Phase 15: the World map (WoW.exe's GetPlayerMapPosition at the world's zoom). Stormwind lies on the Eastern
    // Kingdoms' south west (the World map's right half), Orgrimmar on Kalimdor's east coast (its left half).
    const glm::vec2 stormwindWorld = east->toWorldMap(-8913.0, 554.0);
    CHECK(stormwindWorld.x == doctest::Approx(0.7190).epsilon(0.001));
    CHECK(stormwindWorld.y == doctest::Approx(0.6325).epsilon(0.001));
    CHECK(maps.continentAt(stormwindWorld) == east);
    const glm::dvec2 stormwindBack = east->fromWorldMap(stormwindWorld);
    CHECK(stormwindBack.x == doctest::Approx(-8913.0).epsilon(1e-3));
    CHECK(stormwindBack.y == doctest::Approx(554.0).epsilon(1e-2));
    const game::WorldMapContinent* kalimdorContinent = maps.continent(1);
    REQUIRE(kalimdorContinent);
    const glm::vec2 orgrimmarWorld = kalimdorContinent->toWorldMap(1600.0, -4400.0);
    CHECK(orgrimmarWorld.x > 0.25f);
    CHECK(orgrimmarWorld.x < 0.35f);
    CHECK(maps.continentAt(orgrimmarWorld) == kalimdorContinent);
    CHECK(maps.continentAt({0.5f, 0.5f}) == nullptr); // the Maelstrom's sea

    game::TaxiData taxi;
    REQUIRE(taxi.init(db));
    const game::TaxiNodeInfo* stormwind = taxi.node(2);
    REQUIRE(stormwind);
    CHECK(stormwind->name == "Stormwind, Elwynn");
    CHECK(stormwind->mountAlliance == 541);
    const game::TaxiPathInfo* toSentinel = taxi.path(2, 4);
    REQUIRE(toSentinel);
    CHECK(toSentinel->cost == 110);
    CHECK(toSentinel->pointCount > 10);
    CHECK(taxi.points(*toSentinel).front().index == 0);
    // Stormwind -> Booty Bay knowing only Sentinel Hill on the way: through it.
    game::TaxiMask known{};
    for (const u32 node : {2u, 4u, 19u}) {
        known[node / 32] |= 1u << (node % 32);
    }
    game::TaxiRoute route;
    REQUIRE(taxi.route(2, 19, known, route));
    CHECK(route.count >= 2);
    CHECK(route.nodes[0] == 2);
    CHECK(route.nodes[route.count - 1] == 19);
    MESSAGE("Stormwind -> Booty Bay: " << route.count << " nodes, " << route.cost << " copper, "
                                       << route.length << " yards");
}

namespace {

// SMSG_MONSTER_MOVE as packet_builder.cpp writes a flying (Catmull-Rom) spline: start, id, type, flags,
// duration, then every point after the start in full.
Bytes flyingMove(u64 guid, glm::vec3 start, u32 splineId, u32 durationMs,
                 const std::vector<glm::vec3>& points) {
    Bytes out;
    net::ByteWriter w(out);
    w.packedGuid(guid).f32v(start.x).f32v(start.y).f32v(start.z).u32v(splineId).u8v(0);
    w.u32v(0x200 | 0x100).u32v(durationMs).u32v(static_cast<u32>(points.size()));
    for (const glm::vec3& p : points) {
        w.f32v(p.x).f32v(p.y).f32v(p.z);
    }
    return out;
}

} // namespace

TEST_CASE("Flying splines are Catmull-Rom curves through their points, timed by segment length") {
    game::EntityStore store;
    REQUIRE(store.applyUpdateObject(
        created(kPlayer, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}}), 0));
    const std::vector<glm::vec3> points = {
        {100.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 0.0f}, {200.0f, 100.0f, 50.0f}};
    REQUIRE(store.applyMonsterMove(flyingMove(kPlayer, {0.0f, 0.0f, 0.0f}, 41, 10000, points), 1000));
    const game::Entity* e = store.find(kPlayer);
    REQUIRE(e);
    CHECK(e->splineId == 41);
    CHECK_FALSE(e->splineStopped);
    REQUIRE(e->path.size() == 4);
    REQUIRE(e->segmentEnds.size() == 3);
    CHECK(e->segmentEnds.back() == doctest::Approx(1.0f));
    CHECK(e->segmentEnds[0] < e->segmentEnds[1]);
    // Through the start, every point (at the end of its segment's time) and the destination.
    const glm::vec3 begin = e->positionAt(1000);
    CHECK(begin.x == doctest::Approx(0.0f));
    for (u32 i = 0; i < 3; ++i) {
        const u32 at = 1000 + static_cast<u32>(e->segmentEnds[i] * 10000.0f + 0.5f);
        const glm::vec3 p = e->positionAt(at);
        CHECK(glm::length(p - points[i]) < 0.5f);
    }
    // A curve: halfway along the corner segment it cuts inside the straight lines.
    const u32 middle = 1000 + static_cast<u32>((e->segmentEnds[0] + e->segmentEnds[1]) * 0.5f * 10000.0f);
    const glm::vec3 corner = e->positionAt(middle);
    CHECK(corner.x > 100.0f);
    f32 heading = 0.0f;
    REQUIRE(e->headingAt(1001, heading));
    CHECK(std::abs(heading) < 0.2f); // leaving along +X
    CHECK(e->speedAt(5000) > 30.0f);

    // The player's controller: follow, then answer the end once.
    game::ServerSplineStep step = game::serverSplineStep(*e, 0, 6000);
    CHECK(step.kind == game::ServerSplineStep::Kind::Follow);
    CHECK(step.splineId == 41);
    CHECK(step.hasFacing);
    step = game::serverSplineStep(*e, 0, 11001);
    CHECK(step.kind == game::ServerSplineStep::Kind::Done);
    CHECK(glm::length(step.position - points.back()) < 0.01f);
    CHECK(game::serverSplineStep(*e, 41, 11001).kind == game::ServerSplineStep::Kind::None);

    // A stop (MonsterMoveStop) on the player: answered where it stands.
    Bytes stop;
    net::ByteWriter(stop).packedGuid(kPlayer).f32v(150.0f).f32v(100.0f).f32v(20.0f).u32v(42).u8v(1);
    REQUIRE(store.applyMonsterMove(stop, 7000));
    CHECK(e->splineStopped);
    step = game::serverSplineStep(*e, 41, 7000);
    CHECK(step.kind == game::ServerSplineStep::Kind::Done);
    CHECK(step.splineId == 42);
    CHECK(step.position.x == doctest::Approx(150.0f));
    // A walking (linear) spline stays a polyline at constant speed.
    Bytes walk;
    net::ByteWriter ww(walk);
    ww.packedGuid(kPlayer).f32v(0.0f).f32v(0.0f).f32v(0.0f).u32v(43).u8v(0).u32v(0x100).u32v(1000).u32v(1);
    ww.f32v(10.0f).f32v(0.0f).f32v(0.0f);
    REQUIRE(store.applyMonsterMove(walk, 0));
    CHECK(e->segmentEnds.empty());
    CHECK(e->positionAt(500).x == doctest::Approx(5.0f));
}

TEST_CASE("A server spline's facing target lasts until the unit moves on its own (Charge)") {
    game::EntityStore store;
    constexpr u64 kVictim = 0xF130000F00001234ull;
    REQUIRE(store.applyUpdateObject(
        created(kPlayer, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}}), 0));
    // SMSG_MONSTER_MOVE facing a target (MonsterMoveFacingTarget): the charge to the victim, 20 yards in 600 ms.
    Bytes charge;
    net::ByteWriter(charge)
        .packedGuid(kPlayer)
        .f32v(0.0f).f32v(0.0f).f32v(0.0f)
        .u32v(7)
        .u8v(3)
        .u64v(kVictim)
        .u32v(0x100)
        .u32v(600)
        .u32v(1)
        .f32v(20.0f).f32v(0.0f).f32v(0.0f);
    REQUIRE(store.applyMonsterMove(charge, 1000));
    CHECK(store.find(kPlayer)->facingTarget == kVictim);
    // The player's own movement afterwards: it faces where it says.
    net::MovementInfo info;
    info.position = {20.0f, 0.0f, 0.0f, 1.5f};
    store.setMovement(kPlayer, info, 2000);
    CHECK(store.find(kPlayer)->facingTarget == 0);
    CHECK_FALSE(store.find(kPlayer)->hasFinalAngle);
    CHECK(store.find(kPlayer)->orientation == doctest::Approx(1.5f));
}

TEST_CASE("Transport paths: the server's keyframe timing over real ship and zeppelin paths") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::TaxiData taxi;
    REQUIRE(taxi.init(db));
    // The server's `transports` periods (vmangos sql/migrations): its own computation is "not perfect", these
    // are what the client walks.
    struct Known {
        u32 path;
        const char* name;
        u32 period;
    };
    for (const Known& k :
         {Known{302, "Orgrimmar - Undercity", 356284}, Known{241, "Ratchet - Booty Bay", 350818},
          Known{301, "Grom'gol - Orgrimmar", 303463}}) {
        const game::TaxiPathInfo* info = taxi.pathById(k.path);
        REQUIRE(info);
        game::TransportPath path;
        REQUIRE(path.build(taxi.points(*info), 30.0f, 1.0f));
        MESSAGE(std::string(k.name) << ": " << path.keyFrameCount() << " frames, period " << path.period()
                                    << " ms (server " << k.period << ")");
        CHECK(path.period() > 0);
        const game::TransportPlacement first = path.at(0);
        const game::TransportPlacement later = path.at(path.period() / 3);
        CHECK(glm::length(later.position - first.position) > 1.0f);
        // Continuous while moving: 50 ms apart never more than twice the nominal speed (a Catmull-Rom segment
        // is walked at a constant parameter rate, not a constant speed, by the server too); jumps are the
        // jumps.
        u32 jumps = 0;
        f32 fastest = 0.0f;
        for (u32 t = 0; t + 50 < path.period(); t += 50) {
            const game::TransportPlacement a = path.at(t), b = path.at(t + 50);
            const f32 step = glm::length(b.position - a.position);
            if (a.map != b.map || step > 10.0f) {
                ++jumps;
            } else {
                fastest = std::max(fastest, step / 0.05f);
            }
        }
        MESSAGE("  fastest " << fastest << " yd/s, " << jumps << " jumps");
        CHECK(fastest < 60.0f);
        CHECK(jumps <= 3); // the map changes (and a jump within the map) only
    }
}

TEST_CASE("Transports: elevator offsets, passenger frames, and the transport fields of entities") {
    // TransportAnimation.dbc nodes: up 20 yards in 5 s, wait, down.
    game::TransportAnimations animations;
    animations.add(4170, 0, {0.0f, 0.0f, 0.0f});
    animations.add(4170, 5000, {0.0f, 0.0f, 20.0f});
    animations.add(4170, 10000, {0.0f, 0.0f, 20.0f});
    animations.add(4170, 15000, {0.0f, 0.0f, 0.0f});
    CHECK(animations.has(4170));
    CHECK(animations.period(4170) == 15000);
    glm::vec3 offset(0.0f);
    REQUIRE(animations.offsetAt(4170, 2500, offset));
    CHECK(offset.z == doctest::Approx(10.0f));
    REQUIRE(animations.offsetAt(4170, 7000, offset));
    CHECK(offset.z == doctest::Approx(20.0f));
    REQUIRE(animations.offsetAt(4170, 15000 + 12500, offset)); // the cycle repeats
    CHECK(offset.z == doctest::Approx(10.0f));
    CHECK_FALSE(animations.offsetAt(1, 0, offset));

    // A passenger's offset turns with its transport (GenericTransport::CalculatePassengerPosition / Offset).
    game::TransportPlacement ship;
    ship.position = {1000.0f, 2000.0f, 50.0f};
    ship.orientation = 1.5707964f;
    const glm::vec3 world = game::transportToWorld(ship, {10.0f, 0.0f, 3.0f});
    CHECK(world.x == doctest::Approx(1000.0f).epsilon(1e-5));
    CHECK(world.y == doctest::Approx(2010.0f));
    CHECK(world.z == doctest::Approx(53.0f));
    const glm::vec3 back = game::worldToTransport(ship, world);
    CHECK(back.x == doctest::Approx(10.0f));
    CHECK(back.y == doctest::Approx(0.0f).epsilon(1e-4));

    // A transport's create block: UPDATEFLAG_TRANSPORT | ALL | HAS_POSITION (ShipTransport), the time into
    // its cycle.
    game::EntityStore store;
    Bytes block;
    net::ByteWriter w(block);
    const u64 zeppelin = 0x1FC0000000000000ull | 164871;
    w.u32v(1).u8v(0).u8v(2).packedGuid(zeppelin).u8v(static_cast<u8>(game::ObjectType::GameObject)).u8v(0x52);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(1.0f); // the stationary position (zero for ships)
    w.u32v(1);                                     // UPDATEFLAG_ALL
    w.u32v(123456);                                // the transport time
    writeValues(w, {{uf::GAMEOBJECT_TYPE_ID, game::kGameObjectMoTransport}});
    REQUIRE(store.applyUpdateObject(block, 5000));
    const game::Entity* e = store.find(zeppelin);
    REQUIRE(e);
    CHECK(e->hasTransportTime);
    CHECK(e->transportTime == 123456);
    CHECK(e->transportTimeAt == 5000);

    // A passenger's movement: MOVEFLAG_ONTRANSPORT with its offset on it.
    REQUIRE(
        store.applyUpdateObject(created(kPlayer, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 1}}), 0));
    Bytes move;
    net::ByteWriter mw(move);
    mw.packedGuid(kPlayer);
    net::MovementInfo info;
    info.flags = net::kMoveFlagOnTransport;
    info.position = {1300.0f, -4650.0f, 66.0f, 0.5f};
    info.transportGuid = zeppelin;
    info.transportPosition = {1.5f, -2.0f, -5.5f, 0.25f};
    net::writeMovementInfo(mw, info);
    REQUIRE(store.applyMove(move, 6000));
    const game::Entity* passenger = store.find(kPlayer);
    REQUIRE(passenger);
    CHECK(passenger->transportGuid == zeppelin);
    CHECK(passenger->transportOffset.x == doctest::Approx(1.5f));
    CHECK(passenger->transportOffset.z == doctest::Approx(-5.5f));
    CHECK(passenger->transportFacing == doctest::Approx(0.25f));
    info.flags = 0;
    move.clear();
    net::ByteWriter mw2(move);
    mw2.packedGuid(kPlayer);
    net::writeMovementInfo(mw2, info);
    REQUIRE(store.applyMove(move, 7000));
    CHECK(passenger->transportGuid == 0); // off it

    // A far teleport on it: SMSG_TRANSFER_PENDING names the transport.
    Harness h;
    Bytes pending;
    net::ByteWriter(pending).u32v(0).u32v(164871).u32v(1);
    h.apply(Opcode::SMSG_TRANSFER_PENDING, pending);
    CHECK(h.play.travel().transferMap() == 0);
    CHECK(h.play.travel().transferTransport() == 164871);
}

TEST_CASE("Instance packets parse as vmangos writes them and reject truncation") {
    Bytes b;
    net::ByteWriter(b).u32v(net::kRaidInstanceWelcome).u32v(409).u32v(3 * 86400);
    net::RaidInstanceMessage m;
    REQUIRE(net::parseRaidInstanceMessage(b, m));
    CHECK(m.type == net::kRaidInstanceWelcome);
    CHECK(m.map == 409);
    CHECK(m.secondsLeft == 3u * 86400u);
    checkTruncations(b, [](std::span<const u8> p) {
        net::RaidInstanceMessage x;
        return net::parseRaidInstanceMessage(p, x);
    });
    b.clear();
    net::ByteWriter(b).u32v(net::kInstanceResetFailedOffline).u32v(36); // the reason first
    u32 reason = 0, map = 0;
    REQUIRE(net::parseInstanceResetFailed(b, reason, map));
    CHECK(reason == net::kInstanceResetFailedOffline);
    CHECK(map == 36);
    checkTruncations(b, [](std::span<const u8> p) {
        u32 r = 0, x = 0;
        return net::parseInstanceResetFailed(p, r, x);
    });
    b.clear();
    net::ByteWriter(b).u32v(60000).u32v(net::kRaidGroupRequired);
    u32 timer = 0, error = 0;
    REQUIRE(net::parseRaidGroupOnly(b, timer, error));
    CHECK(timer == 60000);
    CHECK(error == net::kRaidGroupRequired);
    checkTruncations(b, [](std::span<const u8> p) {
        u32 t = 0, e = 0;
        return net::parseRaidGroupOnly(p, t, e);
    });
    // SMSG_RAID_INSTANCE_INFO: a count, then map, seconds to the reset and instance id for each.
    b.clear();
    net::ByteWriter(b).u32v(2).u32v(409).u32v(7200).u32v(11).u32v(249).u32v(90).u32v(12);
    std::vector<net::SavedInstanceInfo> saved;
    REQUIRE(net::parseRaidInstanceInfo(b, saved));
    REQUIRE(saved.size() == 2);
    CHECK(saved[0].map == 409);
    CHECK(saved[0].secondsLeft == 7200);
    CHECK(saved[0].instanceId == 11);
    CHECK(saved[1].map == 249);
    CHECK(saved[1].instanceId == 12);
    checkTruncations(b, [](std::span<const u8> p) {
        std::vector<net::SavedInstanceInfo> x;
        return net::parseRaidInstanceInfo(p, x);
    });
    b.clear();
    net::ByteWriter(b).u32v(0xFFFFFFFFu); // a count the packet cannot hold: refused before allocating
    CHECK_FALSE(net::parseRaidInstanceInfo(b, saved));
}

TEST_CASE("Travel: instances (phase 12) - raid info, resetting, the boot countdown") {
    Harness h;
    game::Travel& travel = h.play.travel();
    // Every world entry asks for the raid info (RaidFrame on PLAYER_ENTERING_WORLD).
    CHECK(h.count(Opcode::CMSG_REQUEST_RAID_INFO) == 1);
    CHECK_FALSE(travel.savedInstancesKnown());
    Bytes b;
    net::ByteWriter(b).u32v(1).u32v(409).u32v(7200).u32v(11);
    h.apply(Opcode::SMSG_RAID_INSTANCE_INFO, b);
    REQUIRE(travel.savedInstances().size() == 1);
    CHECK(travel.savedInstances()[0].map == 409);
    CHECK(travel.savedInstances()[0].instanceId == 11);
    CHECK(travel.savedInstances()[0].resetAtMs == h.now + 7200u * 1000u);
    const game::GameEvent* e = h.nextEvent(GameEventKind::SavedInstancesUpdated);
    REQUIRE(e);
    CHECK(e->value[0] == 0); // not a /raidinfo
    travel.requestRaidInfo(true);
    CHECK(h.count(Opcode::CMSG_REQUEST_RAID_INFO) == 2);
    h.apply(Opcode::SMSG_RAID_INSTANCE_INFO, Bytes{0, 0, 0, 0});
    e = h.nextEvent(GameEventKind::SavedInstancesUpdated);
    REQUIRE(e);
    CHECK(e->value[0] == 1);
    CHECK(travel.savedInstances().empty());

    // Reset all instances (WoW.exe's CanShowResetInstances): only after leaving a party dungeon, not inside one,
    // for 90000 s; a reset forgets it.
    CHECK_FALSE(travel.canShowResetInstances(h.now));
    h.play.enterWorld(kPlayer, 36, false, h.now); // into the Deadmines
    CHECK_FALSE(travel.canShowResetInstances(h.now));
    h.play.enterWorld(kPlayer, 0, false, h.now); // and out
    CHECK(travel.canShowResetInstances(h.now));
    CHECK(travel.canShowResetInstances(h.now + game::Travel::kResetInstancesShownMs));
    CHECK_FALSE(travel.canShowResetInstances(h.now + game::Travel::kResetInstancesShownMs + 1));
    travel.resetInstances();
    CHECK(h.count(Opcode::CMSG_RESET_INSTANCES) == 1);
    CHECK(h.sent.back().body.empty());
    Bytes map;
    net::ByteWriter(map).u32v(36);
    h.apply(Opcode::SMSG_INSTANCE_RESET, map);
    e = h.nextEvent(GameEventKind::InstanceReset);
    REQUIRE(e);
    CHECK(e->id == 36);
    CHECK_FALSE(travel.canShowResetInstances(h.now));
    // A raid map does not count (only Map.dbc's party dungeons); SMSG_UPDATE_LAST_INSTANCE names one too.
    h.apply(Opcode::SMSG_UPDATE_LAST_INSTANCE, map);
    CHECK(travel.canShowResetInstances(h.now));
    h.apply(Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP, Bytes{1, 0, 0, 0});
    CHECK(travel.instanceOwnership());
    // A new login forgets the dungeon left.
    h.play.enterWorld(kPlayer, 0, true, h.now);
    CHECK_FALSE(travel.canShowResetInstances(h.now));
    CHECK_FALSE(travel.instanceOwnership());

    // The instance boot: SMSG_RAID_GROUP_ONLY with a time starts the countdown, with 0 it stops (and shows the error).
    b.clear();
    net::ByteWriter(b).u32v(60000).u32v(net::kRaidGroupRequired);
    h.apply(Opcode::SMSG_RAID_GROUP_ONLY, b);
    e = h.nextEvent(GameEventKind::InstanceBootStarted);
    REQUIRE(e);
    CHECK(e->value[0] == 60000);
    CHECK(travel.bootRemainingMs(h.now) == 60000);
    CHECK(travel.bootRemainingMs(h.now + 15000) == 45000);
    CHECK(travel.bootRemainingMs(h.now + 61000) == 0);
    b.clear();
    net::ByteWriter(b).u32v(0).u32v(net::kRaidGroupRequired);
    h.apply(Opcode::SMSG_RAID_GROUP_ONLY, b);
    e = h.nextEvent(GameEventKind::RaidGroupOnly);
    REQUIRE(e);
    CHECK(e->value[0] == static_cast<i32>(net::kRaidGroupRequired));
    CHECK(h.nextEvent(GameEventKind::InstanceBootStopped));
    CHECK(travel.bootRemainingMs(h.now) == 0);

    // Warnings and failures come out as events for the client's lines (game/Notices).
    b.clear();
    net::ByteWriter(b).u32v(net::kRaidInstanceWarningMin).u32v(409).u32v(600);
    h.apply(Opcode::SMSG_RAID_INSTANCE_MESSAGE, b);
    e = h.nextEvent(GameEventKind::InstanceResetWarning);
    REQUIRE(e);
    CHECK(e->id == 409);
    CHECK(e->value[0] == static_cast<i32>(net::kRaidInstanceWarningMin));
    CHECK(e->value[1] == 600);
    b.clear();
    net::ByteWriter(b).u32v(net::kInstanceResetFailedGeneral).u32v(36);
    h.apply(Opcode::SMSG_INSTANCE_RESET_FAILED, b);
    e = h.nextEvent(GameEventKind::InstanceResetFailed);
    REQUIRE(e);
    CHECK(e->id == 36);
    CHECK(e->value[0] == 0);
}

TEST_CASE("SecondsToTime: two units at most, days and hours only above a whole one") {
    text::unload(); // keys stand for the words
    char out[96];
    CHECK(game::secondsToTime(out, 0).empty());
    CHECK(game::secondsToTime(out, 1) == "1 SECONDS_ABBR");
    CHECK(game::secondsToTime(out, 59) == "59 SECONDS_ABBR_P1");
    CHECK(game::secondsToTime(out, 60) == "1 MINUTES_ABBR");
    CHECK(game::secondsToTime(out, 125) == "2 MINUTES_ABBR_P1 5 SECONDS_ABBR_P1");
    CHECK(game::secondsToTime(out, 3600) == "60 MINUTES_ABBR_P1"); // not above an hour
    CHECK(game::secondsToTime(out, 3661) == "1 HOURS_ABBR 1 MINUTES_ABBR");
    CHECK(game::secondsToTime(out, 86400) == "24 HOURS_ABBR_P1");
    CHECK(game::secondsToTime(out, 6 * 86400 + 23 * 3600 + 59 * 60) == "6 DAYS_ABBR_P1 23 HOURS_ABBR_P1");
    CHECK(game::secondsToTime(out, 125, true) == "2 MINUTES_ABBR_P1");
    if (test::loadTexts()) {
        CHECK(game::secondsToTime(out, 3 * 86400 + 4 * 3600 + 5) == "3 Days 4 Hrs");
        CHECK(game::secondsToTime(out, 3661) == "1 Hr 1 Min");
        text::unload();
    }
}

TEST_CASE("Travel texts: the client's keys for taxi replies, refused transfers and the zone's PvP line") {
    // Without a table every text shows its key: which one each case picks.
    text::unload();
    char buffer[96];
    CHECK(game::taxiReplyText(static_cast<u32>(net::TaxiReply::Ok)).empty());
    CHECK(game::taxiReplyText(static_cast<u32>(net::TaxiReply::NotEnoughMoney)) == "ERR_TAXINOTENOUGHMONEY");
    CHECK(game::taxiReplyText(static_cast<u32>(net::TaxiReply::NotStanding)) == "ERR_TAXINOTSTANDING");
    CHECK(game::taxiReplyText(static_cast<u32>(net::TaxiReply::UnspecifiedServerError)) ==
          "ERR_TAXIUNSPECIFIEDSERVERERROR");
    CHECK(game::taxiReplyText(99) == "ERR_TAXIUNSPECIFIEDSERVERERROR");
    CHECK(game::transferAbortText(net::kTransferAbortZoneInCombat) == "TRANSFER_ABORT_ZONE_IN_COMBAT");
    CHECK(game::transferAbortText(net::kTransferAbortSilently).empty());
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Normal, game::kAreaTeamAlliance).empty());
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Contested, game::kAreaTeamHorde) == "CONTESTED_TERRITORY");

    // The real table, in English: what the player reads.
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR/ui.vdpk not available: skipping the English texts");
        return;
    }
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Friendly, game::kAreaTeamAlliance) ==
          "Alliance Territory");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Hostile, game::kAreaTeamAlliance) == "Horde Territory");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Friendly, game::kAreaTeamHorde) == "Horde Territory");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Hostile, game::kAreaTeamHorde) == "Alliance Territory");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Contested, game::kAreaTeamAlliance) ==
          "Contested Territory");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Arena, game::kAreaTeamAlliance) == "PvP Area");
    CHECK(game::zonePvpText(buffer, game::ZonePvp::Sanctuary, game::kAreaTeamAlliance) == "Sanctuary");
    // Every reply code has its text (a missing one would show its key).
    for (u32 reply = 1; reply <= static_cast<u32>(net::TaxiReply::NotStanding); ++reply) {
        CHECK(game::taxiReplyText(reply).substr(0, 4) != "ERR_");
    }
    CHECK(game::taxiReplyText(static_cast<u32>(net::TaxiReply::TooFarAway)) == text::get("ERR_TAXITOOFARAWAY"));
    CHECK(game::transferAbortText(net::kTransferAbortMaxPlayers) == text::get("TRANSFER_ABORT_MAX_PLAYERS"));
    // The keys the travel windows, the loading screen and their messages use.
    const char* keys[] = {"ERR_ZONE_EXPLORED",
                          "ERR_ZONE_EXPLORED_XP",
                          "ERR_NEWTAXIPATH",
                          "ERR_DEATHBIND_SUCCESS_S",
                          "CONFIRM_BINDER",
                          "ACCEPT",
                          "CANCEL",
                          "CONTINENT",
                          "WORLDMAP_BUTTON",
                          "ERR_TAXINOPATHS",
                          "ERR_TAXINOTENOUGHMONEY",
                          "VD_TRAVEL_SANCTUARY",
                          "VD_TRAVEL_NEW_AREA",
                          "VD_TRAVEL_THIS_PLACE",
                          "VD_TRAVEL_THIS_INN",
                          "VD_TRAVEL_MAP_POSITION",
                          "VD_TRAVEL_MAP_NONE",
                          "VD_TRAVEL_MAP_LOADING",
                          "VD_TRAVEL_TAXI_STOP",
                          "VD_TRAVEL_TAXI_STOPS",
                          "VD_TRAVEL_FLY",
                          "VD_TRAVEL_LOADING_WORLD",
                          "VD_TRAVEL_LOADING_TRANSFER",
                          "VD_TRAVEL_LOADING_NO_PACK"};
    for (const char* key : keys) {
        CHECK_MESSAGE(text::has(key), key);
    }
    CHECK(text::format(buffer, "ERR_ZONE_EXPLORED_XP", "Goldshire", 50) ==
          "Discovered Goldshire: 50 experience gained");
    CHECK(std::string_view(text::format(buffer, "CONFIRM_BINDER", "Goldshire")).find(" Goldshire ") !=
          std::string_view::npos);
    CHECK(text::format(buffer, "VD_TRAVEL_MAP_POSITION", 41.6f, 65.2f) == "Your position: 42, 65");
    CHECK(text::format(buffer, "VD_TRAVEL_TAXI_STOPS", 3u) == "3 stops");
    text::unload();
}
