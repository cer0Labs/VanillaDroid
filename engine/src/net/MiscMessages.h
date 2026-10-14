#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <vector>

// Phase 13 (13.9): server messages the client used to drop, as vmangos-core 4641790 writes them for build 5875
// (Server/Packets/Misc and the raw WorldPackets of GossipDef.cpp / Item.cpp). Same rules as net/GameMessages: the
// parsers return false on a malformed packet and never read past its end.
namespace vd::net {

// ServerMessageType (World.h): the text is ServerMessages.dbc's row of that id, "%s" replaced by `text`.
constexpr u32 kServerMessageShutdownTime = 1;
constexpr u32 kServerMessageRestartTime = 2;
constexpr u32 kServerMessageCustom = 3;
constexpr u32 kServerMessageShutdownCancelled = 4;
constexpr u32 kServerMessageRestartCancelled = 5;

// SMSG_SERVER_MESSAGE (World::SendServerMessage: the shutdown / restart countdown, `.announce`-like texts).
struct ServerMessage {
    u32 type = 0;
    std::string text; // the time left ("1 Hour(s) 5 Minute(s)": secsToTimeString) or the custom text
};
bool parseServerMessage(std::span<const u8> payload, ServerMessage& out);

// SMSG_MOUNTRESULT / SMSG_DISMOUNTRESULT (Player::SendMountResult / SendDismountResult): a u32 result.
// UnitMountResult (UnitDefines.h): 0 invalid mountee .. 9 forced dismount, 10 ok; UnitDismountResult: 0 no pet,
// 1 not mounted, 2 not your pet, 3 ok. The client words each failure with ERR_MOUNT_* / ERR_DISMOUNT_*.
constexpr u32 kMountResultOk = 10;
constexpr u32 kDismountResultOk = 3;
bool parseMountResult(std::span<const u8> payload, u32& result);

// SMSG_AI_REACTION (Creature::SendAIReaction, Unit::SendPetAIReaction): a full GUID and AiReaction
// (SharedDefines.h). The 1.12 client acts on two: alert (the creature noticed the player: its alert sound) and
// hostile (it attacks: its aggro sound).
constexpr u32 kAiReactionAlert = 0;
constexpr u32 kAiReactionHostile = 2;
struct AiReaction {
    u64 guid = 0;
    u32 reaction = 0;
};
bool parseAiReaction(std::span<const u8> payload, AiReaction& out);

// SMSG_GOSSIP_POI (PlayerMenu::SendPointOfInterest: a guard's directions): a point of the current map with an
// icon of Interface\Minimap\POIIcons and its name, which the client keeps as the minimap's one gossip point.
struct GossipPoi {
    u32 flags = 0;
    f32 x = 0.0f, y = 0.0f; // world coordinates
    u32 icon = 0;           // POIIcons cell
    u32 data = 0;
    std::string name; // up to 64 bytes in the client
};
bool parseGossipPoi(std::span<const u8> payload, GossipPoi& out);

// SMSG_SET_FORCED_REACTIONS (ReputationMgr::SendForceReactions: SPELL_AURA_FORCE_REACTION): every forced
// reaction the player has, as (Faction.dbc id, ReputationRank 0 hated .. 7 exalted) pairs; empty clears them.
struct ForcedReaction {
    u32 faction = 0;
    u32 rank = 0;
};
bool parseForcedReactions(std::span<const u8> payload, std::vector<ForcedReaction>& out);

// SMSG_SET_FACTION_ATWAR (Player::SendFactionAtWar: a reputation creature attacked the player, or that
// temporary war ended): reputation list id and FACTION_FLAG_AT_WAR (2) or 0.
struct FactionAtWar {
    u32 listId = 0;
    bool atWar = false;
};
bool parseFactionAtWar(std::span<const u8> payload, FactionAtWar& out);

// SMSG_ITEM_TIME_UPDATE (Item::SendTimeUpdate: an item that expires, every update of its duration): full item
// GUID and the seconds it has left.
struct ItemTimeUpdate {
    u64 item = 0;
    u32 seconds = 0;
};
bool parseItemTimeUpdate(std::span<const u8> payload, ItemTimeUpdate& out);

} // namespace vd::net
