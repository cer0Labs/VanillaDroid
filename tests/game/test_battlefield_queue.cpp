// Phase 17 (queue): the battlemaster's list and joining a battleground queue (CMSG_BATTLEMASTER_HELLO /
// _JOIN, CMSG_BATTLEFIELD_JOIN, CMSG_BATTLEFIELD_LIST, SMSG_BATTLEFIELD_LIST, SMSG_GROUP_JOINED_BATTLEGROUND)
// byte for byte as vmangos-core writes and reads them (Server/Packets/Battleground.cpp,
// BattleGroundMgr::BuildBattleGroundListPacket), truncated packets rejected; the rules WoW.exe 1.12.1 runs
// (the battlemaster's right click 0x5F0130, the list's handler 0x4AA6C0 and its walk-away check 0x4AA090,
// JoinBattlefield 0x4A9F60, ShowBattlefieldList 0x4AB8C0, the group's answer 0x4AACC0) and
// BattlefieldFrame.lua's queue sounds and closing messages, through game::Gameplay with synthetic packets;
// the lines they print (game/BattlefieldLines through game::NoticeFeed), worded in English; the minimap
// button's tooltip and menu (app/BattlefieldText), CONFIRM_BATTLEFIELD_ENTRY's one-per-queue rule
// (app/PopupStack), the battleground's announcements in the chat (game/SocialText); and Map.dbc's rows
// (db.vdpk).
#include "TextTable.h"
#include "app/BattlefieldText.h"
#include "app/PopupStack.h"
#include "core/Text.h"
#include "game/AreaData.h"
#include "game/BattlefieldQueue.h"
#include "game/Battlegrounds.h"
#include "game/CombatText.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Notices.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/BattlefieldListMessages.h"
#include "net/BattlegroundMessages.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::BattlefieldLine;
using game::GameEventKind;
using game::NoticeKind;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kElfarran = 0xF13000003A85004Dull; // Warsong Gulch Battlemaster, Champions' Hall (entry 14981)
constexpr u64 kBattlemaster = 0xF130000099990001ull; // one without a gossip menu
constexpr u32 kWarsongGulch = 489;
constexpr u32 kArathiBasin = 529;
constexpr u32 kAlteracValley = 30;
constexpr u32 kAzeroth = 0;
constexpr f32 kHallX = -8454.6f, kHallY = 318.9f, kHallZ = 121.0f;

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

Bytes list(u64 guid, u32 map, u8 bracket, std::vector<u32> ids) {
    Bytes out;
    net::ByteWriter w(out);
    w.u64v(guid).u32v(map).u8v(bracket).u32v(static_cast<u32>(ids.size()));
    for (const u32 id : ids) {
        w.u32v(id);
    }
    return out;
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

Bytes groupJoined(i32 result) {
    Bytes out;
    net::ByteWriter(out).u32v(static_cast<u32>(result));
    return out;
}

// SMSG_GROUP_LIST: (name, GUID) members online, all in the player's subgroup 0.
Bytes groupList(u64 leader, std::vector<std::pair<std::string, u64>> members, bool raid) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(raid ? net::kGroupTypeRaid : 0).u8v(0).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid] : members) {
        w.cstring(name).u64v(guid).u8v(net::kMemberOnline).u8v(0);
    }
    w.u64v(leader);
    if (!members.empty()) {
        w.u8v(3).u64v(0).u8v(2).u8v(0);
    }
    return out;
}

// Stub texts for the lines' keys, with the formats of GlobalStrings.lua 1.12.1 (the game shows the client's own).
const std::vector<std::pair<std::string, std::string>> kEnglish = {
    {"ERR_GROUP_JOIN_BATTLEGROUND_S", "Group queued for %s"},
    {"ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS", "Group not queued: a Deserter."},
    {"ERR_GROUP_JOIN_BATTLEGROUND_FAIL", "Group queued, not you"},
    {"ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY", "Group too big for it"},
    {"INSTANCE_SHUTDOWN_MESSAGE", "Too few players: ends in %s"},
    {"INSTANCE_COMPLETE_MESSAGE", "Battle over: closes in %s"},
    {"MINUTES_ABBR", "min"},
    {"MINUTES_ABBR_P1", "min"},
    {"SECONDS_ABBR", "sec"},
    {"SECONDS_ABBR_P1", "sec"},
    {"BATTLEFIELD_IN_QUEUE", "Queued: %s\nAverage wait: %s (last 10)\nWaited: %s"},
    {"BATTLEFIELD_QUEUE_CONFIRM", "Ready: %s\nInvite ends in %s"},
    {"BATTLEFIELD_IN_BATTLEFIELD", "In %s\n|cffffffff<Tap: score>\n<Again: map>|r"},
    {"QUEUE_TIME_UNAVAILABLE", "Unavailable"},
    {"LESS_THAN_ONE_MINUTE", "< 1 minute"},
    {"RIGHT_CLICK_MESSAGE", "|cffffffff<Right Click>: options|r"},
    {"CHANGE_INSTANCE", "Change Instance"},
    {"LEAVE_QUEUE", "Leave Queue"},
    {"ENTER_BATTLE", "Enter Battle"},
};

