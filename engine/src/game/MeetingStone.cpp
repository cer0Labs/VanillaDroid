#include "game/MeetingStone.h"

#include "game/Queries.h"
#include "game/Social.h"
#include "net/MeetingStoneMessages.h"
#include "net/UpdateFields.gen.h"

#include <iterator>

namespace vd::game {
namespace {

using net::Opcode;

struct LineDef {
    const char* key;
    bool error; // the error table's display type 2 (UIErrorsFrame) instead of 0 (the chat)
};
constexpr LineDef kLines[] = {
    {"ERR_MEETING_STONE_IN_QUEUE_S", false},
    {"ERR_MEETING_STONE_LEFT_QUEUE_S", false},
    {"ERR_MEETING_STONE_OTHER_MEMBER_LEFT", false},
    {"ERR_MEETING_STONE_PARTY_KICKED_FROM_QUEUE", false},
    {"ERR_MEETING_STONE_MEMBER_STILL_IN_QUEUE", false},
    {"ERR_MEETING_STONE_SUCCESS", false},
    {"ERR_MEETING_STONE_IN_PROGRESS", false},
    {"ERR_MEETING_STONE_MEMBER_ADDED_S", false},
    {"ERR_MEETING_STONE_GROUP_FULL", true},
    {"ERR_MEETING_STONE_NOT_LEADER", true},
    {"ERR_MEETING_STONE_INVALID_LEVEL", true},
    {"ERR_MEETING_STONE_MUST_BE_LEADER", true},
    {"ERR_MEETING_STONE_NO_RAID_GROUP", true},
};
static_assert(std::size(kLines) == static_cast<usize>(MeetingStoneLine::NoRaidGroup) + 1, "one per line");

constexpr u32 kPartySlots = 4; // WoW.exe's party slots (0xBC6F48, 0x4E86D0 counts them): four is a full group

} // namespace

const char* meetingStoneLineKey(MeetingStoneLine line) {
    const u32 i = static_cast<u32>(line);
    return i < std::size(kLines) ? kLines[i].key : "";
}

bool meetingStoneLineIsError(MeetingStoneLine line) {
    const u32 i = static_cast<u32>(line);
    return i < std::size(kLines) && kLines[i].error;
}

MeetingStone::MeetingStone(const EntityStore& entities, const Social& social, QueryCache& queries,
                           Events& events)
    : m_entities(entities), m_social(social), m_queries(queries), m_events(events) {
    m_body.reserve(16);
}

void MeetingStone::enterWorld(u64 player, u32 nowMs) {
    (void)nowMs;
    m_inWorld = true;
    m_player = player;
    m_known = false; // 0x4C9F40: the tooltip says UNKNOWN until the answer
    m_body.clear();
    sendBody(Opcode::CMSG_MEETINGSTONE_INFO);
}

void MeetingStone::leaveWorld() {
    m_inWorld = false;
    m_known = false;
}

bool MeetingStone::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool MeetingStone::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_MEETINGSTONE_SETQUEUE:
    case Opcode::SMSG_MEETINGSTONE_COMPLETE:
    case Opcode::SMSG_MEETINGSTONE_IN_PROGRESS:
    case Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED:
    case Opcode::SMSG_MEETINGSTONE_JOINFAILED: return true;
    default: return false;
    }
}

void MeetingStone::notice(MeetingStoneLine line, u32 areaId, u64 guid, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::MeetingStoneNotice;
    e.timeMs = nowMs;
    e.id = areaId;
    e.guid = guid;
    e.value[0] = static_cast<i32>(line);
}

