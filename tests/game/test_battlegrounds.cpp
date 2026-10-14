// Phase 17 (lead): the battleground queues (SMSG_BATTLEFIELD_STATUS, CMSG_BATTLEFIELD_PORT, CMSG_LEAVE_BATTLEFIELD,
// game::Battlegrounds) and the world states (SMSG_INIT_WORLD_STATES, SMSG_UPDATE_WORLD_STATE, game::WorldStates)
// byte for byte as vmangos-core writes and reads them (Server/Packets/Battleground.cpp, Misc.cpp UpdateWorldState,
// Player::SendInitWorldStates), truncated packets rejected; and the rules WoW.exe 1.12.1 keeps them by (the status
// handler 0x4AA850, the world entry 0x4A9DB0) through game::Gameplay with synthetic packets.
#include "game/AreaData.h"
#include "game/Battlegrounds.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/WorldStates.h"
#include "net/BattlegroundMessages.h"
#include "net/ByteBuffer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::BattlefieldState;
using game::GameEventKind;
using net::Opcode;
using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x11;
constexpr u32 kWarsongGulch = 489;
constexpr u32 kArathiBasin = 529;
constexpr u32 kAlteracValley = 30;
constexpr u32 kAzeroth = 0;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

Bytes status(u32 slot, u32 map, u8 bracket, u32 instance, u32 state, u32 time1, u32 time2) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(slot).u32v(map);
    if (map == 0) {
        return out;
    }
    w.u8v(bracket).u32v(instance).u32v(state).u32v(time1);
    if (state == net::kBattlefieldStatusWaitQueue || state == net::kBattlefieldStatusInProgress) {
        w.u32v(time2);
    }
    return out;
}

Bytes emptySlot(u32 slot) {
    Bytes out;
    net::ByteWriter(out).u32v(slot).u32v(0);
    return out;
}

Bytes initStates(u32 map, u32 zone, std::vector<std::pair<u32, u32>> states) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(map).u32v(zone).u16v(static_cast<u16>(states.size() + 1));
    for (const auto& [id, value] : states) {
        w.u32v(id).u32v(value);
    }
    w.u32v(0).u32v(0); // vMaNGOS' terminator, counted
    return out;
}