struct Line {
    NoticeKind kind;
    std::string text;
    bool operator==(const Line&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Line& l) {
    return os << static_cast<u32>(l.kind) << ": " << l.text;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Gameplay with the battleground maps known (Map.dbc's rows as the 1.12.1 file has them), a level-50 human in
// the Champions' Hall, its sent packets and the client's lines.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::AreaData areas;
    game::NoticeFeed feed;
    std::vector<Sent> sent;
    std::vector<u8> texts = test::textTable(kEnglish);
    u32 now = 100000;
    u32 cursor = 0;
    u32 lineCursor = 0;

    explicit Harness(u8 race = 1) {
        text::setTable(texts);
        std::vector<game::MapInfo> maps(4);
        maps[0].id = kAzeroth;
        maps[0].name = "Eastern Kingdoms";
        maps[1].id = kWarsongGulch;
        maps[1].name = "Warsong Gulch";
        maps[1].instanceType = 3;
        maps[1].minLevel = 10;
        maps[1].maxLevel = 60;
        maps[1].maxPlayers = 10;
        maps[1].descriptionHorde = "A valley bordering Ashenvale Forest (Horde).";
        maps[1].descriptionAlliance = "A valley bordering Ashenvale Forest (Alliance).";
        maps[1].bracketLevels = 10;
        maps[1].groupJoin = true;
        maps[2].id = kArathiBasin;
        maps[2].name = "Arathi Basin";
        maps[2].instanceType = 3;
        maps[2].minLevel = 20;
        maps[2].maxLevel = 60;
        maps[2].maxPlayers = 15;
        maps[2].bracketLevels = 10;
        maps[2].groupJoin = true;
        maps[3].id = kAlteracValley;
        maps[3].name = "Alterac Valley";
        maps[3].instanceType = 3;
        maps[3].minLevel = 51;
        maps[3].maxLevel = 60;
        maps[3].maxPlayers = 40;
        areas.set({}, {}, std::move(maps));
        play.travel().setData(&areas, nullptr);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, kAzeroth, true, now);
        REQUIRE(store.applyUpdateObject(created(kMe, game::ObjectType::Player,
                                                {{uf::UNIT_FIELD_HEALTH, 100},
                                                 {uf::UNIT_FIELD_LEVEL, 50},
                                                 {uf::UNIT_FIELD_BYTES_0, race | (1u << 8)}},
                                                kHallX, kHallY, kHallZ),
                                        now));
        feed.init(nullptr, nullptr);
        feed.restart(play);
        cursor = play.events().end();
        sent.clear();
    }
    ~Harness() { text::unload(); }
    game::BattlefieldQueueing& queue() { return play.battlefieldQueueing(); }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    void tick(u32 ms = 16) {
        now += ms;
        play.update(now);
    }
    void moveTo(f32 x, f32 y, f32 z) {
        net::MovementInfo info;
        info.position.x = x;
        info.position.y = y;
        info.position.z = z;
        store.setMovement(kMe, info, now);
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
    std::vector<game::GameEvent> events(GameEventKind kind) {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            if (e->kind == kind) {
                out.push_back(*e);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> allEvents() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
    std::vector<Line> lines() {
        game::CombatNames names;
        feed.update(play, store.find(kMe), names, now);
        std::vector<Line> out;
        while (const game::Notice* n = feed.lines().next(lineCursor)) {
            out.push_back({n->kind, n->text.data()});
        }
        return out;
    }
};

u32 countKind(const std::vector<game::GameEvent>& events, GameEventKind kind) {
    return static_cast<u32>(std::count_if(events.begin(), events.end(),
                                          [&](const game::GameEvent& e) { return e.kind == kind; }));
}

} // namespace

TEST_CASE("Battlefield queue: the list, the hello, the joins and the group's answer byte for byte") {
    Bytes body;
    net::writeBattlemasterHello(body, kElfarran);
    CHECK(body == Bytes{0x4D, 0x00, 0x85, 0x3A, 0x00, 0x00, 0x30, 0xF1});

    // SMSG_BATTLEFIELD_LIST: Elfarran, Warsong Gulch, the 50-59 bracket, three battles.
    net::BattlefieldList l;
    const Bytes three = list(kElfarran, kWarsongGulch, 4, {1, 2, 5});
    CHECK(three.size() == 8 + 4 + 1 + 4 + 3 * 4);
    REQUIRE(net::parseBattlefieldList(three, l));
    CHECK(l.battlemaster == kElfarran);
    CHECK(l.mapId == kWarsongGulch);
    CHECK(l.bracketId == 4);
    CHECK(l.instanceIds == std::vector<u32>{1, 2, 5});
    checkTruncations(three, [&](std::span<const u8> p) { return net::parseBattlefieldList(p, l); });
    // None running yet: just "First Available".
    const Bytes none = list(kMe, kAlteracValley, 0, {});
    REQUIRE(net::parseBattlefieldList(none, l));
    CHECK(l.battlemaster == kMe);
    CHECK(l.instanceIds.empty());
    checkTruncations(none, [&](std::span<const u8> p) { return net::parseBattlefieldList(p, l); });
    // A count beyond the packet is malformed.
    Bytes lying = list(kElfarran, kWarsongGulch, 4, {1});
    lying[13] = 200;
    CHECK_FALSE(net::parseBattlefieldList(lying, l));

    body.clear();
    net::writeBattlemasterJoin(body, kElfarran, kWarsongGulch, 0, false);
    CHECK(body == Bytes{0x4D, 0x00, 0x85, 0x3A, 0x00, 0x00, 0x30, 0xF1, 0xE9, 0x01, 0, 0, 0, 0, 0, 0, 0});
    body.clear();
    net::writeBattlemasterJoin(body, kMe, kArathiBasin, 7, true);
    CHECK(body == Bytes{0x11, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x02, 0, 0, 7, 0, 0, 0, 1});
    body.clear();
    net::writeBattlefieldJoin(body, kWarsongGulch, 3, false);
    CHECK(body == Bytes{0xE9, 0x01, 0, 0, 3, 0, 0, 0, 0});
    body.clear();
    net::writeBattlefieldListRequest(body, kAlteracValley);
    CHECK(body == Bytes{30, 0, 0, 0});

    i32 result = 0;
    REQUIRE(net::parseGroupJoinedBattleground(groupJoined(net::kGroupJoinDeserters), result));
    CHECK(result == -2);
    REQUIRE(net::parseGroupJoinedBattleground(groupJoined(net::kGroupJoinFailed), result));
    CHECK(result == -1);
    const Bytes joined = groupJoined(kWarsongGulch);
    REQUIRE(net::parseGroupJoinedBattleground(joined, result));
    CHECK(result == static_cast<i32>(kWarsongGulch));
    checkTruncations(joined,
                     [&](std::span<const u8> p) { return net::parseGroupJoinedBattleground(p, result); });
}

TEST_CASE("Battlefield queue: a battlemaster's tap is its gossip, or CMSG_BATTLEMASTER_HELLO without one") {
    Harness h;
    REQUIRE(h.store.applyUpdateObject(
        created(kElfarran, game::ObjectType::Unit,
                {{uf::OBJECT_FIELD_ENTRY, 14981},
                 {uf::UNIT_FIELD_HEALTH, 100},
                 {uf::UNIT_NPC_FLAGS, game::kNpcFlagGossip | game::kNpcFlagBattlemaster}},
                kHallX + 2.0f, kHallY, kHallZ),
        h.now));
    REQUIRE(h.store.applyUpdateObject(created(kBattlemaster, game::ObjectType::Unit,
                                              {{uf::OBJECT_FIELD_ENTRY, 99999},
                                               {uf::UNIT_FIELD_HEALTH, 100},
                                               {uf::UNIT_NPC_FLAGS, game::kNpcFlagBattlemaster}},
                                              kHallX, kHallY + 2.0f, kHallZ),
                                      h.now));
    // GOSSIP first (0x5F0130 tests the flags in order): the gossip's option asks for the list on the server.
    CHECK(h.play.interact(kElfarran) == game::Interaction::Gossip);
    CHECK(h.sentWith(Opcode::CMSG_GOSSIP_HELLO).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEMASTER_HELLO).empty());
    CHECK(h.play.interact(kBattlemaster) == game::Interaction::Battlemaster);
    REQUIRE(h.sentWith(Opcode::CMSG_BATTLEMASTER_HELLO).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEMASTER_HELLO)[0] == Bytes{0x01, 0, 0x99, 0x99, 0, 0, 0x30, 0xF1});
}

TEST_CASE("Battlefield queue: the list as GetBattlefieldInfo reads it, the selection and JoinBattlefield") {
    Harness h;
    game::BattlefieldQueueing& q = h.queue();
    CHECK(q.mapId() == 0);
    game::BattlefieldInfo info;
    CHECK_FALSE(q.info(info));
    CHECK_FALSE(q.join(0, false, h.now)); // no list: nothing sent

    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(kElfarran, kWarsongGulch, 4, {1, 2, 5})) ==
            game::Gameplay::Handled::Yes);
    const auto shown = h.events(GameEventKind::BattlefieldsShow);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == kWarsongGulch);
    CHECK(q.mapId() == kWarsongGulch);
    CHECK(q.battlemaster() == kElfarran);
    CHECK(q.count() == 3);
    CHECK(q.instanceId(0) == 1);
    CHECK(q.instanceId(2) == 5);
    CHECK(q.instanceId(3) == 0);
    REQUIRE(q.info(info));
    CHECK(info.name == "Warsong Gulch");
    CHECK(info.description == "A valley bordering Ashenvale Forest (Alliance)."); // a human: MapDescription1
    CHECK(info.minLevel == 10);
    CHECK(info.maxLevel == 60);
    CHECK(info.bracketMinLevel == 50); // 10 + 4 * 10
    CHECK(info.bracketMaxLevel == 59);
    CHECK(info.maxPlayers == 10);
    CHECK(q.canJoinAsGroup());
    CHECK_FALSE(q.groupLeader()); // alone

    // SetSelectedBattlefield / GetSelectedBattlefield: 0 is First Available, n the n-th battle, out of the
    // list 0.
    CHECK(q.selected() == 0);
    q.select(2);
    CHECK(q.selected() == 2);
    q.select(9);
    CHECK(q.selected() == 0);
    q.select(3);
    CHECK(q.selected() == 3);

    // JoinBattlefield(GetSelectedBattlefield()): the battle's id, then First Available (0).
    REQUIRE(q.join(q.selected(), false, h.now));
    q.select(0);
    REQUIRE(q.join(q.selected(), false, h.now));
    const auto joins = h.sentWith(Opcode::CMSG_BATTLEMASTER_JOIN);
    REQUIRE(joins.size() == 2);
    CHECK(joins[0] == Bytes{0x4D, 0x00, 0x85, 0x3A, 0x00, 0x00, 0x30, 0xF1, 0xE9, 0x01, 0, 0, 5, 0, 0, 0, 0});
    CHECK(joins[1] == Bytes{0x4D, 0x00, 0x85, 0x3A, 0x00, 0x00, 0x30, 0xF1, 0xE9, 0x01, 0, 0, 0, 0, 0, 0, 0});

    // Alterac Valley: no bracket size (its levels 0..0), no group join.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(kElfarran, kAlteracValley, 0, {})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(q.info(info));
    CHECK(info.bracketMinLevel == 0);
    CHECK(info.bracketMaxLevel == 0);
    CHECK_FALSE(q.canJoinAsGroup());
    CHECK(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);

    // A world entry forgets it (0x4A9DB0).
    h.play.enterWorld(kMe, kAzeroth, false, h.now);
    CHECK(q.mapId() == 0);
    CHECK(q.count() == 0);
    CHECK(q.selected() == 0);
    CHECK_FALSE(q.info(info));
}

