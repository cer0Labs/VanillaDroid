#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// Phase 17 (queue): the battlemaster's list and the queue's join, as vmangos-core 4641790 reads and writes
// them for build 5875: Server/Packets/Battleground.{h,cpp} (BattlemasterHello, BattlemasterJoin,
// BattlefieldJoin, BattlefieldListRequest, BattlefieldList, GroupJoinedBattleground),
// Battlegrounds/BattleGroundMgr.cpp (BuildBattleGroundListPacket, BuildGroupJoinedBattlegroundPacket) and
// Handlers/BattleGroundHandler.cpp (HandleBattlemasterHelloOpcode, RequestBgJoinQueue,
// HandleBattlefieldListOpcode). WoW.exe 1.12.1 writes and reads the same (read-only disassembly:
// SMSG_BATTLEFIELD_LIST 0x4AA6C0, JoinBattlefield 0x4A9F60, ShowBattlefieldList 0x4AB8C0,
// SMSG_GROUP_JOINED_BATTLEGROUND 0x4AACC0, the battlemaster's right click 0x5E01A0). Same rules as
// net/GameMessages: parsers fill a caller-owned value and return false on a malformed packet, never reading
// past its end; builders append the body of a client packet.
namespace vd::net {

// Client CMSG_BATTLEMASTER_HELLO: the battlemaster's GUID (a battlemaster without a gossip menu; one with
// UNIT_NPC_FLAG_GOSSIP gets CMSG_GOSSIP_HELLO and its GOSSIP_OPTION_BATTLEFIELD option leads to the same
// list).
void writeBattlemasterHello(std::vector<u8>& out, u64 battlemaster);

// Server SMSG_BATTLEFIELD_LIST: u64 the battlemaster's GUID (the PLAYER's own for a battleground's portal,
// its area trigger, ShowBattlefieldList and the GM's .go warsong), u32 the battleground's map, u8 the
// player's level bracket, u32 count and `count` u32 client instance ids ("Warsong Gulch 3": the battles of
// the bracket the player may ask for by number).
struct BattlefieldList {
    u64 battlemaster = 0;
    u32 mapId = 0;
    u8 bracketId = 0;
    std::vector<u32> instanceIds; // reused: keeps its capacity
};
bool parseBattlefieldList(std::span<const u8> payload, BattlefieldList& out);

// Client JoinBattlefield (0x4A9F60): CMSG_BATTLEMASTER_JOIN when the list came with a GUID: u64 that GUID,
// u32 the map, u32 the client instance id (0: "First Available"), u8 1 to join as a group. Without a GUID
// WoW.exe sends CMSG_BATTLEFIELD_JOIN with the same body minus the GUID (vMaNGOS reads only its map).
void writeBattlemasterJoin(std::vector<u8>& out, u64 battlemaster, u32 mapId, u32 instanceId, bool asGroup);
void writeBattlefieldJoin(std::vector<u8>& out, u32 mapId, u32 instanceId, bool asGroup);

// Client CMSG_BATTLEFIELD_LIST (ShowBattlefieldList, the minimap menu's CHANGE_INSTANCE): u32 the queued map.
// The answer is an SMSG_BATTLEFIELD_LIST with the player's GUID.
void writeBattlefieldListRequest(std::vector<u8>& out, u32 mapId);

// Server SMSG_GROUP_JOINED_BATTLEGROUND (BattleGroundGroupJoinStatus): i32 the queued map (a joined group's
// member), or one of these.
constexpr i32 kGroupJoinDeserters = -2; // BG_GROUPJOIN_DESERTERS: ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS
constexpr i32 kGroupJoinFailed = -1;    // BG_GROUPJOIN_FAILED: ERR_GROUP_JOIN_BATTLEGROUND_FAIL
bool parseGroupJoinedBattleground(std::span<const u8> payload, i32& result);

} // namespace vd::net