Bytes updateState(u32 id, u32 value) {
    Bytes out;
    net::ByteWriter(out).u32v(id).u32v(value);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Gameplay with the battleground maps known (Map.dbc instance type 3) and its sent packets.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::AreaData areas;
    std::vector<Sent> sent;
    u32 now = 100000;
    u32 cursor = 0;

    explicit Harness(u32 map = kAzeroth) {
        std::vector<game::MapInfo> maps(4);
        maps[0].id = kAzeroth;
        maps[0].name = "Eastern Kingdoms";
        maps[1].id = kWarsongGulch;
        maps[1].name = "Warsong Gulch";
        maps[1].instanceType = 3;
        maps[2].id = kArathiBasin;
        maps[2].name = "Arathi Basin";
        maps[2].instanceType = 3;
        maps[3].id = kAlteracValley;
        maps[3].name = "Alterac Valley";
        maps[3].instanceType = 3;
        areas.set({}, {}, std::move(maps));
        play.travel().setData(&areas, nullptr);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, map, true, now);
        cursor = play.events().end();
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) { return play.handle(opcode, body, now); }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> events(GameEventKind kind) {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            if (e->kind == kind) {
                out.push_back(*e);
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE("Battlegrounds: SMSG_BATTLEFIELD_STATUS, the port and the leave byte for byte") {
    net::BattlefieldStatus s;
    // An empty slot: the slot and a u32 0, nothing else.
    const Bytes empty = emptySlot(2);
    REQUIRE(net::parseBattlefieldStatus(empty, s));
    CHECK(s.queueSlot == 2);
    CHECK(s.mapId == 0);
    checkTruncations(empty, [&](std::span<const u8> p) { return net::parseBattlefieldStatus(p, s); });

    // Queued: both times.
    const Bytes queued = status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 90000, 15000);
    CHECK(queued.size() == 4 + 4 + 1 + 4 + 4 + 4 + 4);
    REQUIRE(net::parseBattlefieldStatus(queued, s));
    CHECK(s.queueSlot == 0);
    CHECK(s.mapId == kWarsongGulch);
    CHECK(s.bracketId == 4);
    CHECK(s.instanceId == 0);
    CHECK(s.status == net::kBattlefieldStatusWaitQueue);
    CHECK(s.time1 == 90000);
    CHECK(s.time2 == 15000);
    checkTruncations(queued, [&](std::span<const u8> p) { return net::parseBattlefieldStatus(p, s); });

    // Confirm: time1 only (BattlefieldStatus::AppendBodyTo skips time2 for STATUS_WAIT_JOIN).
    const Bytes confirm = status(1, kArathiBasin, 3, 7, net::kBattlefieldStatusWaitJoin, 80000, 0);
    CHECK(confirm.size() == 4 + 4 + 1 + 4 + 4 + 4);
    REQUIRE(net::parseBattlefieldStatus(confirm, s));
    CHECK(s.queueSlot == 1);
    CHECK(s.instanceId == 7);
    CHECK(s.status == net::kBattlefieldStatusWaitJoin);
    CHECK(s.time1 == 80000);
    CHECK(s.time2 == 0);
    checkTruncations(confirm, [&](std::span<const u8> p) { return net::parseBattlefieldStatus(p, s); });

    // Active: both times again.
    const Bytes active = status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 0, 61000);
    REQUIRE(net::parseBattlefieldStatus(active, s));
    CHECK(s.status == net::kBattlefieldStatusInProgress);
    CHECK(s.time2 == 61000);
    checkTruncations(active, [&](std::span<const u8> p) { return net::parseBattlefieldStatus(p, s); });

    Bytes body;
    net::writeBattlefieldPort(body, kWarsongGulch, true);
    CHECK(body == Bytes{0xE9, 0x01, 0, 0, 1});
    body.clear();
    net::writeBattlefieldPort(body, kArathiBasin, false);
    CHECK(body == Bytes{0x11, 0x02, 0, 0, 0});
    body.clear();
    net::writeLeaveBattlefield(body, kWarsongGulch);
    CHECK(body == Bytes{0xE9, 0x01, 0, 0});
}

TEST_CASE("Battlegrounds: SMSG_INIT_WORLD_STATES and SMSG_UPDATE_WORLD_STATE byte for byte") {
    net::InitWorldStates init;
    const Bytes body = initStates(kWarsongGulch, 3277, {{1581, 2}, {1582, 1}, {2339, 3}});
    REQUIRE(net::parseInitWorldStates(body, init));
    CHECK(init.mapId == kWarsongGulch);
    CHECK(init.zoneId == 3277);
    REQUIRE(init.states.size() == 3); // the (0, 0) terminator is dropped
    CHECK(init.states[0].id == 1581);
    CHECK(init.states[0].value == 2);
    CHECK(init.states[2].id == 2339);
    checkTruncations(body, [&](std::span<const u8> p) { return net::parseInitWorldStates(p, init); });

    net::WorldStatePair pair;
    const Bytes update = updateState(1581, 3);
    REQUIRE(net::parseUpdateWorldState(update, pair));
    CHECK(pair.id == 1581);
    CHECK(pair.value == 3);
    checkTruncations(update, [&](std::span<const u8> p) { return net::parseUpdateWorldState(p, pair); });
}

TEST_CASE("Battlegrounds: every world entry asks for the queues") {
    Harness h;
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_STATUS).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_STATUS)[0].empty());
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 90000, 0)) ==
            game::Gameplay::Handled::Yes);
    // Another map (SMSG_NEW_WORLD): asked again; the queue is kept until the answer says otherwise.
    h.play.enterWorld(kMe, kAzeroth, false, h.now);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_STATUS).size() == 2);
    CHECK(h.play.battlegrounds().queue(0).state == BattlefieldState::Queued);
    // Another character: empty.
    h.play.leaveWorld();
    h.play.enterWorld(0x99, kAzeroth, true, h.now);
    CHECK(h.play.battlegrounds().queue(0).state == BattlefieldState::None);
    CHECK(h.play.battlegrounds().queueCount() == 0);
}