TEST_CASE("Battlefield queue: the horde's description, a portal's list and one without a GUID") {
    Harness h(2); // an orc
    game::BattlefieldQueueing& q = h.queue();
    // A battleground's portal (its area trigger), the GM's .go warsong: the player's own GUID.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(kMe, kWarsongGulch, 4, {3})) ==
            game::Gameplay::Handled::Yes);
    game::BattlefieldInfo info;
    REQUIRE(q.info(info));
    CHECK(info.description == "A valley bordering Ashenvale Forest (Horde)."); // MapDescription0
    q.select(1);
    REQUIRE(q.join(q.selected(), false, h.now));
    REQUIRE(h.sentWith(Opcode::CMSG_BATTLEMASTER_JOIN).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEMASTER_JOIN)[0] ==
          Bytes{0x11, 0, 0, 0, 0, 0, 0, 0, 0xE9, 0x01, 0, 0, 3, 0, 0, 0, 0});
    // No GUID at all: CMSG_BATTLEFIELD_JOIN.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(0, kWarsongGulch, 4, {})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(q.join(0, false, h.now));
    REQUIRE(h.sentWith(Opcode::CMSG_BATTLEFIELD_JOIN).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_JOIN)[0] == Bytes{0xE9, 0x01, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("Battlefield queue: joining as a group, and the group too big for the map") {
    Harness h;
    game::BattlefieldQueueing& q = h.queue();
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(kElfarran, kWarsongGulch, 4, {1})) ==
            game::Gameplay::Handled::Yes);
    // A party the player leads: Join as Group enabled, sent with 1.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Aeryn", 0x21}, {"Brakk", 0x22}}, false)) ==
            game::Gameplay::Handled::Yes);
    CHECK(q.groupLeader());
    REQUIRE(q.join(0, true, h.now));
    CHECK(h.sentWith(Opcode::CMSG_BATTLEMASTER_JOIN).back() ==
          Bytes{0x4D, 0x00, 0x85, 0x3A, 0x00, 0x00, 0x30, 0xF1, 0xE9, 0x01, 0, 0, 0, 0, 0, 0, 1});
    // Led by someone else: disabled.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0x21, {{"Aeryn", 0x21}, {"Brakk", 0x22}}, false)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(q.groupLeader());
    // A raid of eleven for Warsong Gulch's ten: refused by the client itself.
    std::vector<std::pair<std::string, u64>> raid;
    for (u32 i = 0; i < 10; ++i) {
        raid.push_back({"Member" + std::to_string(i), 0x100 + i});
    }
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, raid, true)) == game::Gameplay::Handled::Yes);
    CHECK(q.groupLeader());
    h.sent.clear();
    h.events(GameEventKind::BattlefieldNotice);
    CHECK_FALSE(q.join(0, true, h.now));
    CHECK(h.sentWith(Opcode::CMSG_BATTLEMASTER_JOIN).empty());
    const auto notices = h.events(GameEventKind::BattlefieldNotice);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].value[0] == static_cast<i32>(BattlefieldLine::GroupTooMany));
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::Error, "Group too big for it"}});
    // Alone in the queue it still goes.
    REQUIRE(q.join(0, false, h.now));
}

