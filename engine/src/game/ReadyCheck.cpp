#include "game/ReadyCheck.h"

#include "core/Profile.h"
#include "net/SocialMessages.h"

namespace vd::game {
namespace {

using net::Opcode;

// Time `now` has reached `at` (wrap-safe, as WoW.exe's signed differences).
bool reached(u32 now, u32 at) {
    return static_cast<i32>(now - at) >= 0;
}

} // namespace

ReadyCheck::ReadyCheck(const Social& social, Events& events) : m_social(social), m_events(events) {
    m_body.reserve(16);
}

void ReadyCheck::enterWorld(u64 player, bool newLogin, u32 nowMs) {
    (void)nowMs;
    m_inWorld = true;
    if (newLogin || player != m_player) {
        m_endMs = 0;
        m_pendingCount = 0;
        m_afkCount = 0;
        m_prompt = false;
    }
    m_player = player;
}

void ReadyCheck::leaveWorld() {
    m_inWorld = false;
    m_prompt = false; // the frame goes with the interface (a new map keeps the check's own timer)
    m_demo = false;
}

void ReadyCheck::notice(ReadyCheckLine line, u64 guid, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::ReadyCheckNotice;
    e.timeMs = nowMs;
    e.guid = guid;
    e.value[0] = static_cast<i32>(line);
    e.value[1] = static_cast<i32>(m_afkCount);
}

bool ReadyCheck::inProgress(u32 nowMs) const {
    return m_endMs != 0 && !reached(nowMs, m_endMs);
}

u32 ReadyCheck::promptRemainingMs(u32 nowMs) const {
    return m_prompt && !reached(nowMs, m_promptEndMs) ? m_promptEndMs - nowMs : 0;
}

bool ReadyCheck::waitingFor(u64 member) const {
    for (u32 i = 0; i < m_pendingCount; ++i) {
        if (m_pending[i] == member) {
            return true;
        }
    }
    return false;
}

void ReadyCheck::forget(u64 member) {
    for (u32 i = 0; i < m_pendingCount; ++i) {
        if (m_pending[i] == member) {
            m_pending[i] = m_pending[--m_pendingCount];
            return;
        }
    }
}

bool ReadyCheck::waitingForOnline() const {
    // 0x4BA491: a roster entry still flagged (+0x158) whose group list status says online (+0x18 bit 0).
    for (const GroupMember& m : m_social.group().members) {
        if ((m.status & net::kMemberOnline) != 0 && waitingFor(m.guid)) {
            return true;
        }
    }
    return false;
}

bool ReadyCheck::start(u32 nowMs) {
    if (!m_inWorld || !isLeader()) {
        return false; // 0x4BB1EB: silently, unless the player leads the group
    }
    if (inProgress(nowMs)) {
        notice(ReadyCheckLine::InProgress, 0, nowMs); // error 0x1C7
        return false;
    }
    // Every raid member but the player is waited for (a party has no raid roster: nobody).
    const u32 count = m_social.raidMembers(m_roster);
    m_pendingCount = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!m_roster[i].self && m_roster[i].guid != m_player && m_pendingCount < m_pending.size()) {
            m_pending[m_pendingCount++] = m_roster[i].guid;
        }
    }
    m_body.clear();
    const bool sent = m_sink(Opcode::MSG_RAID_READY_CHECK, m_body);
    m_endMs = nowMs + kDurationMs;
    if (m_endMs == 0) {
        m_endMs = 1;
    }
    notice(ReadyCheckLine::Start, 0, nowMs);
    return sent;
}

bool ReadyCheck::answer(bool ready, u32 nowMs) {
    m_prompt = false; // ReadyCheckFrame:Hide() after ConfirmReadyCheck, whatever it did
    m_demo = false;
    if (!m_inWorld || !inProgress(nowMs)) {
        return false;
    }
    m_body.clear();
    net::writeReadyCheckAnswer(m_body, ready);
    return m_sink(Opcode::MSG_RAID_READY_CHECK, m_body);
}