TEST_CASE("Battlegrounds: queued, invited, in the battle, the victory and out, as WoW.exe keeps them") {
    Harness h;
    const game::Battlegrounds& bg = h.play.battlegrounds();
    CHECK(bg.activeSlot() == -1);
    CHECK_FALSE(bg.inBattleground());

    // Queued for Warsong Gulch 15 s ago, 90 s average.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 90000, 15000)) ==
            game::Gameplay::Handled::Yes);
    auto ev = h.events(GameEventKind::BattlefieldStatus);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].id == 0);
    CHECK(ev[0].value[0] == static_cast<i32>(BattlefieldState::Queued));
    CHECK(ev[0].value[1] == static_cast<i32>(BattlefieldState::None));
    CHECK(bg.queue(0).state == BattlefieldState::Queued);
    CHECK(std::string_view(game::battlefieldStateName(bg.queue(0).state)) == "queued");
    CHECK(bg.mapName(0) == "Warsong Gulch");
    CHECK(bg.queue(0).averageWaitMs == 90000);
    CHECK(bg.timeWaitedMs(0, h.now) == 15000);
    CHECK(bg.timeWaitedMs(0, h.now + 5000) == 20000);
    CHECK(bg.queueCount() == 1);

    // A second queue (Arathi Basin) in slot 1.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.queueCount() == 2);
    CHECK(bg.queue(1).averageWaitMs == 0); // QUEUE_TIME_UNAVAILABLE

    // Invited to Warsong Gulch 3: 80 s to accept.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusWaitJoin, 80000, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.queue(0).state == BattlefieldState::Confirm);
    CHECK(bg.queue(0).instanceId == 3);
    CHECK(bg.portExpirationMs(0, h.now) == 80000);
    CHECK(bg.portExpirationMs(0, h.now + 30000) == 50000);
    CHECK(bg.portExpirationMs(0, h.now + 90000) == 0);
    CHECK(bg.timeWaitedMs(0, h.now) == 0);

    // Enter: CMSG_BATTLEFIELD_PORT with the queue's map and 1.
    REQUIRE(h.play.battlegrounds().acceptPort(0, true));
    REQUIRE(h.sentWith(Opcode::CMSG_BATTLEFIELD_PORT).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_PORT)[0] == Bytes{0xE9, 0x01, 0, 0, 1});
    // Leave the other queue: 0.
    REQUIRE(h.play.battlegrounds().acceptPort(1, false));
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_PORT)[1] == Bytes{0x11, 0x02, 0, 0, 0});
    CHECK_FALSE(h.play.battlegrounds().acceptPort(2, true)); // an empty slot
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS, emptySlot(1)) == game::Gameplay::Handled::Yes);
    CHECK(bg.queue(1).state == BattlefieldState::None);
    CHECK(bg.queueCount() == 1);

    // The new world, then "active": the instance started 61 s ago, no end yet.
    h.play.enterWorld(kMe, kWarsongGulch, false, h.now);
    CHECK(bg.inBattleground());
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 0, 61000)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.queue(0).state == BattlefieldState::Active);
    CHECK(bg.activeSlot() == 0);
    CHECK(bg.instanceRunTimeMs(h.now) == 61000);
    CHECK(bg.instanceRunTimeMs(h.now + 1000) == 62000);
    CHECK(bg.instanceExpirationMs(h.now) == 0);
    CHECK(bg.winner() == -1);

    // Phase 17 (scores): LeaveBattlefield sends nothing before the score said the battle ended (0x4ABE60).
    CHECK_FALSE(h.play.battlegrounds().leave());
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_BATTLEFIELD).empty());

    // The victory: 2 minutes to auto-leave (TIME_TO_AUTOREMOVE); the score handler sets the winner and the end.
    h.play.battlegrounds().setWinner(1);
    h.play.battlegrounds().setEnded(true);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 120000, 900000)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.instanceExpirationMs(h.now) == 120000);
    CHECK(bg.instanceExpirationMs(h.now + 20000) == 100000);
    CHECK(bg.winner() == 1);

    // LeaveBattlefield: the active queue's map.
    REQUIRE(h.play.battlegrounds().leave());
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_BATTLEFIELD).back() == Bytes{0xE9, 0x01, 0, 0});
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS, emptySlot(0)) == game::Gameplay::Handled::Yes);
    CHECK(bg.activeSlot() == -1);
    CHECK(bg.instanceExpirationMs(h.now) == 0);
    CHECK(bg.instanceRunTimeMs(h.now) == 0);
    REQUIRE(h.play.battlegrounds().leave()); // still "ended", no active queue: map 0, as WoW.exe sends it
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_BATTLEFIELD).back() == Bytes{0, 0, 0, 0});
    h.play.enterWorld(kMe, kAzeroth, false, h.now);
    CHECK_FALSE(bg.inBattleground());
    CHECK_FALSE(h.play.battlegrounds().leave()); // a new world cleared the end: nothing sent
    CHECK(bg.winner() == -1);                    // a new world clears the score
    CHECK_FALSE(bg.ended());
}

