#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/WorldStateUi.h"
#include "net/Opcodes.gen.h"
#include "net/PvpLogMessages.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Battlegrounds;
class EntityStore;
class QueryCache;
class Social;

// GetBattlefieldScore(i)'s line (WoW.exe's 0x40-byte entries at 0xB6E880): the server's numbers, the side
// from the name query's race (0x4AA26F: the race's faction template's group, 0 Horde, 1 Alliance, -1
// neither), the race and class for the name's tooltip, and the battleground's columns
// (GetBattlefieldStatData).
struct BattlefieldScoreLine {
    u64 guid = 0;
    u32 rank = 0; // HonorMgr's rank: GetPVPRankInfo (game/CharacterSheet pvpRankKey / pvpRankNumber)
    u32 killingBlows = 0, honorableKills = 0, deaths = 0, bonusHonor = 0;
    i32 faction = -1;
    u8 race = 0, cls = 0;
    std::array<i32, net::kPvpLogExtraFields> stats{};
};

// GetBattlefieldPosition(i) / GetBattlefieldFlagPosition(i): a teammate out of the player's group, or the
// flag carrier, where the client last knows it (world coordinates: the unit itself while in sight, else the
// packet's).
struct BattlefieldPosition {
    u64 guid = 0;
    f32 x = 0.0f, y = 0.0f;
    std::string_view name; // empty until its name query answers
};
struct BattlefieldFlag {
    u64 guid = 0;
    f32 x = 0.0f, y = 0.0f;
    // The flag it holds (0x4AA670: 0x8457D4[the player's side]): "HordeFlag" for an Alliance player (its
    // teammate carries the Horde flag), "AllianceFlag" for a Horde one; unknown before the player's race is.
    enum class Token : u8 { Unknown, AllianceFlag, HordeFlag } token = Token::Unknown;
};

// Phase 17 (scores): inside a battleground, as WoW.exe 1.12.1 keeps it (read-only disassembly
// 0x4A9C40..0x4AC430; the Lua API of WorldStateFrame.lua, Blizzard_BattlefieldMinimap and WorldMapFrame.lua
// in brackets):
//  - RequestBattlefieldScoreData (0x4AA170): MSG_PVP_LOG_DATA (empty) at most once every 5 s (the score frame
//    calls it every frame while shown). RequestBattlefieldPositions (0x4AA5C0):
//    MSG_BATTLEGROUND_PLAYER_POSITIONS at most once every 5 s while a queue is active (the world map calls it
//    every frame while shown); none active: no positions.
//  - MSG_PVP_LOG_DATA (0x4AAB30): "ended" and the winner at once (GetBattlefieldWinner:
//    game::Battlegrounds::winner, LeaveBattlefield's condition), the lines replace the last ones and every
//    player's name is asked for; once all are known (0x4AA580, 0x4AA200) each line takes its side from its
//    race, the lines are sorted (0x4AA350: with a faction filter its side first; then killing blows down,
//    deaths up, bonus honor down, name up) and UPDATE_BATTLEFIELD_SCORE fires. GetNumBattlefieldScores is how
//    many lines are of the filter's side (all without one). SetBattlefieldScoreFaction (0x4AA5A0: -1, 0 or 1)
//    sorts again.
//  - The columns (GetNumBattlefieldStats, GetBattlefieldStatInfo: WorldStateUI's type-2 rows of the
//    battleground) are taken when a queue turns "active" (0x4AA99D), which also resets the filter (0x4AAA57).
//  - MSG_BATTLEGROUND_PLAYER_POSITIONS (0x4AAD40) replaces the team's positions and the flag carrier.
//    GetNumBattlefieldPositions / GetBattlefieldPosition skip the player and its party and raid (0x4E7F70,
//    0x4BAEE0): the group's own dots show those.
//  - SMSG_BATTLEGROUND_PLAYER_JOINED / _LEFT (0x4AAE10): ERR_BG_PLAYER_JOINED_SS / ERR_BG_PLAYER_LEFT_S in
//    the chat (the error table's display type 0, CHAT_MSG_SYSTEM) once the name is known (game::NoticeFeed).
//  - Every world entry (0x4A9DB0) clears the score, the columns, the positions and both request timers.
// SMSG_BATTLEFIELD_WIN / _LOSE are consumed and ignored (no handler in WoW.exe 1.12.1). Main thread; fixed
// storage (nothing allocates per frame).
//
// Events (Gameplay::events()): BattlefieldScore (UPDATE_BATTLEFIELD_SCORE), BattlegroundPlayer (guid = the
// player, value[0] = 1 joined, 0 left).
class BattlefieldScore {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kRequestEveryMs = 5000; // 0x4AA1C5 / 0x4AA635: now + 0x1388

