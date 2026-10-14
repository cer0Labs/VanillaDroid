// Phase 17 (scores): MSG_PVP_LOG_DATA, MSG_BATTLEGROUND_PLAYER_POSITIONS and SMSG_BATTLEGROUND_PLAYER_JOINED
// / _LEFT byte for byte as vmangos-core writes them (Server/Packets/Battleground.cpp PvpLogData,
// BattlegroundPlayerJoined / _Left; BattleGroundMgr::BuildPvpLogDataPacket with each battleground's extra
// fields; HandleBattleGroundPlayerPositionsOpcode), truncated packets rejected; and game::BattlefieldScore's
// rules as WoW.exe 1.12.1 keeps them (0x4AA170 .. 0x4AC3D0: the 5-second requests, the names awaited, the
// sides and the sort, the faction filter, the columns of an active queue, the positions without the group,
// the world entry) through game::Gameplay with synthetic packets.
#include "TextTable.h"
#include "core/Text.h"
#include "game/BattlefieldScore.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Notices.h"
#include "game/WorldStateUi.h"
#include "net/BattlegroundMessages.h"
#include "net/ByteBuffer.h"
#include "net/PvpLogMessages.h"
#include "net/SocialMessages.h"

#include <doctest/doctest.h>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::GameEventKind;
using net::Opcode;
using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x31;
constexpr u32 kWarsongGulch = 489, kArathiBasin = 529, kAlteracValley = 30;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

struct Line {
    u64 guid;
    u32 rank, kb, hk, deaths, honor;
    std::vector<u32> extra;
};

// PvpLogData::AppendBodyTo.
Bytes pvpLog(bool ended, u8 winner, const std::vector<Line>& lines) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(ended ? 1 : 0);
    if (ended) {
        w.u8v(winner);
    }
    w.u32v(static_cast<u32>(lines.size()));
    for (const Line& l : lines) {
        w.u64v(l.guid).u32v(l.rank).u32v(l.kb).u32v(l.hk).u32v(l.deaths).u32v(l.honor);
        w.u32v(static_cast<u32>(l.extra.size()));
        for (const u32 e : l.extra) {
            w.u32v(e);
        }
    }
    return out;
}

// HandleBattleGroundPlayerPositionsOpcode's answer.
Bytes positions(const std::vector<std::tuple<u64, f32, f32>>& team, bool carrier, u64 guid = 0, f32 x = 0.0f,
                f32 y = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(static_cast<u32>(team.size()));
    for (const auto& [g, px, py] : team) {
        w.u64v(g).f32v(px).f32v(py);
    }
    w.u8v(carrier ? 1 : 0);
    if (carrier) {
        w.u64v(guid).f32v(x).f32v(y);
    }
    return out;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::ByteWriter(out).u64v(guid);
    return out;
}

Bytes nameAnswer(u64 guid, const char* name, u32 race, u32 cls) {
    Bytes out;
    net::ByteWriter(out).u64v(guid).cstring(name).cstring("").u32v(race).u32v(0).u32v(cls);
    return out;
}

Bytes status(u32 slot, u32 map, u32 state, u32 time1, u32 time2) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(slot).u32v(map);
    if (map == 0) {
        return out;
    }
    w.u8v(4).u32v(1).u32v(state).u32v(time1);
    if (state == net::kBattlefieldStatusWaitQueue || state == net::kBattlefieldStatusInProgress) {
        w.u32v(time2);
    }
    return out;
}

