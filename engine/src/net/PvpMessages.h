#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>
#include <vector>

// Phase 17 (honor): the battleground graveyards' area spirit healer, the insignia, honor and "under attack",
// as vmangos-core 4641790 writes and reads them for build 5875: Server/Packets/Npc.cpp
// (AreaSpiritHealerTime), Server/Packets/Battleground.cpp (AreaSpiritHealerQuery / Queue),
// Server/Packets/Misc.cpp (PvpCredit, ZoneUnderAttack, PlayerSkinned) and Maps/Map.cpp
// (Map::SendDefenseMessage, written by hand). Same rules as net/GameMessages: parsers fill a caller-owned
// value and return false on a malformed packet, never reading past its end; builders append the body of a
// client packet.
namespace vd::net {

// Server SMSG_AREA_SPIRIT_HEALER_TIME (Creature::SendAreaSpiritHealerQueryOpcode): u64 the spirit guide, u32
// the ms its Spirit Heal Channel (22011) has left: the next resurrection wave (0 while it does not channel).
// Sent for CMSG_AREA_SPIRIT_HEALER_QUERY in a battleground and for CMSG_GOSSIP_HELLO on a spirit guide.
struct AreaSpiritHealerTime {
    u64 healer = 0;
    u32 timeMs = 0;
};
bool parseAreaSpiritHealerTime(std::span<const u8> payload, AreaSpiritHealerTime& out);

// Client CMSG_AREA_SPIRIT_HEALER_QUERY (the client found a spirit guide) and CMSG_AREA_SPIRIT_HEALER_QUEUE
// (AcceptAreaSpiritHeal): u64 the spirit guide. The server ignores both outside a battleground; the queue
// runs the guide's gossip script (npc_spirit_guide: Waiting to Resurrect, 2584, on the player).
void writeAreaSpiritHealerQuery(std::vector<u8>& out, u64 healer);
void writeAreaSpiritHealerQueue(std::vector<u8>& out, u64 healer);

// Server SMSG_PLAYER_SKINNED (Player::RemovedInsignia): u8 free repop (vMaNGOS always 0). An enemy took the
// player's insignia: its corpse became bones (it can no longer be reclaimed) and the server released the
// spirit.
struct PlayerSkinned {
    bool freeRepop = false;
};
bool parsePlayerSkinned(std::span<const u8> payload, PlayerSkinned& out);

// Server SMSG_PVP_CREDIT (HonorMgr::SendPVPCredit): i32 honor (negative for a dishonorable kill), u64 the
// victim (0: honor awarded without one, a battleground's bonus), i32 the victim's rank (a player's visual
// rank, 5 "Scout" at least; 19 a racial leader; 0 otherwise).
struct PvpCredit {
    i32 honor = 0;
    u64 victim = 0;
    i32 rank = 0;
};
bool parsePvpCredit(std::span<const u8> payload, PvpCredit& out);

// Server SMSG_ZONE_UNDER_ATTACK (Creature::SendZoneUnderAttackMessage): u32 the area (AreaTable) where a
// player killed a guard or a PvP-enabling creature; to every player of the other team on that map, once per
// area every 10 s.
struct ZoneUnderAttack {
    u32 areaId = 0;
};
bool parseZoneUnderAttack(std::span<const u8> payload, ZoneUnderAttack& out);

// Server SMSG_DEFENSE_MESSAGE (Map::SendDefenseMessage: the Eastern Plaguelands' towers): u32 the zone, u32
// the text's length with its NUL, the text (the broadcast text in the player's language, colour codes
// included).
struct DefenseMessage {
    u32 zoneId = 0;
    std::string_view text; // into the payload
};
bool parseDefenseMessage(std::span<const u8> payload, DefenseMessage& out);

} // namespace vd::net
