// Phase 17 (scores): GetNumWorldStateUI / GetWorldStateUIInfo (game::WorldStateUi) as WoW.exe 1.12.1 builds
// them over WorldStateUI.dbc and the world states (0x4C5650 .. 0x4C5BFA: the list by map, zone and type, the
// LocalDefense rule of 0x49BD50, the "%NNNNw" texts of 0x508560), driven through game::Gameplay with the
// packets vmangos-core sends (Player::SendInitWorldStates, BattleGround*::FillInitialWorldStates,
// UpdateWorldState, OutdoorPvPEP / SI); with VD_PACKS_DIR, against the client's own WorldStateUI.dbc and
// ChatChannels.dbc (db.vdpk).
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/WorldStateUi.h"
#include "game/WorldStates.h"
#include "net/ByteBuffer.h"
#include "net/SocialMessages.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::GameEventKind;
using net::Opcode;
using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x21;
constexpr u32 kWarsongGulch = 489, kWarsongGulchZone = 3277;
constexpr u32 kArathiBasin = 529, kArathiBasinZone = 3358;
constexpr u32 kAlteracValley = 30, kAlteracValleyZone = 2597;
constexpr u32 kEasternKingdoms = 0, kEasternPlaguelands = 139;
constexpr u32 kKalimdor = 1, kSilithus = 1377;
constexpr u32 kLocalDefense = 22; // ChatChannels.dbc

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

Bytes youJoined(const char* channel) {
    Bytes out;
    net::ByteWriter(out)
        .u8v(static_cast<u8>(net::ChannelNotice::YouJoined))
        .cstring(channel)
        .u32v(0x18)
        .u32v(0);
    return out;
}

Bytes youLeft(const char* channel) {
    Bytes out;
    net::ByteWriter(out).u8v(static_cast<u8>(net::ChannelNotice::YouLeft)).cstring(channel);
    return out;
}