// SMSG_GROUP_LIST of a battleground raid with these members (the player in subgroup 0).
Bytes raidList(std::vector<std::pair<const char*, u64>> members) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(net::kGroupTypeRaid).u8v(0).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid] : members) {
        w.cstring(name).u64v(guid).u8v(1).u8v(0);
    }
    w.u64v(kMe).u8v(3).u64v(0).u8v(2).u8v(0);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Stub texts with the formats of GlobalStrings.lua 1.12.1 (the game shows the client's own).
const std::vector<std::pair<std::string, std::string>> kEnglish = {
    {"ERR_BG_PLAYER_JOINED_SS", "|Hplayer:%s|h[%s]|h joins"},
    {"ERR_BG_PLAYER_LEFT_S", "%s leaves"},
};

std::vector<game::WorldStateUiRow> columns() {
    std::vector<game::WorldStateUiRow> rows;
    const auto add = [&](u32 id, i32 map, const char* text, u32 type) {
        game::WorldStateUiRow r;
        r.id = id;
        r.map = map;
        r.text = text;
        r.type = type;
        rows.push_back(r);
    };
    add(63, 30, "Graveyards Assaulted", 2);
    add(65, 30, "Graveyards Defended", 2);
    add(61, 30, "Towers Assaulted", 2);
    add(64, 30, "Towers Defended", 2);
    add(62, 30, "Mines Captured", 2);
    add(81, 30, "Leaders Killed", 2);
    add(82, 30, "Secondary Objectives", 2);
    add(2, 489, "%1581w/%1601w", 0);
    add(42, 489, "Flag Captures", 2);
    add(44, 489, "Flag Returns", 2);
    add(122, 529, "Bases Assaulted", 2);
    add(123, 529, "Bases Defended", 2);
    return rows;
}

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::WorldStateUiData data;
    game::NoticeFeed feed;
    std::vector<Sent> sent;
    std::vector<u8> texts = test::textTable(kEnglish);
    u32 now = 100000;
    u32 cursor = 0;
    u32 lineCursor = 0;

    explicit Harness(u32 map = kWarsongGulch) {
        text::setTable(texts);
        data.set(columns(), {22}, {"LocalDefense - %s"});
        play.worldStateUi().setData(&data);
        play.battlefieldScore().setData(&data);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, map, true, now);
        play.queries().setOnline(true);
        feed.init(nullptr, nullptr);
        feed.restart(play);
        cursor = play.events().end();
    }
    ~Harness() { text::unload(); }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    void tick(u32 ms = 16) {
        now += ms;
        play.update(now);
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
    u32 events(GameEventKind kind) {
        u32 n = 0;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            n += e->kind == kind ? 1 : 0;
        }
        return n;
    }
    void name(u64 guid, const char* who, u32 race, u32 cls) {
        REQUIRE(packet(Opcode::SMSG_NAME_QUERY_RESPONSE, nameAnswer(guid, who, race, cls)) ==
                game::Gameplay::Handled::Yes);
    }
    std::vector<std::string> lines() {
        game::CombatNames names;
        feed.update(play, store.find(kMe), names, now);
        std::vector<std::string> out;
        while (const game::Notice* n = feed.lines().next(lineCursor)) {
            out.emplace_back(n->text.data());
        }
        return out;
    }
    void active(u32 map) {
        REQUIRE(packet(Opcode::SMSG_BATTLEFIELD_STATUS, status(0, map, net::kBattlefieldStatusInProgress, 0,
                                                               60000)) == game::Gameplay::Handled::Yes);
        tick();
    }
    std::vector<std::string> order() {
        std::vector<std::string> out;
        const game::BattlefieldScore& s = play.battlefieldScore();
        for (u32 i = 0; i < s.count(); ++i) {
            out.emplace_back(s.name(s.line(i)));
        }
        return out;
    }
};

} // namespace