TEST_CASE("Battlefield queue: SMSG_GROUP_JOINED_BATTLEGROUND's lines where WoW.exe puts them") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, groupJoined(kWarsongGulch)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, groupJoined(net::kGroupJoinDeserters)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, groupJoined(net::kGroupJoinFailed)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, groupJoined(4242)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, Bytes{1, 2}) ==
          game::Gameplay::Handled::Malformed);
    CHECK(h.lines() ==
          std::vector<Line>{
              {NoticeKind::Info, "Group queued for Warsong Gulch"},
              {NoticeKind::Info, "Group not queued: a Deserter."},
              {NoticeKind::Error, "Group queued, not you"},
              {NoticeKind::Error, "Group queued, not you"},
          });
}

TEST_CASE("Battlefield queue: walking 5.56 yd from where the list came closes it, once") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_LIST, list(kElfarran, kWarsongGulch, 4, {})) ==
            game::Gameplay::Handled::Yes);
    h.events(GameEventKind::BattlefieldsShow);
    h.moveTo(kHallX + 5.0f, kHallY, kHallZ);
    h.tick();
    CHECK(h.events(GameEventKind::BattlefieldsClosed).empty());
    h.moveTo(kHallX + 4.0f, kHallY + 4.0f, kHallZ);
    h.tick();
    REQUIRE(h.events(GameEventKind::BattlefieldsClosed).size() == 1);
    h.moveTo(kHallX + 30.0f, kHallY, kHallZ);
    h.tick();
    CHECK(h.events(GameEventKind::BattlefieldsClosed).empty());
    // The list itself stays (WoW.exe keeps it until the next world): only the frame hears
    // BATTLEFIELDS_CLOSED.
    CHECK(h.queue().mapId() == kWarsongGulch);
}