// The 1.12.1 rows this file needs, as the client's table has them (dumped from WorldStateUI.dbc), in its
// order.
std::vector<game::WorldStateUiRow> sampleRows() {
    using game::WorldStateUiRow;
    std::vector<WorldStateUiRow> rows;
    const auto row = [&](u32 id, i32 map, u32 area, const char* icon, const char* text, const char* tooltip,
                         u32 state, u32 type) -> WorldStateUiRow& {
        WorldStateUiRow r;
        r.id = id;
        r.map = map;
        r.area = area;
        r.icon = icon;
        r.text = text;
        r.tooltip = tooltip;
        r.stateVariable = state;
        r.type = type;
        rows.push_back(r);
        return rows.back();
    };
    row(136, 0, 139, "Interface\\WorldStateFrame\\AllianceTower", "Towers Controlled: %2327w",
        "Alliance Towers Controlled", 0, 1);
    row(137, 0, 139, "Interface\\WorldStateFrame\\HordeTower", "Towers Controlled: %2328w",
        "Horde Towers Controlled", 0, 1);
    WorldStateUiRow& bar = row(138, 0, 139, "", "Progress: %2427w", "", 2426, 1);
    bar.extendedUi = "CAPTUREPOINT";
    bar.extendedStates = {2427, 2428, 0};
    row(126, 1, 1377, "Interface\\TargetingFrame\\UI-PVP-Alliance", "%2313w/%2317w",
        "Alliance Silithyst Collected", 0, 1);
    row(127, 1, 1377, "Interface\\TargetingFrame\\UI-PVP-Horde", "%2314w/%2317w", "Horde Silithyst Collected",
        0, 1);
    row(63, 30, 0, "Interface\\WorldStateFrame\\ColumnIcon-GraveyardCapture", "Graveyards Assaulted", "", 0,
        2);
    row(65, 30, 0, "Interface\\WorldStateFrame\\ColumnIcon-GraveyardDefend", "Graveyards Defended", "", 0, 2);
    row(61, 30, 0, "Interface\\WorldStateFrame\\ColumnIcon-TowerCapture", "Towers Assaulted", "", 0, 2);
    row(64, 30, 0, "Interface\\WorldStateFrame\\ColumnIcon-TowerDefend", "Towers Defended", "", 0, 2);
    row(62, 30, 0, "Interface\\WorldStateFrame\\ColumnIcon-MineCapture", "Mines Captured", "", 0, 2);
    row(81, 30, 0, "", "Leaders Killed", "", 0, 2);
    row(82, 30, 0, "", "Secondary Objectives", "", 0, 2);
    WorldStateUiRow& alliance = row(2, 489, 0, "Interface\\TargetingFrame\\UI-PVP-Alliance", "%1581w/%1601w",
                                    "Alliance flag captures", 2339, 0);
    alliance.dynamicIcon = "Interface\\WorldStateFrame\\HordeFlag";
    alliance.dynamicTooltip = "Horde flag has been picked up";
    WorldStateUiRow& horde = row(3, 489, 0, "Interface\\TargetingFrame\\UI-PVP-Horde", "%1582w/%1601w",
                                 "Horde flag captures", 2338, 0);
    horde.dynamicIcon = "Interface\\WorldStateFrame\\AllianceFlag";
    horde.dynamicTooltip = "Alliance flag has been picked up";
    row(42, 489, 0, "Interface\\WorldStateFrame\\ColumnIcon-FlagCapture", "Flag Captures", "", 0, 2);
    row(44, 489, 0, "Interface\\WorldStateFrame\\ColumnIcon-FlagReturn", "Flag Returns", "", 0, 2);
    row(101, 529, 0, "Interface\\TargetingFrame\\UI-PVP-Alliance", "Bases: %1779w  Resources: %1776w/%1780w",
        "Alliance status", 0, 0);
    row(102, 529, 0, "Interface\\TargetingFrame\\UI-PVP-Horde", "Bases: %1778w  Resources: %1777w/%1780w",
        "Horde status", 0, 0);
    row(122, 529, 0, "Interface\\WorldStateFrame\\ColumnIcon-TowerCapture", "Bases Assaulted", "", 0, 2);
    row(123, 529, 0, "Interface\\WorldStateFrame\\ColumnIcon-TowerDefend", "Bases Defended", "", 0, 2);
    return rows;
}

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::WorldStateUiData data;
    u32 now = 50000;
    u32 cursor = 0;

    explicit Harness(u32 map = kEasternKingdoms, game::WorldStateUiData* real = nullptr) {
        if (!real) {
            data.set(sampleRows(), {kLocalDefense}, {"LocalDefense - %s"});
        }
        play.worldStateUi().setData(real ? real : &data);
        play.battlefieldScore().setData(real ? real : &data);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void*, Opcode, std::span<const u8>) { return true; };
        play.setSink(sink);
        play.enterWorld(kMe, map, true, now);
        cursor = play.events().end();
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    void tick(u32 ms = 16) {
        now += ms;
        play.update(now);
    }
    u32 events(GameEventKind kind) {
        u32 n = 0;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            n += e->kind == kind ? 1 : 0;
        }
        return n;
    }
    // GetWorldStateUIInfo(i + 1) as a tuple of what the tests check.
    std::string text(u32 i) {
        char buffer[256];
        return std::string(play.worldStateUi().info(i, buffer).text);
    }
    u32 id(u32 i) {
        char buffer[256];
        return play.worldStateUi().info(i, buffer).id;
    }
    u32 state(u32 i) {
        char buffer[256];
        return play.worldStateUi().info(i, buffer).state;
    }
};

} // namespace