bool MeetingStone::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    switch (opcode) {
    case Opcode::SMSG_MEETINGSTONE_SETQUEUE: {
        net::MeetingStoneQueue q;
        if (!net::parseMeetingStoneSetQueue(payload, q)) {
            return false;
        }
        const u32 before = m_areaId;
        m_areaId = q.areaId;
        m_known = true;
        switch (q.status) {
        case net::kMeetingStoneLeftQueue:
            if (before != 0) { // 0x4CA286: the name of the queue left (a valid AreaTable row)
                notice(MeetingStoneLine::LeftQueue, before, 0, nowMs);
            }
            break;
        case net::kMeetingStoneJoinedQueue:
            if (q.areaId != before) { // 0x4CA2C7: nothing when it only confirms the same queue (a new world)
                notice(MeetingStoneLine::InQueue, q.areaId, 0, nowMs);
                ++m_joins;
                GameEvent& e = m_events.push(); // "HARDCODED Meeting Stone Join" on the player
                e = GameEvent{};
                e.kind = GameEventKind::MeetingStoneJoined;
                e.timeMs = nowMs;
                e.guid = m_player;
            }
            break;
        case net::kMeetingStoneMemberLeft:
            notice(MeetingStoneLine::OtherMemberLeft, q.areaId, 0, nowMs);
            break;
        case net::kMeetingStonePartyRemoved: notice(MeetingStoneLine::PartyKicked, q.areaId, 0, nowMs); break;
        case net::kMeetingStoneLookingForNewParty:
            notice(MeetingStoneLine::StillInQueue, q.areaId, 0, nowMs);
            break;
        default: break; // 5 (none) and anything else: silently
        }
        GameEvent& e = m_events.push(); // MEETINGSTONE_CHANGED (0x207)
        e = GameEvent{};
        e.kind = GameEventKind::MeetingStoneChanged;
        e.timeMs = nowMs;
        e.id = m_areaId;
        e.value[0] = q.status;
        e.value[1] = static_cast<i32>(before);
        return true;
    }
    case Opcode::SMSG_MEETINGSTONE_COMPLETE:
        notice(MeetingStoneLine::Success, m_areaId, 0, nowMs);
        return true;
    case Opcode::SMSG_MEETINGSTONE_IN_PROGRESS:
        notice(MeetingStoneLine::InProgress, m_areaId, 0, nowMs);
        return true;
    case Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED: {
        u64 player = 0;
        if (!net::parseMeetingStoneMemberAdded(payload, player)) {
            return false;
        }
        m_queries.playerName(player); // 0x4CA478: the name asked for, the line when it is known
        notice(MeetingStoneLine::MemberAdded, m_areaId, player, nowMs);
        return true;
    }
    case Opcode::SMSG_MEETINGSTONE_JOINFAILED: {
        u8 reason = 0;
        if (!net::parseMeetingStoneJoinFailed(payload, reason)) {
            return false;
        }
        switch (reason) {
        case net::kMeetingStoneFailPartyLeader: notice(MeetingStoneLine::MustBeLeader, 0, 0, nowMs); break;
        case net::kMeetingStoneFailFullGroup: notice(MeetingStoneLine::GroupFull, 0, 0, nowMs); break;
        case net::kMeetingStoneFailRaidGroup: notice(MeetingStoneLine::NoRaidGroup, 0, 0, nowMs); break;
        default: break;
        }
        return true;
    }
    default: return false;
    }
}

u32 MeetingStone::partyMembers() const {
    const GroupState& group = m_social.group();
    if (!group.active) {
        return 0;
    }
    u32 count = 0;
    for (const GroupMember& m : group.members) {
        count += !group.raid || m.subgroup == group.ownSubgroup ? 1 : 0;
    }
    return count;
}

bool MeetingStone::use(u64 stone, u32 nowMs) {
    if (!m_inWorld || stone == 0) {
        return false;
    }
    const u32 party = partyMembers();
    if (party > 0 && !m_social.isLeader()) {
        notice(MeetingStoneLine::MustBeLeader, 0, 0, nowMs);
        return false;
    }
    if (party >= kPartySlots) {
        notice(MeetingStoneLine::GroupFull, 0, 0, nowMs);
        return false;
    }
    // The stone's levels (its template; unknown reads as 0..0, which only a level 0 would pass).
    const Entity* me = m_entities.find(m_player);
    const Entity* go = m_entities.find(stone);
    const u32 level = me ? me->value(net::uf::UNIT_FIELD_LEVEL) : 0;
    const GameObjectInfo* info =
        go ? m_queries.gameObject(go->value(net::uf::OBJECT_FIELD_ENTRY), stone) : nullptr;
    const bool stoneInfo = info && info->found && info->type == kGameObjectMeetingStone;
    const u32 minLevel = stoneInfo ? info->data[0] : 0;
    const u32 maxLevel = stoneInfo ? info->data[1] : 0;
    if (level < minLevel || level > maxLevel) {
        notice(MeetingStoneLine::InvalidLevel, 0, 0, nowMs);
        return false;
    }
    if (m_social.raidMemberCount() > 0) {
        notice(MeetingStoneLine::NoRaidGroup, 0, 0, nowMs);
        return false;
    }
    m_body.clear();
    net::writeMeetingStoneJoin(m_body, stone);
    return sendBody(Opcode::CMSG_MEETINGSTONE_JOIN);
}

bool MeetingStone::leave(u32 nowMs) {
    if (!m_inWorld) {
        return false;
    }
    if (partyMembers() > 0 && !m_social.isLeader()) {
        notice(MeetingStoneLine::NotLeader, m_areaId, 0, nowMs); // 0x4CA14E
        return false;
    }
    m_body.clear();
    return sendBody(Opcode::CMSG_MEETINGSTONE_LEAVE);
}

void MeetingStone::demoQueue(u32 areaId, u32 nowMs) {
    const u32 before = m_areaId;
    m_areaId = areaId;
    m_known = true;
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::MeetingStoneChanged;
    e.timeMs = nowMs;
    e.id = areaId;
    e.value[0] = areaId != 0 ? net::kMeetingStoneJoinedQueue : net::kMeetingStoneNone;
    e.value[1] = static_cast<i32>(before);
}

} // namespace vd::game