TEST_CASE("Battlefield queue: ShowBattlefieldList asks for a queued map's list") {
    Harness h;
    game::BattlefieldQueueing& q = h.queue();
    CHECK_FALSE(q.showList(0)); // empty
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 60000, 0)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kWarsongGulch, 4, 2, net::kBattlefieldStatusWaitJoin, 80000, 0)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(q.showList(0));
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_LIST)[0] == Bytes{0x11, 0x02, 0, 0});
    CHECK_FALSE(q.showList(1)); // "confirm": not while invited
    CHECK(h.sentWith(Opcode::CMSG_BATTLEFIELD_LIST).size() == 1);
}

TEST_CASE("Battlefield queue: BattlefieldFrame_UpdateStatus' sounds and popups") {
    Harness h;
    // Queued for Warsong Gulch: PVPENTERQUEUE once (IsAlreadyInQueue keeps it quiet on the next updates).
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 90000, 0)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    auto ev = h.allEvents();
    REQUIRE(countKind(ev, GameEventKind::BattlefieldQueueJoined) == 1);
    CHECK(std::find_if(ev.begin(), ev.end(), [](const game::GameEvent& e) {
              return e.kind == GameEventKind::BattlefieldQueueJoined;
          })->id == 0);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 85000, 30000)) ==
            game::Gameplay::Handled::Yes);
    // A second queue, Arathi Basin: only it sounds.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    ev = h.allEvents();
    REQUIRE(countKind(ev, GameEventKind::BattlefieldQueueJoined) == 1);
    // Invited to Warsong Gulch 3: PVPTHROUGHQUEUE and the popup, again on every update while it waits.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusWaitJoin, 80000, 0)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    ev = h.allEvents();
    CHECK(countKind(ev, GameEventKind::BattlefieldConfirm) == 1);
    CHECK(countKind(ev, GameEventKind::BattlefieldQueueJoined) == 0);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 20000)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(countKind(h.allEvents(), GameEventKind::BattlefieldConfirm) == 1);
    // Nothing changed: nothing again.
    h.tick();
    CHECK(h.allEvents().empty());
    // Back to the same character's world (a new map): the queues stay known, quiet.
    h.play.enterWorld(kMe, kAzeroth, false, h.now);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 25000)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(countKind(h.allEvents(), GameEventKind::BattlefieldQueueJoined) == 0);
    // A new character (a new interface): its queues sound again.
    h.play.leaveWorld();
    h.play.enterWorld(kMe, kAzeroth, true, h.now);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 30000)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(countKind(h.allEvents(), GameEventKind::BattlefieldQueueJoined) == 1);
}