TEST_CASE("MSG_PVP_LOG_DATA: each battleground's extra fields, the end and its winner, byte for byte") {
    net::PvpLogData out;
    // Warsong Gulch mid-battle: flag captures, flag returns.
    const Bytes wsg = pvpLog(false, 0, {{0x101, 7, 3, 12, 2, 166, {1, 4}}, {0x102, 0, 0, 5, 6, 83, {0, 0}}});
    REQUIRE(net::parsePvpLogData(wsg, out));
    CHECK_FALSE(out.ended);
    REQUIRE(out.count == 2);
    CHECK(out.scores[0].guid == 0x101);
    CHECK(out.scores[0].rank == 7);
    CHECK(out.scores[0].killingBlows == 3);
    CHECK(out.scores[0].honorableKills == 12);
    CHECK(out.scores[0].deaths == 2);
    CHECK(out.scores[0].bonusHonor == 166);
    CHECK(out.scores[0].extraCount == 2);
    CHECK(out.scores[0].extra[0] == 1);
    CHECK(out.scores[0].extra[1] == 4);
    checkTruncations(wsg, [&](std::span<const u8> p) { return net::parsePvpLogData(p, out); });
    // Arathi Basin over, the Horde won: bases assaulted, defended.
    const Bytes ab = pvpLog(true, net::kPvpLogWinnerHorde, {{0x201, 4, 1, 9, 3, 420, {2, 1}}});
    REQUIRE(net::parsePvpLogData(ab, out));
    CHECK(out.ended);
    CHECK(out.winner == net::kPvpLogWinnerHorde);
    CHECK(out.scores[0].extra[0] == 2);
    checkTruncations(ab, [&](std::span<const u8> p) { return net::parsePvpLogData(p, out); });
    // Alterac Valley's seven.
    const Bytes av =
        pvpLog(true, net::kPvpLogWinnerAlliance, {{0x301, 9, 8, 40, 5, 900, {1, 2, 3, 4, 5, 6, 7}}});
    REQUIRE(net::parsePvpLogData(av, out));
    CHECK(out.winner == net::kPvpLogWinnerAlliance);
    CHECK(out.scores[0].extraCount == 7);
    CHECK(out.scores[0].extra[6] == 7);
    checkTruncations(av, [&](std::span<const u8> p) { return net::parsePvpLogData(p, out); });
    // More extra fields than WoW.exe keeps: read past, the first eight kept; a negative one stays negative
    // (int32).
    const Bytes many = pvpLog(false, 0, {{0x401, 0, 0, 0, 0, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFFFFFFFFu}}});
    REQUIRE(net::parsePvpLogData(many, out));
    CHECK(out.scores[0].extraCount == 10);
    CHECK(out.scores[0].extra[7] == 8);
    const Bytes negative = pvpLog(false, 0, {{0x402, 0, 0, 0, 0, 0, {0xFFFFFFFFu}}});
    REQUIRE(net::parsePvpLogData(negative, out));
    CHECK(out.scores[0].extra[0] == -1);
    // An empty score (the battle has nobody yet) and more than WoW.exe's 80.
    REQUIRE(net::parsePvpLogData(pvpLog(false, 0, {}), out));
    CHECK(out.count == 0);
    std::vector<Line> crowd(81, Line{0x500, 0, 0, 0, 0, 0, {}});
    CHECK_FALSE(net::parsePvpLogData(pvpLog(false, 0, crowd), out));
    crowd.resize(80);
    CHECK(net::parsePvpLogData(pvpLog(false, 0, crowd), out));
    CHECK(out.count == 80);
}

TEST_CASE("MSG_BATTLEGROUND_PLAYER_POSITIONS and SMSG_BATTLEGROUND_PLAYER_JOINED / _LEFT, byte for byte") {
    net::BattlegroundPositions out;
    const Bytes withCarrier =
        positions({{0x11, 1500.5f, 1450.25f}, {0x12, 1000.0f, 1300.0f}}, true, 0x13, 1200.0f, 1400.0f);
    REQUIRE(net::parseBattlegroundPositions(withCarrier, out));
    REQUIRE(out.count == 2);
    CHECK(out.players[0].guid == 0x11);
    CHECK(out.players[0].x == 1500.5f);
    CHECK(out.players[0].y == 1450.25f);
    CHECK(out.hasCarrier);
    CHECK(out.carrier.guid == 0x13);
    CHECK(out.carrier.x == 1200.0f);
    CHECK(out.carrier.y == 1400.0f);
    checkTruncations(withCarrier,
                     [&](std::span<const u8> p) { return net::parseBattlegroundPositions(p, out); });
    const Bytes none = positions({}, false); // in the team's raid: no teammates, no carrier
    REQUIRE(net::parseBattlegroundPositions(none, out));
    CHECK(out.count == 0);
    CHECK_FALSE(out.hasCarrier);
    checkTruncations(none, [&](std::span<const u8> p) { return net::parseBattlegroundPositions(p, out); });
    u64 player = 0;
    const Bytes joined = guidBody(0x0000000000ABCDEFull);
    REQUIRE(net::parseBattlegroundPlayer(joined, player));
    CHECK(player == 0xABCDEF);
    checkTruncations(joined, [&](std::span<const u8> p) { return net::parseBattlegroundPlayer(p, player); });
}

