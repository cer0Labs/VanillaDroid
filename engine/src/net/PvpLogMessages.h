#pragma once

#include "core/Types.h"

#include <array>
#include <span>

// Phase 17 (scores): what the server tells a player inside a battleground, as vmangos-core 4641790 writes it
// for build 5875: Server/Packets/Battleground.{h,cpp} (PvpLogData, BattlegroundPlayerJoined / _Left),
// Battlegrounds/BattleGroundMgr.cpp (BuildPvpLogDataPacket: the battleground's extra fields), and
// Handlers/BattleGroundHandler.cpp (HandlePVPLogDataOpcode, HandleBattleGroundPlayerPositionsOpcode). WoW.exe
// 1.12.1 reads them in its battlefield module (registered at 0x4A9C40): MSG_PVP_LOG_DATA 0x4AAB30,
// MSG_BATTLEGROUND_PLAYER_POSITIONS 0x4AAD40, SMSG_BATTLEGROUND_PLAYER_JOINED / _LEFT 0x4AAE10. Same rules as
// net/GameMessages: parsers fill a caller-owned value (fixed storage: nothing allocates) and return false on
// a malformed packet, never reading past its end.
//
// Bodies covered elsewhere: the client's MSG_PVP_LOG_DATA (RequestBattlefieldScoreData) and
// MSG_BATTLEGROUND_PLAYER_POSITIONS (RequestBattlefieldPositions) are empty. SMSG_BATTLEFIELD_WIN / _LOSE
// (empty, vMaNGOS sends one to each player when a battle ends) have no handler in WoW.exe 1.12.1 ("handler
// removed in 1.9"): the victory's sound is the SMSG_PLAY_SOUND that comes with them.
namespace vd::net {

// WoW.exe keeps 80 scores (0xB6E880: 80 entries; vMaNGOS "Client has a hard limit to 80") and 8 extra fields
// of each (0x4AAC43 stores the ones at offsets 0x20..0x3C and reads past the others).
constexpr u32 kPvpLogMaxScores = 80;
constexpr u32 kPvpLogExtraFields = 8;
// BattleGroundWinner (vMaNGOS BattleGroundDefines.h): the u8 after "ended". 2 (WINNER_NONE) when a battle
// ends without a side reaching the minimum (BattleGround::Update's premature end).
constexpr u8 kPvpLogWinnerHorde = 0;
constexpr u8 kPvpLogWinnerAlliance = 1;
constexpr u8 kPvpLogWinnerNone = 2;

// One player's line: guid, honor rank (HonorMgr's 0..18, PLAYER_BYTES_3's; 4 when the server does not find
// the player), killing blows, honorable kills, deaths, bonus honor, then the battleground's own fields:
// Warsong Gulch flag captures, flag returns; Arathi Basin bases assaulted, defended; Alterac Valley
// graveyards assaulted, defended, towers assaulted, defended, secondary objectives, lieutenants, secondary
// NPCs. WoW.exe reads the extra fields as int32.
struct PvpLogScore {
    u64 guid = 0;
    u32 rank = 0;
    u32 killingBlows = 0;
    u32 honorableKills = 0;
    u32 deaths = 0;
    u32 bonusHonor = 0;
    u32 extraCount = 0; // as sent (only the first kPvpLogExtraFields are kept)
    std::array<i32, kPvpLogExtraFields> extra{};
};

// Server MSG_PVP_LOG_DATA: u8 ended (1 when the battle ended), u8 winner only when ended, u32 count, then per
// player: u64 guid, u32 rank, u32 killing blows, u32 honorable kills, u32 deaths, u32 bonus honor, u32 extra
// count, extra count x u32 (PvpLogData::AppendBodyTo). The server sends it on request (always the final one,
// the same packet, once the battle ended) and to everyone when the battle ends. More than kPvpLogMaxScores
// players is malformed here (WoW.exe would write past its table).
struct PvpLogData {
    bool ended = false;
    u8 winner = 0; // kPvpLogWinner* (only when ended)
    u32 count = 0;
    std::array<PvpLogScore, kPvpLogMaxScores> scores{};
};
bool parsePvpLogData(std::span<const u8> payload, PvpLogData& out);

// Server MSG_BATTLEGROUND_PLAYER_POSITIONS (HandleBattleGroundPlayerPositionsOpcode, only inside a
// battleground): u32 count, count x (u64 guid, f32 x, f32 y) — the player's team when the player is NOT in
// its team's battleground raid (else none: the raid's own positions show them) — then u8 flag carriers (0 or
// 1: Warsong Gulch's carrier of the enemy flag of the player's team, i.e. the player's teammate holding the
// other side's flag) and, when 1, u64 guid, f32 x, f32 y. WoW.exe reads one carrier at most (0x4AADA5) and
// keeps no more than fits its table; more than kBattlegroundMaxPositions players is malformed here.
constexpr u32 kBattlegroundMaxPositions = 80;
struct BattlegroundPosition {
    u64 guid = 0;
    f32 x = 0.0f, y = 0.0f; // world coordinates
};
struct BattlegroundPositions {
    u32 count = 0;
    std::array<BattlegroundPosition, kBattlegroundMaxPositions> players{};
    bool hasCarrier = false;
    BattlegroundPosition carrier;
};
bool parseBattlegroundPositions(std::span<const u8> payload, BattlegroundPositions& out);

// Server SMSG_BATTLEGROUND_PLAYER_JOINED / _LEFT (BattleGround::AddPlayer / RemovePlayer, to the player's
// team): the player's full GUID.
bool parseBattlegroundPlayer(std::span<const u8> payload, u64& player);

} // namespace vd::net