TEST_CASE("Battlefield queue: the closing messages of BattlefieldFrame_OnUpdate") {
    SUBCASE("after the victory: INSTANCE_COMPLETE_MESSAGE at 2 min, 1 min, 45, 30 and 15 sec") {
        Harness h;
        h.play.enterWorld(kMe, kWarsongGulch, false, h.now);
        h.play.battlegrounds().setWinner(1); // the score's MSG_PVP_LOG_DATA (f17/scores)
        REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                         status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 120000, 900000)) ==
                game::Gameplay::Handled::Yes);
        h.tick();
        CHECK(h.queue().shutdownMs() > 119000);
        std::vector<i32> seconds;
        for (u32 i = 0; i < 125000 / 16; ++i) {
            h.tick();
            for (const game::GameEvent& e : h.events(GameEventKind::BattlefieldNotice)) {
                CHECK(e.value[0] == static_cast<i32>(BattlefieldLine::InstanceComplete));
                seconds.push_back(e.value[1]);
            }
        }
        CHECK(seconds == std::vector<i32>{120, 60, 45, 30, 15});
        CHECK(h.queue().shutdownMs() == 0);
        const std::vector<Line> lines = h.lines();
        REQUIRE(lines.size() == 5);
        CHECK(lines[0] == Line{NoticeKind::System, "Battle over: closes in 2 min"});
        CHECK(lines[4] == Line{NoticeKind::System, "Battle over: closes in 15 sec"});
    }
    SUBCASE("without a winner: INSTANCE_SHUTDOWN_MESSAGE; a world entry stops the count") {
        Harness h;
        h.play.enterWorld(kMe, kWarsongGulch, false, h.now);
        REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                         status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 400000, 60000)) ==
                game::Gameplay::Handled::Yes);
        std::vector<i32> seconds;
        for (u32 i = 0; i < 20000 / 16; ++i) {
            h.tick();
            for (const game::GameEvent& e : h.events(GameEventKind::BattlefieldNotice)) {
                CHECK(e.value[0] == static_cast<i32>(BattlefieldLine::InstanceShutdown));
                seconds.push_back(e.value[1]);
            }
        }
        CHECK(seconds == std::vector<i32>{600}); // 400 s under the 600 threshold: ceil(397 / 600) * 600
        CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Too few players: ends in 10 min"}});
        h.play.enterWorld(kMe, kAzeroth, false, h.now);
        CHECK(h.queue().shutdownMs() == 0);
        h.tick(10000);
        CHECK(h.events(GameEventKind::BattlefieldNotice).empty());
    }
    SUBCASE("a running battle (no expiration) counts nothing") {
        Harness h;
        h.play.enterWorld(kMe, kWarsongGulch, false, h.now);
        REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                         status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 0, 60000)) ==
                game::Gameplay::Handled::Yes);
        h.tick();
        CHECK(h.queue().shutdownMs() == 0);
    }
}