TEST_CASE(
    "Battlefield score: RequestBattlefieldScoreData at most every 5 s (0x4AA170), the request's empty body") {
    Harness h;
    CHECK(h.play.battlefieldScore().requestScore(h.now));
    CHECK_FALSE(h.play.battlefieldScore().requestScore(h.now + 16));
    CHECK_FALSE(h.play.battlefieldScore().requestScore(h.now + 4999));
    CHECK(h.play.battlefieldScore().requestScore(h.now + 5000));
    REQUIRE(h.sentWith(Opcode::MSG_PVP_LOG_DATA).size() == 2);
    CHECK(h.sentWith(Opcode::MSG_PVP_LOG_DATA)[0].empty());
    // A new world: the timer starts over (0x4A9DE5).
    h.play.enterWorld(kMe, kWarsongGulch, false, h.now + 5100);
    CHECK(h.play.battlefieldScore().requestScore(h.now + 5100));
}

TEST_CASE(
    "Battlefield score: the lines wait for their names, then sides, sort and UPDATE_BATTLEFIELD_SCORE") {
    Harness h;
    h.active(kWarsongGulch);
    h.events(GameEventKind::BattlefieldScore);
    // Alliance: Aeryn (human), Brakk (dwarf); Horde: Grok (orc), Zulja (troll).
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA, pvpLog(false, 0,
                                                      {{0x41, 0, 2, 10, 1, 100, {1, 0}},
                                                       {0x42, 7, 5, 12, 3, 100, {0, 2}},
                                                       {0x43, 0, 5, 9, 1, 50, {0, 0}},
                                                       {0x44, 0, 2, 10, 1, 100, {0, 1}}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 4); // every name asked for
    CHECK(h.play.battlefieldScore().count() == 0);
    CHECK(h.play.battlegrounds().winner() == -1);
    h.tick();
    CHECK(h.events(GameEventKind::BattlefieldScore) == 0); // not before the names
    h.name(0x41, "Aeryn", 1, 1);
    h.name(0x42, "Brakk", 3, 1);
    h.name(0x43, "Grok", 2, 1);
    h.tick();
    CHECK(h.events(GameEventKind::BattlefieldScore) == 0);
    h.name(0x44, "Zulja", 8, 3);
    h.tick();
    CHECK(h.events(GameEventKind::BattlefieldScore) == 1);
    const game::BattlefieldScore& s = h.play.battlefieldScore();
    REQUIRE(s.count() == 4);
    // Killing blows down (Grok 5 and 1 death before Brakk 5 and 3), then deaths up, honor down, name up.
    CHECK(h.order() == std::vector<std::string>{"Grok", "Brakk", "Aeryn", "Zulja"});
    CHECK(s.line(0).faction == 0);
    CHECK(s.line(1).faction == 1);
    CHECK(s.line(1).rank == 7);
    CHECK(s.line(1).race == 3);
    CHECK(s.line(3).cls == 3);
    CHECK(s.line(1).stats[1] == 2);
    // SetBattlefieldScoreFaction(1): the Alliance first, GetNumBattlefieldScores counts them.
    h.play.battlefieldScore().setFaction(1, h.now);
    CHECK(s.count() == 2);
    CHECK(h.order() == std::vector<std::string>{"Brakk", "Aeryn"});
    h.play.battlefieldScore().setFaction(0, h.now);
    CHECK(h.order() == std::vector<std::string>{"Grok", "Zulja"});
    h.play.battlefieldScore().setFaction(5, h.now); // ignored (0x4AA5A0)
    CHECK(s.faction() == 0);
    h.play.battlefieldScore().setFaction(-1, h.now);
    CHECK(s.count() == 4);
    CHECK(h.events(GameEventKind::BattlefieldScore) == 3); // each filter sorts again and fires it
    // The columns of the active queue (Warsong Gulch: flag captures and returns).
    REQUIRE(s.statCount() == 2);
    CHECK(s.stat(0)->text == "Flag Captures");
    CHECK(s.stat(1)->text == "Flag Returns");
    CHECK(s.stat(2) == nullptr);
}

TEST_CASE("Battlefield score: the victory sets GetBattlefieldWinner at once; a draw is 2 (WINNER_NONE)") {
    Harness h;
    h.active(kWarsongGulch);
    h.name(0x51, "Aeryn", 1, 1);
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA,
                     pvpLog(true, net::kPvpLogWinnerAlliance, {{0x51, 0, 1, 1, 0, 300, {3, 0}}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.play.battlefieldScore().ended());
    CHECK(h.play.battlegrounds().ended());
    CHECK(h.play.battlegrounds().winner() == 1);
    CHECK(h.play.battlefieldScore().count() == 1); // the name was known: sorted at once
    // Leave Battleground (LeaveBattlefield, 0x4ABE60): now it goes, with the active queue's map.
    REQUIRE(h.play.battlegrounds().leave());
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_BATTLEFIELD).back() == Bytes{0xE9, 0x01, 0, 0});
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA, pvpLog(true, net::kPvpLogWinnerNone, {})) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.play.battlegrounds().winner() == 2);
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA, pvpLog(false, 0, {})) == game::Gameplay::Handled::Yes);
    CHECK(h.play.battlegrounds().winner() == -1);
    CHECK_FALSE(h.play.battlefieldScore().ended());
    CHECK_FALSE(h.play.battlegrounds().leave()); // not before the end
    // SMSG_BATTLEFIELD_WIN / _LOSE: consumed, nothing (no handler in 1.12.1).
    CHECK(h.packet(Opcode::SMSG_BATTLEFIELD_WIN, {}) == game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_BATTLEFIELD_LOSE, {}) == game::Gameplay::Handled::Yes);
    // Malformed: nothing changes.
    CHECK(h.packet(Opcode::MSG_PVP_LOG_DATA, Bytes{1}) == game::Gameplay::Handled::Malformed);
    // A world entry clears the winner and the score (0x4A9DB0).
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA, pvpLog(true, 0, {{0x51, 0, 1, 1, 0, 300, {3, 0}}})) ==
            game::Gameplay::Handled::Yes);
    h.play.enterWorld(kMe, 0, false, h.now);
    CHECK(h.play.battlegrounds().winner() == -1);
    CHECK(h.play.battlefieldScore().count() == 0);
    CHECK_FALSE(h.play.battlefieldScore().ended());
    CHECK(h.play.battlefieldScore().statCount() == 0);
}

