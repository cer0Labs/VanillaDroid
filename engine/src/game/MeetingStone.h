#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <vector>

namespace vd::game {

class QueryCache;
class Social;

// GAMEOBJECT_TYPE_MEETINGSTONE: its template data are the levels and the zone (vMaNGOS GameObjectDefines.h
// meetingstone: data0 minLevel, data1 maxLevel, data2 areaID; WoW.exe 0x621B00 properties 0x33, 0x34, 0x35).
constexpr u32 kGameObjectMeetingStone = 23;

// The client's own lines about meeting stones (MeetingStoneNotice events), worded by game::NoticeFeed.
// WoW.exe's error table (0x488410 entries 422..434) puts the first eight in the chat (CHAT_MSG_SYSTEM) and
// the others in the UIErrorsFrame (red).
enum class MeetingStoneLine : u8 {
    InQueue,         // ERR_MEETING_STONE_IN_QUEUE_S: queued for an area (id)
    LeftQueue,       // ERR_MEETING_STONE_LEFT_QUEUE_S: out of the queue of an area (id: the one before)
    OtherMemberLeft, // ERR_MEETING_STONE_OTHER_MEMBER_LEFT
    PartyKicked,     // ERR_MEETING_STONE_PARTY_KICKED_FROM_QUEUE
    StillInQueue,    // ERR_MEETING_STONE_MEMBER_STILL_IN_QUEUE
    Success,         // ERR_MEETING_STONE_SUCCESS: SMSG_MEETINGSTONE_COMPLETE
    InProgress, // ERR_MEETING_STONE_IN_PROGRESS: SMSG_MEETINGSTONE_IN_PROGRESS (every 5 minutes in the queue)
    MemberAdded,  // ERR_MEETING_STONE_MEMBER_ADDED_S: guid = the player the stone added
    GroupFull,    // ERR_MEETING_STONE_GROUP_FULL (UIErrorsFrame)
    NotLeader,    // ERR_MEETING_STONE_NOT_LEADER: a party member tried to leave the queue
    InvalidLevel, // ERR_MEETING_STONE_INVALID_LEVEL: the client's own check of the stone's levels
    MustBeLeader, // ERR_MEETING_STONE_MUST_BE_LEADER
    NoRaidGroup,  // ERR_MEETING_STONE_NO_RAID_GROUP
};
// Its GlobalStrings key, and whether it goes to the UIErrorsFrame (else the chat).
const char* meetingStoneLineKey(MeetingStoneLine line);
bool meetingStoneLineIsError(MeetingStoneLine line);

// Phase 16 (alerts): the meeting stones' queue as WoW.exe 1.12.1 keeps it (MeetingStoneInfo.cpp, read-only
// disassembly 0x4C9F40..0x4CA5B0) against vMaNGOS' LFG queue (LFG/LFGHandler.cpp, LFGQueue.cpp, Group.cpp):
//  - Entering the world (PLAYER_ENTERING_WORLD, 0x4908C0 -> 0x4C9F40) asks CMSG_MEETINGSTONE_INFO; the answer
//  is a
//    SMSG_MEETINGSTONE_SETQUEUE (the queue the player or its group is in, or none).
//  - Using a meeting stone (its type's OnRightClick, 0x5F69D0) is not CMSG_GAMEOBJ_USE (vMaNGOS ignores that
//  for
//    type 23): in a party but not its leader -> ERR_MEETING_STONE_MUST_BE_LEADER; 4 party members (a full
//    group) -> _GROUP_FULL; the player's level out of the stone's data0..data1 -> _INVALID_LEVEL; in a raid
//    -> _NO_RAID_GROUP; otherwise CMSG_MEETINGSTONE_JOIN with the stone's GUID.
//  - SMSG_MEETINGSTONE_SETQUEUE (0x4CA230): the area is kept (IsInMeetingStoneQueue: area != 0); status 0
//  says
//    _LEFT_QUEUE_S with the area before, 1 _IN_QUEUE_S with the new one (only when the area changes; the join
//    also plays "HARDCODED Meeting Stone Join" on the player and a tutorial), 2..4 their lines, others
//    nothing. Always MEETINGSTONE_CHANGED (the minimap's button) after.
//  - SMSG_MEETINGSTONE_COMPLETE, _IN_PROGRESS, _MEMBER_ADDED (the name, when known), _JOINFAILED (1..3):
//  their lines.
//  - CancelMeetingStoneRequest (0x4CA120, CONFIRM_LEAVE_QUEUE's OnAccept): a party member that is not the
//  leader gets
//    ERR_MEETING_STONE_NOT_LEADER; otherwise CMSG_MEETINGSTONE_LEAVE (the answer is a SETQUEUE status 0).
// Main thread; nothing allocates after construction.
//
// Events (Gameplay::events()): MeetingStoneChanged (id = the queue's area, 0 none; value[0] = status,
// value[1] = the area before), MeetingStoneNotice (value[0] = MeetingStoneLine, id = its area, guid = the
// player it names), MeetingStoneJoined (guid = the player: the join's visual).
class MeetingStone {
public:
    using Events = EventRing<GameEvent, 64>;

    MeetingStone(const EntityStore& entities, const Social& social, QueryCache& queries, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    // Every world entry (also a new map): CMSG_MEETINGSTONE_INFO.
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // The SMSG_MEETINGSTONE_* answers. False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // The player uses the meeting stone `stone` (a type-23 game object in reach): the client's checks, then
    // CMSG_MEETINGSTONE_JOIN. False when refused (a MeetingStoneNotice says why) or not sent.
    bool use(u64 stone, u32 nowMs);
    // CancelMeetingStoneRequest. False when refused (NotLeader) or not sent.
    bool leave(u32 nowMs);

    // IsInMeetingStoneQueue and the queue's AreaTable id (GetMeetingStoneStatusText: MEETINGSTONE_TOOLTIP
    // with its name, or UNKNOWN before the first answer of this world).
    bool inQueue() const { return m_areaId != 0; }
    u32 areaId() const { return m_areaId; }
    bool statusKnown() const { return m_known; }
    // How many times the player was queued at a stone (each plays "HARDCODED Meeting Stone Join": app/Units).
    u32 joins() const { return m_joins; }

    // Development (VD_MEETINGSTONE_DEMO, the gallery): as if the server had queued the player for `areaId`.
    void demoQueue(u32 areaId, u32 nowMs);

private:
    void notice(MeetingStoneLine line, u32 areaId, u64 guid, u32 nowMs);
    bool sendBody(net::Opcode opcode);
    // The player's party (its raid subgroup in a raid): the others in it, as WoW.exe's four party slots.
    u32 partyMembers() const;

    const EntityStore& m_entities;
    const Social& m_social;
    QueryCache& m_queries;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_areaId = 0;     // 0xB72038 (kept across worlds, as WoW.exe)
    bool m_known = false; // a SETQUEUE came in this world (the tooltip's text is built: 0xB7203C)
    u32 m_joins = 0;
};

} // namespace vd::game