TEST_CASE("Battlegrounds: a slot out of range is dropped, another status clears the instance's times") {
    Harness h(kWarsongGulch);
    const game::Battlegrounds& bg = h.play.battlegrounds();
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(3, kWarsongGulch, 4, 3, net::kBattlefieldStatusWaitQueue, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.events(GameEventKind::BattlefieldStatus).empty()); // 0x4AA86E: not even the event
    CHECK(bg.queueCount() == 0);

    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 120000, 5000)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.instanceExpirationMs(h.now) == 120000);
    // A queue for something else while in the battle: WoW.exe clears the instance's times (0x4AAA66) but keeps the
    // active slot.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.activeSlot() == 0);
    CHECK(bg.instanceExpirationMs(h.now) == 0);
    CHECK(bg.instanceRunTimeMs(h.now) == 0);
    // An unknown status: "error".
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(2, kAlteracValley, 5, 0, net::kBattlefieldStatusWaitLeave, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(bg.queue(2).state == BattlefieldState::Error);
    // Malformed.
    CHECK(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS, Bytes{0, 0, 0}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("World states: a new set per zone, updates add or change one") {
    Harness h(kWarsongGulch);
    const game::WorldStates& ws = h.play.worldStates();
    CHECK(ws.states().empty());
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kWarsongGulch, 3277, {{1582, 1}, {1581, 0}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.mapId() == kWarsongGulch);
    CHECK(ws.zoneId() == 3277);
    REQUIRE(ws.states().size() == 2);
    CHECK(ws.states()[0].id == 1581); // sorted
    CHECK(ws.has(1582));
    CHECK(ws.value(1582) == 1);
    CHECK_FALSE(ws.has(2339));
    CHECK(ws.value(2339) == 0);
    CHECK(ws.value(2339, 7) == 7);
    auto ev = h.events(GameEventKind::WorldStates);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].id == 0);

    const u32 changes = ws.changes();
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(1581, 2)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2339, 1)) == game::Gameplay::Handled::Yes);
    CHECK(ws.value(1581) == 2);
    CHECK(ws.value(2339) == 1);
    CHECK(ws.states().size() == 3);
    CHECK(ws.changes() == changes + 2);
    ev = h.events(GameEventKind::WorldStates);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].id == 1581);
    CHECK(ev[0].value[0] == 2);

    // The next zone's set replaces this one.
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kAzeroth, 1519, {})) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.states().empty());
    CHECK(ws.zoneId() == 1519);
    CHECK(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
    h.play.leaveWorld();
    CHECK(ws.mapId() == 0);

    // A far teleport (SMSG_NEW_WORLD) empties them too: nothing is known until the new zone's INIT (leaving a
    // battleground, its lines and battle map must not survive into the next world).
    h.play.enterWorld(kMe, kWarsongGulch, true, h.now);
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kWarsongGulch, 3277, {{1581, 2}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.known());
    h.play.enterWorld(kMe, kAzeroth, false, h.now);
    CHECK_FALSE(ws.known());
    CHECK(ws.states().empty());
    CHECK(ws.value(1581) == 0);
}