TEST_CASE("Battlefield score: Alterac Valley's 80 players and seven columns; Arathi Basin's two") {
    Harness h(kAlteracValley);
    h.active(kAlteracValley);
    std::vector<Line> lines;
    for (u32 i = 0; i < 80; ++i) {
        const u64 guid = 0x1000 + i;
        lines.push_back({guid, i % 19, i % 7, 20 + i, i % 5, 10 * i, {1, 2, 3, 4, 5, 6, i}});
        const std::string name = "Player" + std::to_string(i);
        h.name(guid, name.c_str(), i % 2 == 0 ? 1 : 2, 1);
    }
    REQUIRE(h.packet(Opcode::MSG_PVP_LOG_DATA, pvpLog(false, 0, lines)) == game::Gameplay::Handled::Yes);
    const game::BattlefieldScore& s = h.play.battlefieldScore();
    REQUIRE(s.count() == 80);
    REQUIRE(s.statCount() == 7);
    CHECK(s.stat(6)->text == "Secondary Objectives");
    for (u32 i = 1; i < 80; ++i) { // killing blows never rise down the list
        CHECK(s.line(i - 1).killingBlows >= s.line(i).killingBlows);
    }
    h.play.battlefieldScore().setFaction(1, h.now);
    CHECK(s.count() == 40);
    // A status for another queue does not touch the filter; Arathi Basin turning active takes its columns.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, net::kBattlefieldStatusWaitQueue, 0, 0)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(s.faction() == 1);
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS,
                     status(1, kArathiBasin, net::kBattlefieldStatusInProgress, 0, 1000)) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(s.faction() == -1); // 0x4AAA57
    REQUIRE(s.statCount() == 2);
    CHECK(s.stat(0)->text == "Bases Assaulted");
    // Its queue emptied: the columns go.
    REQUIRE(h.packet(Opcode::SMSG_BATTLEFIELD_STATUS, status(1, 0, 0, 0, 0)) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(s.statCount() == 0);
}