TEST_CASE(
    "Battlefield queue: MiniMapBattlefieldFrame's tooltip and menu as BattlefieldFrame.lua builds them") {
    std::vector<u8> table = test::textTable(kEnglish);
    text::setTable(table);
    using game::BattlefieldState;
    std::array<BattlefieldQueueView, 3> q{};
    std::array<char, 1024> out{};
    CHECK(battlefieldStatusText(out, q).empty()); // no queue: no tooltip (and the button hides)
    // Queued without an average yet, for 12 s: Unavailable; the right click's line after.
    q[0] = {BattlefieldState::Queued, "Arathi Basin", "Arathi Basin", 0, 12000, 0};
    CHECK(battlefieldStatusText(out, q) == "Queued: Arathi Basin\nAverage wait: Unavailable (last 10)\nWaited: "
                                           "12 sec\n|cffffffff<Right Click>: options|r");
    // Under a minute on average; then 3 minutes (no seconds), 4 min 12 s waited.
    q[0].averageWaitMs = 45000;
    CHECK(std::string_view(battlefieldStatusText(out, q)).find("Average wait: < 1 minute") !=
          std::string_view::npos);
    q[0].averageWaitMs = 185000;
    q[0].waitedMs = 252000;
    CHECK(std::string_view(battlefieldStatusText(out, q))
              .find("Average wait: 3 min (last 10)\nWaited: 4 min 12 sec") != std::string_view::npos);
    // Warsong Gulch 3 running in slot 0 and Alterac Valley 2 waiting in slot 2: "\n\n" between them.
    q[1] = q[0];
    q[0] = {BattlefieldState::Active, "Warsong Gulch 3", "Warsong Gulch", 0, 0, 0};
    q[2] = {BattlefieldState::Confirm, "Alterac Valley 2", "Alterac Valley", 0, 0, 80000};
    CHECK(battlefieldStatusText(out, q) == "In Warsong Gulch 3\n|cffffffff<Tap: score>\n<Again: map>|r\n\n"
                                           "Queued: Arathi Basin\nAverage wait: 3 min (last 10)\nWaited: 4 min "
                                           "12 sec\n\nReady: Alterac Valley 2\nInvite ends in 1 min 20 "
                                           "sec\n|cffffffff<Right Click>: options|r");
    // Only the battle: no right click's line.
    std::array<BattlefieldQueueView, 3> onlyBattle{};
    onlyBattle[1] = q[0];
    CHECK(std::string_view(battlefieldStatusText(out, onlyBattle)).find("Right Click") ==
          std::string_view::npos);

    // The menu: the queued and the invited ones, a spacer between them, titles without the instance.
    std::array<DropDownItemView, 11> items{};
    std::array<BattlefieldMenuAction, 11> actions{};
    std::array<u8, 11> slots{};
    REQUIRE(battlefieldMenu(q, items, actions, slots) == 7);
    CHECK(items[0].text == "Arathi Basin");
    CHECK(items[0].title);
    CHECK(items[1].text == "Change Instance");
    CHECK(actions[1] == BattlefieldMenuAction::ChangeInstance);
    CHECK(slots[1] == 1);
    CHECK(items[2].text == "Leave Queue");
    CHECK(actions[2] == BattlefieldMenuAction::LeaveQueue);
    CHECK(items[3].text.empty()); // the spacer
    CHECK(items[3].title);
    CHECK(items[4].text == "Alterac Valley");
    CHECK(items[5].text == "Enter Battle");
    CHECK(actions[5] == BattlefieldMenuAction::EnterBattle);
    CHECK(slots[5] == 2);
    CHECK(actions[6] == BattlefieldMenuAction::LeaveQueue);
    CHECK(battlefieldMenu(onlyBattle, items, actions, slots) == 0); // the battle alone: nothing in it
    text::unload();
}

