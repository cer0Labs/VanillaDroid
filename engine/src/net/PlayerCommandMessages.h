#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// The packets behind the player's menus and the slash commands of phase 13 (UnitPopup.lua, ChatFrame.lua's
// SlashCmdList) that no other area writes or reads, as vmangos-core 4641790 handles them for build 5875:
// raid target icons and ready checks (Server/Packets/Group.{h,cpp}, Handlers/GroupHandler.cpp
// HandleRaidTargetUpdateOpcode / HandleRaidReadyCheckOpcode / HandleGroupAssistantLeaderOpcode, Group/Group.cpp
// SetTargetIcon / SendTargetIconList), the time played (Packets/Misc.cpp PlayedTime), inspecting a player
// (Misc.cpp Inspect / InspectResponse), renaming a hunter's pet (Packets/Pet.cpp PetRename) and the chat
// channels' moderation (Packets/Channel.cpp: ChannelPassword, ChannelSetOwner, ChannelModerator...). Same rules
// as net/GameMessages: parsers fill a caller-owned struct and return false on a malformed packet, never reading
// past its end; builders append the body of a client packet (the opcode travels separately).
//
// Bodies covered elsewhere: CMSG_INSPECT, CMSG_GROUP_SET_LEADER (writeGuid); CMSG_GUILD_LEADER, CMSG_CHANNEL_OWNER,
// CMSG_CHANNEL_ANNOUNCEMENTS, CMSG_CHANNEL_MODERATE (one C string: writeCString); CMSG_PLAYED_TIME,
// CMSG_TOGGLE_PVP (the 1.12 client's TogglePVP sends no state byte), CMSG_GUILD_DISBAND and the leader's
// MSG_RAID_READY_CHECK request (empty); SMSG_INSPECT (one full GUID, read and ignored by the 1.12 client:
// parseGuid); SMSG_PET_NAME_INVALID (empty: the client says ERR_INVALID_PETNAME).
namespace vd::net {

// --- Raid target icons (MSG_RAID_TARGET_UPDATE) ---------------------------------------------------------------

// Group.h RaidTargetIcon: 0 star .. 7 skull on the wire; the interface counts them 1..8 (RAID_TARGET_1..8).
constexpr u32 kRaidTargetIcons = 8;
// The client's request for the whole list (RaidTargetUpdate::ReadFromWorldPacket: no GUID follows it).
constexpr u8 kRaidTargetRequestList = 0xFF;

// Client: an icon (0..7) on a unit, or the icon taken off when `guid` is 0 (Group::SetTargetIcon clears its slot
// and moves the icon off any other unit it was on).
void writeRaidTargetSet(std::vector<u8>& out, u8 icon, u64 guid);
// Client: the group's whole list, answered with a `full` update to this player only.
void writeRaidTargetRequest(std::vector<u8>& out);

// Server: u8 mode, then (mode 0) one icon and its unit — RaidTargetUpdateDelta, broadcast to the group when an
// icon moves — or (mode 1) the marked icons only, each an icon and its unit, to the end of the packet —
// RaidTargetUpdateAll, the answer to a request and what Group::SendUpdate resends after every SMSG_GROUP_LIST of a
// party (not a raid: the client clears its icons on those).
struct RaidTargetUpdate {
    struct Entry {
        u8 icon = 0; // 0..7
        u64 guid = 0; // 0: the icon is off
    };
    bool full = false;
    u32 count = 0; // entries used
    std::array<Entry, kRaidTargetIcons> entries{};
};
bool parseRaidTargetUpdate(std::span<const u8> payload, RaidTargetUpdate& out);

// --- Raid assistants (CMSG_GROUP_ASSISTANT_LEADER) -----------------------------------------------------------

// PromoteToAssistant / DemoteAssistant: the member's full GUID (builds after 1.11.2) and 1 to promote, 0 to demote.
// Only the raid's leader is obeyed (HandleGroupAssistantLeaderOpcode).
void writeGroupAssistant(std::vector<u8>& out, u64 member, bool assistant);

// --- Ready check (MSG_RAID_READY_CHECK) ----------------------------------------------------------------------

// A member's answer to the leader's check (1 ready, 0 not): one byte. The leader's request has no body.
void writeReadyCheckAnswer(std::vector<u8>& out, bool ready);

// Phase 16 (alerts). Server: empty — the request (RaidReadyCheckFromServer_Request), broadcast to the whole group,
// the leader included — or a member's answer forwarded to the leader alone (RaidReadyCheckFromServer_Response: its
// full GUID and the u8 it sent). Any other length is malformed.
struct ReadyCheckMessage {
    bool answer = false; // false: the request
    u64 member = 0;
    u8 state = 0; // 1 ready, 0 not (ConfirmReadyCheck() without an argument)
};
bool parseReadyCheck(std::span<const u8> payload, ReadyCheckMessage& out);

// --- Time played (SMSG_PLAYED_TIME) --------------------------------------------------------------------------

struct PlayedTime { // the answer to CMSG_PLAYED_TIME: seconds in total and at this level
    u32 total = 0;
    u32 level = 0;
};
bool parsePlayedTime(std::span<const u8> payload, PlayedTime& out);

// --- Pets (CMSG_PET_RENAME) ----------------------------------------------------------------------------------

// The hunter's pet and its new name (HandlePetRename: refused with SMSG_PET_NAME_INVALID unless the pet carries
// UNIT_FLAG_PET_RENAME and the name passes ObjectMgr::CheckPetName).
void writePetRename(std::vector<u8>& out, u64 pet, std::string_view name);

// --- Chat channels' moderation --------------------------------------------------------------------------------

// The channel and a player's name: CMSG_CHANNEL_SET_OWNER, _MODERATOR, _UNMODERATOR, _MUTE, _UNMUTE, _INVITE,
// _KICK, _BAN, _UNBAN; the channel and the new password: CMSG_CHANNEL_PASSWORD. Two C strings.
void writeChannelCommand(std::vector<u8>& out, std::string_view channel, std::string_view argument);

} // namespace vd::net