bool ReadyCheck::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode != Opcode::MSG_RAID_READY_CHECK) {
        return false;
    }
    net::ReadyCheckMessage msg;
    if (!net::parseReadyCheck(payload, msg)) {
        return false;
    }
    if (!isLeader()) {
        // A member is asked (whatever the body: WoW.exe reads none here). The line needs the leader's name.
        const u64 leader = m_social.group().leader;
        notice(ReadyCheckLine::LeaderStarted, leader, nowMs);
        m_endMs = nowMs + kDurationMs;
        if (m_endMs == 0) {
            m_endMs = 1;
        }
        showPrompt(leader, nowMs);
        return true;
    }
    if (!msg.answer && m_selfPrompt) {
        showPrompt(m_player, nowMs); // development: the leader's own request opens its frame too
    }
    // The leader: an answer (or its own request back, which names nobody). 0x4BA3F0: a member of the raid
    // roster (a party has none) is marked answered, and a "no" is said at once, however many times it comes.
    if (msg.answer && m_social.raidMemberCount() > 0 && m_social.subgroupOf(msg.member) >= 0) {
        forget(msg.member);
        if (msg.state == 0) {
            notice(ReadyCheckLine::NotReady, msg.member, nowMs);
        }
    }
    if (!waitingForOnline()) {
        m_endMs = nowMs - 1; // 0x4BA4CE: over now (the offline ones are the AFK list)
        if (m_endMs == 0) {
            m_endMs = 1;
        }
        update(nowMs);
    }
    return true;
}

void ReadyCheck::showPrompt(u64 leader, u32 nowMs) {
    // READY_CHECK -> ShowReadyCheck: the frame, its 30 s and the sound (ReadyCheckFrame.timer = 30).
    m_prompt = true;
    m_promptLeader = leader;
    m_promptEndMs = nowMs + kDurationMs;
    m_demo = false;
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::ReadyCheck;
    e.timeMs = nowMs;
    e.guid = leader;
}

void ReadyCheck::finish(u32 nowMs) {
    m_endMs = 0;
    m_afkCount = 0;
    // Roster order; a member that left the raid meanwhile is no longer in it, one that joined was never
    // waited for.
    const u32 count = m_social.raidMembers(m_roster);
    for (u32 i = 0; i < count; ++i) {
        if (waitingFor(m_roster[i].guid) && m_afkCount < m_afk.size()) {
            m_afk[m_afkCount++] = m_roster[i].guid;
        }
    }
    // The flags stay (WoW.exe clears them only at the next DoReadyCheck): an answer that comes late still
    // counts.
    notice(m_afkCount > 0 ? ReadyCheckLine::MembersAfk : ReadyCheckLine::NoAfk, 0, nowMs);
}

void ReadyCheck::update(u32 nowMs) {
    VD_PROFILE_ZONE("ReadyCheck::update");
    if (!m_inWorld) {
        return;
    }
    // CheckReadyCheckTime: only the leader's client ends a check (a member's timer just runs out).
    if (m_endMs != 0 && reached(nowMs, m_endMs) && isLeader()) {
        finish(nowMs);
    }
    // ReadyCheckFrame: RAID_ROSTER_UPDATE with no raid hides it unanswered; its 30 s run out with a line.
    if (m_prompt) {
        if (!m_demo && m_social.raidMemberCount() == 0) {
            m_prompt = false;
        } else if (reached(nowMs, m_promptEndMs)) {
            m_prompt = false;
            m_demo = false;
            notice(ReadyCheckLine::YouWereAfk, 0, nowMs);
        }
    }
}

void ReadyCheck::demoPrompt(u64 leader, u32 nowMs) {
    m_prompt = true;
    m_promptLeader = leader;
    m_promptEndMs = nowMs + kDurationMs;
    m_demo = true;
}

} // namespace vd::game
