// Phase 16 (alerts): the client's own lines of the ready check and of the meeting stones (game::NoticeFeed's
// part), worded as WoW.exe words them (game/ReadyCheck.h, game/MeetingStone.h).
#include "core/Text.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/MeetingStone.h"
#include "game/Notices.h"
#include "game/ReadyCheck.h"

#include <cstring>

namespace vd::game {
namespace {

using K = GameEventKind;

// The player a line names: its GUID's name from the name cache (the group list's names are there too).
const std::string* nameOf(const Gameplay& play, u64 guid) {
    return guid != 0 ? play.queries().findPlayerName(guid) : nullptr;
}

bool namesSomeone(const GameEvent& e) {
    if (e.kind == K::ReadyCheckNotice) {
        const auto line = static_cast<ReadyCheckLine>(e.value[0]);
        return line == ReadyCheckLine::LeaderStarted || line == ReadyCheckLine::NotReady;
    }
    return static_cast<MeetingStoneLine>(e.value[0]) == MeetingStoneLine::MemberAdded;
}

} // namespace

bool NoticeFeed::raidAlertReady(Gameplay& play, const GameEvent& e) const {
    // A name not known yet is asked for (the line waits for it, kNameWaitMs at most).
    return !namesSomeone(e) || e.guid == 0 || play.queries().playerName(e.guid) != nullptr;
}

void NoticeFeed::raidAlertLine(Gameplay& play, const GameEvent& e) {
    if (e.kind == K::ReadyCheckNotice) {
        const std::string* name = nameOf(play, e.guid);
        switch (static_cast<ReadyCheckLine>(e.value[0])) {
        case ReadyCheckLine::Start: text::format(push(NoticeKind::System), "READY_CHECK_START"); break;
        case ReadyCheckLine::InProgress:
            text::format(push(NoticeKind::Error), "ERR_READY_CHECK_IN_PROGRESS");
            break;
        case ReadyCheckLine::LeaderStarted: // WoW.exe writes nothing when the name is not in its cache
            if (name) {
                text::format(push(NoticeKind::System), "ERR_RAID_LEADER_READY_CHECK_START_S",
                             std::string_view(*name));
            }
            break;
        case ReadyCheckLine::NotReady:
            if (name) {
                text::format(push(NoticeKind::System), "RAID_MEMBER_NOT_READY", std::string_view(*name));
            }
            break;
        case ReadyCheckLine::MembersAfk: {
            // ", " between the names it knows, in a 512-byte buffer (0x4BB3A1).
            char list[512];
            usize used = 0;
            for (const u64 member : play.readyCheck().afk()) {
                const std::string* who = nameOf(play, member);
                if (!who || who->empty()) {
                    continue;
                }
                const usize sep = used > 0 ? 2 : 0;
                if (used + sep + who->size() >= sizeof(list)) {
                    break;
                }
                if (sep) {
                    std::memcpy(list + used, ", ", 2);
                }
                std::memcpy(list + used + sep, who->data(), who->size());
                used += sep + who->size();
            }
            if (used == 0) { // nobody it could name: the list is empty, the line still says it
                text::format(push(NoticeKind::System), "READY_CHECK_NO_AFK");
            } else {
                text::format(push(NoticeKind::System), "RAID_MEMBERS_AFK", std::string_view(list, used));
            }
            break;
        }
        case ReadyCheckLine::NoAfk: text::format(push(NoticeKind::System), "READY_CHECK_NO_AFK"); break;
        case ReadyCheckLine::YouWereAfk:
            text::format(push(NoticeKind::System), "READY_CHECK_YOU_WERE_AFK");
            break;
        }
        return;
    }
    const auto line = static_cast<MeetingStoneLine>(e.value[0]);
    const NoticeKind kind = meetingStoneLineIsError(line) ? NoticeKind::Error : NoticeKind::System;
    const text::Key key = meetingStoneLineKey(line);
    switch (line) {
    case MeetingStoneLine::InQueue:
    case MeetingStoneLine::LeftQueue: {
        // AreaTable's name of the queue's area, UNKNOWN when it has none (0x4CA2F4).
        const std::string_view area = m_data && e.id != 0 ? m_data->areaName(e.id) : std::string_view{};
        text::format(push(kind), key, area.empty() ? text::get("UNKNOWN") : area);
        break;
    }
    case MeetingStoneLine::MemberAdded:
        if (const std::string* name = nameOf(play, e.guid)) {
            text::format(push(kind), key, std::string_view(*name));
        }
        break;
    default: text::format(push(kind), key); break;
    }
}

} // namespace vd::game