TEST_CASE("World state texts: %NNNNw is the state's value, as WoW.exe 0x508560 writes it") {
    game::WorldStates::Events events;
    game::WorldStates states(events);
    const net::WorldStatePair pairs[] = {{1581, 2}, {1601, 3}, {1776, 1450}};
    states.demoInit(489, 3277, pairs, 0);
    char out[64];
    CHECK(game::formatWorldStateText("%1581w/%1601w", states, out) == "2/3");
    CHECK(game::formatWorldStateText("Resources: %1776W/%1780w", states, out) ==
          "Resources: 1450/0"); // unset: 0
    CHECK(game::formatWorldStateText("50%", states, out) == "50%");
    CHECK(game::formatWorldStateText("%12x and %w", states, out) ==
          "%x and 0"); // the digits go, the '%' stays
    CHECK(game::formatWorldStateText("plain", states, out) == "plain");
    char tiny[4];
    CHECK(game::formatWorldStateText("%1581w/%1601w", states, tiny) == "2/3"); // truncated to the buffer
    states.demoSet(1581, 0xFFFFFFFFu, 0);
    CHECK(game::formatWorldStateText("%1581w", states, out) == "-1"); // %d of the u32
}

TEST_CASE("World state UI: nothing before the first INIT; Warsong Gulch's two lines with their states") {
    Harness h;
    h.tick();
    CHECK(h.play.worldStateUi().count() == 0);
    // BattleGroundWS::FillInitialWorldStates: captures, taken flags, max, flag states (1: at base, 2:
    // carried).
    REQUIRE(
        h.packet(Opcode::SMSG_INIT_WORLD_STATES,
                 initStates(kWarsongGulch, kWarsongGulchZone,
                            {{1581, 1}, {1582, 0}, {1545, 0}, {1546, 1}, {1601, 3}, {2339, 2}, {2338, 1}})) ==
        game::Gameplay::Handled::Yes);
    h.tick();
    REQUIRE(h.play.worldStateUi().count() == 2);
    CHECK(h.id(0) == 2);
    CHECK(h.text(0) == "1/3");
    CHECK(h.state(0) == 2); // the Horde flag is carried: the Alliance line flashes its dynamic icon
    CHECK(h.id(1) == 3);
    CHECK(h.text(1) == "0/3");
    CHECK(h.state(1) == 1);
    char buffer[64];
    const game::WorldStateUiInfo info = h.play.worldStateUi().info(0, buffer);
    CHECK(info.icon == "Interface\\TargetingFrame\\UI-PVP-Alliance");
    CHECK(info.dynamicIcon == "Interface\\WorldStateFrame\\HordeFlag");
    CHECK(info.tooltip == "Alliance flag captures");
    CHECK(info.dynamicTooltip == "Horde flag has been picked up");
    CHECK(info.extendedUi.empty());
    // UpdateWorldState: a capture, the flag back at its base.
    h.events(GameEventKind::WorldStates);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(1581, 2)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2339, 1)) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(h.events(GameEventKind::WorldStates) == 2); // UPDATE_WORLD_STATES each
    CHECK(h.text(0) == "2/3");
    CHECK(h.state(0) == 1);
    // Past the list: GetWorldStateUIInfo's state 0.
    CHECK(h.state(2) == 0);
    CHECK(h.text(2).empty());
}

TEST_CASE(
    "World state UI: Arathi Basin's lines, none for Alterac Valley (1.12.1 has only its score columns)") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES,
                     initStates(kArathiBasin, kArathiBasinZone,
                                {{1779, 2}, {1778, 3}, {1776, 1240}, {1777, 1500}, {1780, 2000}})) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    REQUIRE(h.play.worldStateUi().count() == 2);
    CHECK(h.text(0) == "Bases: 2  Resources: 1240/2000");
    CHECK(h.text(1) == "Bases: 3  Resources: 1500/2000");
    CHECK(h.state(0) == 1); // no state variable: always shown
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kAlteracValley, kAlteracValleyZone,
                                                                {{1, 1}})) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(h.play.worldStateUi().count() == 0);
    // The score columns come from the same table (type 2), not this list.
    const game::WorldStateUiRow* columns[8] = {};
    CHECK(h.data.statColumns(kAlteracValley, columns) == 7);
    CHECK(columns[0]->text == "Graveyards Assaulted");
    CHECK(columns[6]->text == "Secondary Objectives");
    CHECK(h.data.statColumns(kWarsongGulch, columns) == 2);
    CHECK(columns[0]->text == "Flag Captures");
    CHECK(columns[1]->text == "Flag Returns");
    CHECK(h.data.statColumns(kArathiBasin, columns) == 2);
    CHECK(columns[0]->text == "Bases Assaulted");
    CHECK(h.data.statColumns(kEasternKingdoms, columns) == 0);
}

