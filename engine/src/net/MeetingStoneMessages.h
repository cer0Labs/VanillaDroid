#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// Phase 16 (alerts): the meeting stones' queue (the 1.12 "Looking for more" of a dungeon's stone), as
// vmangos-core 4641790 reads and writes it for build 5875: Server/Packets/Misc.{h,cpp} (MeetingStoneJoin,
// MeetingstoneSetQueue, MeetingstoneJoinFailed, MeetingstoneMemberAdded, MeetingstoneInProgress,
// MeetingstoneComplete), LFG/LFGHandler.cpp (HandleMeetingStoneJoinOpcode / Leave / Info), LFG/LFGQueue.cpp
// and Group/Group.cpp (who gets which answer, when). Same rules as net/GameMessages: parsers fill a
// caller-owned value and return false on a malformed packet, never reading past its end; builders append the
// body of a client packet.
//
// Bodies covered elsewhere: CMSG_MEETINGSTONE_LEAVE and CMSG_MEETINGSTONE_INFO (empty);
// SMSG_MEETINGSTONE_COMPLETE and SMSG_MEETINGSTONE_IN_PROGRESS (empty).
namespace vd::net {

// MeetingstoneQueueStatus (LFG/LFGDefines.h): the u8 after the area of SMSG_MEETINGSTONE_SETQUEUE. WoW.exe's
// handler (0x4CA230) prints a line for 0..4 and nothing for the others; vMaNGOS sends 5 with area 0 when
// there is no queue.
constexpr u8 kMeetingStoneLeftQueue = 0;          // ERR_MEETING_STONE_LEFT_QUEUE_S (the previous area)
constexpr u8 kMeetingStoneJoinedQueue = 1;        // ERR_MEETING_STONE_IN_QUEUE_S (only when the area changes)
constexpr u8 kMeetingStoneMemberLeft = 2;         // ERR_MEETING_STONE_OTHER_MEMBER_LEFT
constexpr u8 kMeetingStonePartyRemoved = 3;       // ERR_MEETING_STONE_PARTY_KICKED_FROM_QUEUE
constexpr u8 kMeetingStoneLookingForNewParty = 4; // ERR_MEETING_STONE_MEMBER_STILL_IN_QUEUE
constexpr u8 kMeetingStoneNone = 5;               // silent: no queue (MEETINGSTONE_STATUS_NONE)

// MeetingstoneFailedStatus: the u8 of SMSG_MEETINGSTONE_JOINFAILED (0x4CA3E3 shows 1..3, ignores the rest).
constexpr u8 kMeetingStoneFailPartyLeader = 1; // ERR_MEETING_STONE_MUST_BE_LEADER
constexpr u8 kMeetingStoneFailFullGroup = 2;   // ERR_MEETING_STONE_GROUP_FULL
constexpr u8 kMeetingStoneFailRaidGroup = 3;   // ERR_MEETING_STONE_NO_RAID_GROUP

// Client: the meeting stone used (GAMEOBJECT_TYPE_MEETINGSTONE, 23): its full GUID. The server checks the
// interaction distance and the group (leader, not a raid, not full) and queues for the template's areaID.
void writeMeetingStoneJoin(std::vector<u8>& out, u64 stone);

// Server SMSG_MEETINGSTONE_SETQUEUE: u32 AreaTable id (0: out of the queue) and u8 status (kMeetingStone*).
struct MeetingStoneQueue {
    u32 areaId = 0;
    u8 status = 0;
};
bool parseMeetingStoneSetQueue(std::span<const u8> payload, MeetingStoneQueue& out);

// Server SMSG_MEETINGSTONE_JOINFAILED: u8 reason (kMeetingStoneFail*).
bool parseMeetingStoneJoinFailed(std::span<const u8> payload, u8& reason);

// Server SMSG_MEETINGSTONE_MEMBER_ADDED: the full GUID of the player the stone added to the group.
bool parseMeetingStoneMemberAdded(std::span<const u8> payload, u64& player);

} // namespace vd::net