TEST_CASE("Battlefield positions: requested every 5 s while a queue is active; the group's own are skipped") {
    Harness h;
    CHECK_FALSE(h.play.battlefieldScore().requestPositions(h.now)); // nothing active (0x4AA5DF)
    h.active(kWarsongGulch);
    CHECK(h.play.battlefieldScore().requestPositions(h.now));
    CHECK_FALSE(h.play.battlefieldScore().requestPositions(h.now + 4000));
    CHECK(h.play.battlefieldScore().requestPositions(h.now + 5000));
    REQUIRE(h.sentWith(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS).size() == 2);
    CHECK(h.sentWith(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS)[0].empty());
    // The player, a raid member and two others; the Alliance carrier of the Horde flag.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, raidList({{"Brakk", 0x62}})) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS, positions({{kMe, 1500.0f, 1450.0f},
                                                                           {0x61, 1100.0f, 1400.0f},
                                                                           {0x62, 1000.0f, 1000.0f},
                                                                           {0x63, 1200.0f, 1300.0f}},
                                                                          true, 0x61, 1100.0f, 1400.0f)) ==
            game::Gameplay::Handled::Yes);
    game::BattlefieldScore& s = h.play.battlefieldScore();
    REQUIRE(s.positionCount() == 2);
    game::BattlefieldPosition p;
    REQUIRE(s.position(0, h.now, p));
    CHECK(p.guid == 0x61);
    CHECK(p.x == 1100.0f);
    CHECK(p.name.empty()); // asked for
    h.name(0x61, "Aeryn", 4, 11);
    REQUIRE(s.position(0, h.now, p));
    CHECK(p.name == "Aeryn");
    REQUIRE(s.position(1, h.now, p));
    CHECK(p.guid == 0x63);
    CHECK_FALSE(s.position(2, h.now, p));
    game::BattlefieldFlag flag;
    REQUIRE(s.flagCarrier(h.now, flag));
    CHECK(flag.guid == 0x61);
    CHECK(flag.y == 1400.0f);
    CHECK(flag.token == game::BattlefieldFlag::Token::Unknown); // the player's race not known yet
    h.name(kMe, "Arthel", 4, 1); // a night elf: the Alliance's carrier holds the Horde flag (0x4AA670)
    REQUIRE(s.flagCarrier(h.now, flag));
    CHECK(flag.token == game::BattlefieldFlag::Token::HordeFlag);
    REQUIRE(h.packet(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS, positions({}, false)) ==
            game::Gameplay::Handled::Yes);
    CHECK(s.positionCount() == 0);
    CHECK_FALSE(s.flagCarrier(h.now, flag));
    CHECK(h.packet(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS, Bytes{1, 0, 0}) ==
          game::Gameplay::Handled::Malformed);
}

TEST_CASE(
    "Battleground players joining and leaving: ERR_BG_PLAYER_JOINED_SS / _LEFT_S once the name is known") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED, guidBody(0x71)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY) == std::vector<Bytes>{guidBody(0x71)});
    CHECK(h.lines().empty()); // waiting for the name
    h.name(0x71, "Aeryn", 1, 1);
    CHECK(h.lines() == std::vector<std::string>{"|Hplayer:Aeryn|h[Aeryn]|h joins"});
    REQUIRE(h.packet(Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT, guidBody(0x71)) == game::Gameplay::Handled::Yes);
    CHECK(h.lines() == std::vector<std::string>{"Aeryn leaves"});
    CHECK(h.packet(Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT, Bytes{1, 2}) == game::Gameplay::Handled::Malformed);
}