TEST_CASE(
    "World state UI: the zone objectives only while in LocalDefense (0x49BD50), with UPDATE_WORLD_STATES") {
    Harness h(kEasternKingdoms);
    // OutdoorPvPEP::FillInitialWorldStates: towers controlled, the capture bar while near a tower.
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES,
                     initStates(kEasternKingdoms, kEasternPlaguelands,
                                {{2327, 1}, {2328, 2}, {2426, 1}, {2427, 70}, {2428, 20}})) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(h.play.worldStateUi().count() == 0); // not in the zone's LocalDefense channel
    h.events(GameEventKind::WorldStates);
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, youJoined("LocalDefense - Eastern Plaguelands")) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(h.play.worldStateUi().inLocalDefense());
    CHECK(h.events(GameEventKind::WorldStates) == 1); // 0x49BDD6
    REQUIRE(h.play.worldStateUi().count() == 3);
    CHECK(h.text(0) == "Towers Controlled: 1");
    CHECK(h.text(1) == "Towers Controlled: 2");
    char buffer[64];
    const game::WorldStateUiInfo bar = h.play.worldStateUi().info(2, buffer);
    CHECK(bar.extendedUi == "CAPTUREPOINT");
    CHECK(bar.state == 1); // WS_UI_TOWER_SLIDER_DISPLAY
    CHECK(bar.extendedStates[0] == 70);
    CHECK(bar.extendedStates[1] == 20);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2426, 0)) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(h.state(2) == 0); // out of the tower's reach: hidden
    // Silithus has its own rows; the same channel rule.
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES,
                     initStates(kKalimdor, kSilithus, {{2313, 7}, {2314, 12}, {2317, 200}})) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    REQUIRE(h.play.worldStateUi().count() == 2);
    CHECK(h.text(0) == "7/200");
    CHECK(h.text(1) == "12/200");
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, youLeft("LocalDefense - Eastern Plaguelands")) ==
            game::Gameplay::Handled::Yes);
    h.tick();
    CHECK_FALSE(h.play.worldStateUi().inLocalDefense());
    CHECK(h.play.worldStateUi().count() == 0);
    CHECK(h.events(GameEventKind::WorldStates) == 3); // the slider's update, Silithus' INIT and the channel's
}

TEST_CASE("World states: INIT replaces the set (0x4C5650 frees every value), leaving the world empties it") {
    Harness h;
    REQUIRE(
        h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kWarsongGulch, kWarsongGulchZone, {{1581, 3}})) ==
        game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(9999, 4)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.worldStates().value(9999) == 4);
    REQUIRE(
        h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kArathiBasin, kArathiBasinZone, {{1776, 5}})) ==
        game::Gameplay::Handled::Yes);
    CHECK_FALSE(h.play.worldStates().has(1581));
    CHECK_FALSE(h.play.worldStates().has(9999));
    CHECK(h.play.worldStates().value(1776) == 5);
    CHECK(h.play.worldStates().known());
    h.tick();
    CHECK(h.play.worldStateUi().count() == 2);
    h.play.leaveWorld();
    CHECK_FALSE(h.play.worldStates().known());
    CHECK(h.play.worldStateUi().count() == 0);
}