TEST_CASE("Battlefield queue: CONFIRM_BATTLEFIELD_ENTRY is one popup per queue (multiple = 1)") {
    PopupStack s;
    Popup* first = s.show(PopupKind::ConfirmBattlefieldEntry, false, 0);
    REQUIRE(first);
    first->setName("Warsong Gulch 3");
    REQUIRE(s.show(PopupKind::ConfirmBattlefieldEntry, false, 2));
    CHECK(s.count() == 2);
    // The same queue again (the next status update): replaced in its own dialog, the other stays.
    REQUIRE(s.show(PopupKind::ConfirmBattlefieldEntry, false, 0));
    CHECK(s.count() == 2);
    CHECK(s.slots()[0].data == 0);
    CHECK(s.slots()[1].data == 2);
    // StaticPopup_Hide("CONFIRM_BATTLEFIELD_ENTRY", 2) hides only that one; whileDead: shown to the dead too.
    s.hide(PopupKind::ConfirmBattlefieldEntry, 2);
    CHECK(s.count() == 1);
    CHECK(s.show(PopupKind::ConfirmBattlefieldEntry, true, 1) != nullptr);
    // ENTER_BATTLE accepts with the queue's slot.
    PopupOutcome o;
    while (s.takeOutcome(o)) {
    }
    s.click(0, 1);
    REQUIRE(s.takeOutcome(o));
    CHECK(o.kind == PopupKind::ConfirmBattlefieldEntry);
    CHECK(o.end == PopupEnd::Accept);
    CHECK(o.data == 0);
}

TEST_CASE("Battlefield queue: the battleground's announcements in the chat, $n the sender") {
    game::ChatLine l;
    l.type = net::ChatType::BgSystemHorde;
    l.text.assign("$n grabbed the Alliance flag!");
    std::string out;
    game::appendChatLine(out, l, "Aeryn", 0);
    CHECK(out == "Aeryn grabbed the Alliance flag!");
    l.type = net::ChatType::BgSystemNeutral;
    l.text.assign("One minute until the gates open.");
    out.clear();
    game::appendChatLine(out, l, {}, 0);
    CHECK(out == "One minute until the gates open.");
}

TEST_CASE("Battlefield queue: Map.dbc's battleground rows as GetBattlefieldInfo reads them (1.12.1 data)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::AreaData areas;
    REQUIRE(areas.init(db));
    const game::MapInfo* wsg = areas.map(kWarsongGulch);
    REQUIRE(wsg);
    CHECK(wsg->name == "Warsong Gulch");
    CHECK(wsg->instanceType == 3);
    CHECK(wsg->minLevel == 10);
    CHECK(wsg->maxLevel == 60);
    CHECK(wsg->maxPlayers == 10);
    CHECK(wsg->bracketLevels == 10);
    CHECK(wsg->groupJoin);
    CHECK(wsg->descriptionAlliance.starts_with("A valley bordering Ashenvale Forest"));
    CHECK(wsg->descriptionHorde == wsg->descriptionAlliance);
    const game::MapInfo* ab = areas.map(kArathiBasin);
    REQUIRE(ab);
    CHECK(ab->minLevel == 20);
    CHECK(ab->maxPlayers == 15);
    CHECK(ab->groupJoin);
    const game::MapInfo* av = areas.map(kAlteracValley);
    REQUIRE(av);
    CHECK(av->minLevel == 51);
    CHECK(av->maxPlayers == 40);
    CHECK(av->bracketLevels == 0); // no bracket size: SMSG_BATTLEFIELD_LIST's levels stay 0..0
    CHECK_FALSE(av->groupJoin);    // CanJoinBattlefieldAsGroup: no Join as Group
    CHECK(av->descriptionHorde.starts_with("Hidden within the Alterac Mountains"));
}
