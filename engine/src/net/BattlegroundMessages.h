#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// Phase 17 (lead): the battleground queues' status and the world states, as vmangos-core 4641790 writes and reads
// them for build 5875: Server/Packets/Battleground.{h,cpp} (BattlefieldStatus, BattlefieldStatusEmpty,
// BattleFieldPort, LeaveBattlefield), Battlegrounds/BattleGroundMgr.cpp (BuildBattleGroundStatusPacket: what time1
// and time2 mean), Handlers/BattleGroundHandler.cpp (HandleBattleFieldPortOpcode, HandleLeaveBattlefieldOpcode,
// HandleBattlefieldStatusOpcode), Server/Packets/Misc.cpp (UpdateWorldState) and Objects/Player.cpp
// (SendInitWorldStates). Same rules as net/GameMessages: parsers fill a caller-owned value and return false on a
// malformed packet, never reading past its end; builders append the body of a client packet.
//
// Bodies covered elsewhere: CMSG_BATTLEFIELD_STATUS (empty: the server answers one SMSG_BATTLEFIELD_STATUS per
// queue the player is in, nothing for the empty ones).
namespace vd::net {

// BattleGroundStatus (Battlegrounds/BattleGroundDefines.h): SMSG_BATTLEFIELD_STATUS's u32 status, and what its
// times mean (BattlefieldStatus in Server/Packets/Battleground.h).
constexpr u32 kBattlefieldStatusNone = 0;
constexpr u32 kBattlefieldStatusWaitQueue = 1;  // "queued": time1 = average wait (ms), time2 = time in the queue (ms)
constexpr u32 kBattlefieldStatusWaitJoin = 2;   // "confirm": time1 = until the invitation expires (ms); no time2
constexpr u32 kBattlefieldStatusInProgress = 3; // "active": time1 = until the instance closes (ms, 0 while it runs),
                                                // time2 = since it started (ms)
constexpr u32 kBattlefieldStatusWaitLeave = 4;  // the server never sends it (the end is "active" with a time1)

// PLAYER_MAX_BATTLEGROUND_QUEUES (vMaNGOS), MAX_BATTLEFIELD_QUEUES (BattlefieldFrame.lua): WoW.exe drops a status
// for a slot >= 3 (0x4AA86E).
constexpr u32 kBattlefieldQueueSlots = 3;

// Server SMSG_BATTLEFIELD_STATUS: u32 queue slot, u32 map (0: the slot is empty, a u32 0 and nothing else follows);
// then u8 bracket, u32 client instance id, u32 status, u32 time1, and u32 time2 only for kBattlefieldStatusWaitQueue
// and kBattlefieldStatusInProgress (BattlefieldStatus::AppendBodyTo). WoW.exe reads the same (0x4AA850).
struct BattlefieldStatus {
    u32 queueSlot = 0;
    u32 mapId = 0;      // Map.dbc id of the battleground; 0: the slot is empty
    u8 bracketId = 0;   // the level bracket (BattleGroundBracketId: (level - minLevel) / 10)
    u32 instanceId = 0; // the client instance id ("Warsong Gulch 3"; 0 before one is assigned)
    u32 status = 0;     // kBattlefieldStatus*
    u32 time1 = 0;
    u32 time2 = 0;
};
bool parseBattlefieldStatus(std::span<const u8> payload, BattlefieldStatus& out);

// Client CMSG_BATTLEFIELD_PORT (AcceptBattlefieldPort): u32 the queue's map, u8 action (1 enter the battle, 0 leave
// the queue). The server finds the queue by the map's battleground type.
void writeBattlefieldPort(std::vector<u8>& out, u32 mapId, bool enter);

// Client CMSG_LEAVE_BATTLEFIELD (LeaveBattlefield): u32 the map the player is in (the server ignores another one,
// and the request while in combat before the battle ended).
void writeLeaveBattlefield(std::vector<u8>& out, u32 mapId);

// One world state: a variable of the map or zone the interface shows (WorldStateUI.dbc's StateVariable).
struct WorldStatePair {
    u32 id = 0;
    u32 value = 0;
};

// Server SMSG_INIT_WORLD_STATES (Player::SendInitWorldStates, on every zone change): u32 map, u32 zone, u16 count,
// then `count` pairs of u32 state and u32 value. vMaNGOS counts and appends a (0, 0) terminator: it is dropped here.
struct InitWorldStates {
    u32 mapId = 0;
    u32 zoneId = 0;
    std::vector<WorldStatePair> states; // reused: keeps its capacity
};
bool parseInitWorldStates(std::span<const u8> payload, InitWorldStates& out);

// Server SMSG_UPDATE_WORLD_STATE: u32 state, u32 value.
bool parseUpdateWorldState(std::span<const u8> payload, WorldStatePair& out);

} // namespace vd::net