TEST_CASE("World state UI against the client's WorldStateUI.dbc (VD_PACKS_DIR)") {
    pack::PackReader db;
    game::WorldStateUiData data;
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() || !data.init(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    REQUIRE(data.rows().size() == 20);
    // Warsong Gulch.
    const game::WorldStateUiRow* wsg = data.row(2);
    REQUIRE(wsg);
    CHECK(wsg->map == 489);
    CHECK(wsg->text == "%1581w/%1601w");
    CHECK(wsg->stateVariable == 2339);
    CHECK(wsg->type == game::kWorldStateUiAlwaysUp);
    CHECK(wsg->icon == "Interface\\TargetingFrame\\UI-PVP-Alliance");
    CHECK(wsg->dynamicIcon == "Interface\\WorldStateFrame\\HordeFlag");
    CHECK(wsg->dynamicTooltip == "Horde flag has been picked up");
    REQUIRE(data.row(3));
    CHECK(data.row(3)->stateVariable == 2338);
    // Arathi Basin.
    REQUIRE(data.row(101));
    CHECK(data.row(101)->text == "Bases: %1779w  Resources: %1776w/%1780w");
    CHECK(data.row(102)->tooltip == "Horde status");
    // Silithus and the Eastern Plaguelands: zone objectives.
    REQUIRE(data.row(126));
    CHECK(data.row(126)->area == 1377);
    CHECK(data.row(126)->type == game::kWorldStateUiZone);
    CHECK(data.row(127)->text == "%2314w/%2317w");
    REQUIRE(data.row(138));
    CHECK(data.row(138)->area == 139);
    CHECK(data.row(138)->extendedUi == "CAPTUREPOINT");
    CHECK(data.row(138)->stateVariable == 2426);
    CHECK(data.row(138)->extendedStates[0] == 2427);
    CHECK(data.row(138)->extendedStates[1] == 2428);
    CHECK(data.row(136)->icon == "Interface\\WorldStateFrame\\AllianceTower");
    // Score columns: Alterac Valley's seven, in the table's order.
    const game::WorldStateUiRow* columns[game::kMaxStatColumns] = {};
    REQUIRE(data.statColumns(30, columns) == 7);
    const char* av[] = {"Graveyards Assaulted", "Graveyards Defended", "Towers Assaulted",
                        "Towers Defended",      "Mines Captured",      "Leaders Killed",
                        "Secondary Objectives"};
    for (u32 i = 0; i < 7; ++i) {
        CHECK(columns[i]->text == av[i]);
    }
    CHECK(columns[0]->icon == "Interface\\WorldStateFrame\\ColumnIcon-GraveyardCapture");
    CHECK(columns[5]->icon.empty());
    REQUIRE(data.statColumns(489, columns) == 2);
    CHECK(columns[0]->text == "Flag Captures");
    CHECK(columns[0]->tooltip.find("enemy flag") != std::string_view::npos);
    REQUIRE(data.statColumns(529, columns) == 2);
    CHECK(columns[1]->text == "Bases Defended");
    // LocalDefense is the only defense channel with the zone's name.
    CHECK(data.defenseChannel(22, {}));
    CHECK_FALSE(data.defenseChannel(23, {})); // WorldDefense
    CHECK_FALSE(data.defenseChannel(1, {}));  // General
    CHECK(data.defenseChannel(0, "LocalDefense - Silithus"));
    CHECK_FALSE(data.defenseChannel(0, "General - Silithus"));

    // The lists through Gameplay with the real table.
    Harness wsgRun(kWarsongGulch, &data);
    REQUIRE(wsgRun.packet(Opcode::SMSG_INIT_WORLD_STATES,
                          initStates(kWarsongGulch, kWarsongGulchZone, {{1581, 0}, {1582, 2}, {1601, 3}})) ==
            game::Gameplay::Handled::Yes);
    wsgRun.tick();
    REQUIRE(wsgRun.play.worldStateUi().count() == 2);
    CHECK(wsgRun.text(1) == "2/3");
    Harness ep(kEasternKingdoms, &data);
    REQUIRE(ep.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kEasternKingdoms, kEasternPlaguelands,
                                                                 {{2327, 4}, {2426, 1}, {2427, 50}})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(ep.packet(Opcode::SMSG_CHANNEL_NOTIFY, youJoined("LocalDefense - Eastern Plaguelands")) ==
            game::Gameplay::Handled::Yes);
    ep.tick();
    REQUIRE(ep.play.worldStateUi().count() == 3);
    CHECK(ep.text(0) == "Towers Controlled: 4");
    CHECK(ep.text(2) == "Progress: 50");
}