    BattlefieldScore(const EntityStore& entities, const Social& social, QueryCache& queries,
                     Battlegrounds& battlegrounds, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    void setData(const WorldStateUiData* data) { m_data = data; }
    // Every world entry (also a new map).
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // The packets above. False when malformed (nothing changes then).
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // Once a frame: the names the lines wait for, the columns of a queue that turned active.
    void update(u32 nowMs);

    // RequestBattlefieldScoreData / RequestBattlefieldPositions. False when not sent (too soon, nothing
    // active).
    bool requestScore(u32 nowMs);
    bool requestPositions(u32 nowMs);

    // GetNumBattlefieldScores and GetBattlefieldScore(index + 1) (sorted; the first count() are the filter's
    // side).
    u32 count() const { return m_shown; }
    u32 total() const { return m_ready ? m_count : 0; }
    const BattlefieldScoreLine& line(u32 index) const;
    // The line's name from the name cache (empty while unknown).
    std::string_view name(const BattlefieldScoreLine& line) const;
    // SetBattlefieldScoreFaction: -1 all, 0 Horde, 1 Alliance (anything else is ignored, as 0x4AA5A0 does).
    void setFaction(i32 faction, u32 nowMs);
    i32 faction() const { return m_faction; }
    // The last MSG_PVP_LOG_DATA said the battle ended (0xB6EBCC).
    bool ended() const { return m_ended; }
    // GetNumBattlefieldStats / GetBattlefieldStatInfo(index + 1): its text, icon and tooltip.
    u32 statCount() const { return m_statCount; }
    const WorldStateUiRow* stat(u32 index) const { return index < m_statCount ? m_stats[index] : nullptr; }

    // GetNumBattlefieldPositions and GetBattlefieldPosition(index + 1) (asks for the name while unknown).
    u32 positionCount() const;
    bool position(u32 index, u32 nowMs, BattlefieldPosition& out);
    // GetNumBattlefieldFlagPositions (0 or 1) and GetBattlefieldFlagPosition(1).
    bool flagCarrier(u32 nowMs, BattlefieldFlag& out) const;

    // Score packets applied (a view rebuilds when it moves); world entries so far (PLAYER_ENTERING_WORLD).
    u32 changes() const { return m_changes; }
    u32 worldEntries() const { return m_worldEntries; }

    // Development (VD_SCORE_DEMO, the gallery): as if the server had sent them (the names come from the name
    // cache: QueryCache::onName puts the samples' there).
    void demoScore(const net::PvpLogData& data, u32 nowMs);
    void demoPositions(const net::BattlegroundPositions& positions);
    void demoStats(u32 map);

private:
    bool namesKnown() const;
    void finish(u32 nowMs); // 0x4AA200: sides, sort, UPDATE_BATTLEFIELD_SCORE
    bool groupOrSelf(u64 guid) const;
    bool sendEmpty(net::Opcode opcode);
    void apply(const net::PvpLogData& data, u32 nowMs);

    const EntityStore& m_entities;
    const Social& m_social;
    QueryCache& m_queries;
    Battlegrounds& m_battlegrounds;
    Events& m_events;
    PacketSink m_sink;
    const WorldStateUiData* m_data = nullptr;
    bool m_inWorld = false;
    u64 m_player = 0;

    // The score.
    net::PvpLogData m_packet; // parse scratch
    std::array<BattlefieldScoreLine, net::kPvpLogMaxScores> m_lines{};
    std::array<u8, net::kPvpLogMaxScores> m_order{}; // sorted
    u32 m_count = 0;
    u32 m_shown = 0;        // 0xB6EBC4
    bool m_ready = false;   // the names are known and the lines sorted
    bool m_waiting = false; // 0xB6EBD4 != 0
    bool m_ended = false;
    i32 m_faction = -1;         // 0x8457D0
    u32 m_scoreAllowedAtMs = 0; // 0xB6EBC8 (0: now)
    bool m_scoreAsked = false;
    std::array<const WorldStateUiRow*, kMaxStatColumns> m_stats{};
    u32 m_statCount = 0;   // 0xB6EBD8
    i32 m_statsSlot = -1;  // the queue they were taken for
    u32 m_eventCursor = 0; // the BattlefieldStatus events

    // The positions.
    net::BattlegroundPositions m_positions; // 0xB6E710 / 0xB6EA30, the carrier 0xB6E6D8
    u32 m_positionsAllowedAtMs = 0;         // 0xB6EBE0 (0: now)
    bool m_positionsAsked = false;
    std::vector<u8> m_body;
    u32 m_changes = 0;
    u32 m_worldEntries = 0;
};

} // namespace vd::game
