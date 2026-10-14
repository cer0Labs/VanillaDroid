#include "game/QuestJournal.h"

#include "game/Queries.h"
#include "game/QuestData.h"

#include <algorithm>
#include <ctime>

namespace vd::game {

void QuestJournal::reset() {
    m_rowCount = 0;
    m_headerCount = 0;
    m_collapsedCount = 0;
    m_selected = 0;
    m_watchCount = 0;
}

void QuestJournal::refresh(const QuestLog& log, const QueryCache& queries, const QuestData* names) {
    // The log's quests whose template is known, without those the client never lists.
    struct Entry {
        u32 questId;
        i32 zone;
        u32 level;
        std::string_view title;
    };
    std::array<Entry, kQuestLogSize> entries{};
    u32 count = 0;
    std::array<i32, kQuestLogSize> zones{};
    u32 zoneCount = 0;
    for (const QuestLogEntry& slot : log.slots()) {
        const QuestInfo* info = slot.questId != 0 ? queries.findQuest(slot.questId) : nullptr;
        if (!info || (info->flags & kQuestFlagAutoRewarded) != 0) {
            continue;
        }
        entries[count++] = {slot.questId, info->zoneOrSort, info->level, info->title};
        if (std::find(zones.begin(), zones.begin() + zoneCount, info->zoneOrSort) ==
            zones.begin() + zoneCount) {
            zones[zoneCount++] = info->zoneOrSort;
        }
    }
    // WoW.exe's order: zones by name (zone 0 first; the client compares with a case-sensitive strcmp), quests
    // of a zone by level, then title.
    const auto name = [&](i32 zone) { return names ? names->headerName(zone) : std::string_view{}; };
    std::sort(zones.begin(), zones.begin() + zoneCount, [&](i32 a, i32 b) {
        if (a == 0 || b == 0) {
            return a == 0 && b != 0;
        }
        const int order = name(a).compare(name(b));
        return order != 0 ? order < 0 : a < b;
    });
    std::sort(entries.begin(), entries.begin() + count, [](const Entry& a, const Entry& b) {
        if (a.level != b.level) {
            return a.level < b.level;
        }
        const int order = a.title.compare(b.title);
        return order != 0 ? order < 0 : a.questId < b.questId;
    });
    m_rowCount = 0;
    m_headerCount = zoneCount;
    for (u32 z = 0; z < zoneCount; ++z) {
        const i32 zone = zones[z];
        const bool hidden = collapsed(zone);
        m_rows[m_rowCount++] = {0, zone, true, hidden};
        for (u32 i = 0; i < count && !hidden; ++i) {
            if (entries[i].zone == zone) {
                m_rows[m_rowCount++] = {entries[i].questId, zone, false, false};
            }
        }
    }
    // Collapsed headers of zones no longer in the log are forgotten.
    for (u32 c = 0; c < m_collapsedCount;) {
        const bool present =
            std::find(zones.begin(), zones.begin() + zoneCount, m_collapsed[c]) != zones.begin() + zoneCount;
        if (present) {
            ++c;
        } else {
            m_collapsed[c] = m_collapsed[--m_collapsedCount];
        }
    }
    // Watches of quests that left the log (AutoQuestWatch_CheckDeleted; the client forgets the C side's too).
    for (u32 w = 0; w < m_watchCount;) {
        if (log.find(m_watches[w].questId) == nullptr) {
            removeWatch(w);
        } else {
            ++w;
        }
    }
    // The selection stays on a quest listed; else the first one (QuestLog_SetFirstValidSelection).
    bool listed = false;
    u32 first = 0;
    for (u32 r = 0; r < m_rowCount; ++r) {
        if (!m_rows[r].header) {
            listed = listed || m_rows[r].questId == m_selected;
            first = first == 0 ? m_rows[r].questId : first;
        }
    }
    if (!listed) {
        m_selected = first;
    }
}

bool QuestJournal::allCollapsed() const {
    for (u32 r = 0; r < m_rowCount; ++r) {
        if (m_rows[r].header && !m_rows[r].collapsed) {
            return false;
        }
    }
    return true;
}

bool QuestJournal::collapsed(i32 zoneOrSort) const {
    for (u32 c = 0; c < m_collapsedCount; ++c) {
        if (m_collapsed[c] == zoneOrSort) {
            return true;
        }
    }
    return false;
}

void QuestJournal::expand(i32 zoneOrSort) {
    if (zoneOrSort == 0) {
        m_collapsedCount = 0;
        return;
    }
    for (u32 c = 0; c < m_collapsedCount; ++c) {
        if (m_collapsed[c] == zoneOrSort) {
            m_collapsed[c] = m_collapsed[--m_collapsedCount];
            return;
        }
    }
}

void QuestJournal::collapse(i32 zoneOrSort) {
    if (zoneOrSort == 0) {
        for (u32 r = 0; r < m_rowCount; ++r) {
            if (m_rows[r].header && !collapsed(m_rows[r].zoneOrSort) &&
                m_collapsedCount < m_collapsed.size()) {
                m_collapsed[m_collapsedCount++] = m_rows[r].zoneOrSort;
            }
        }
        return;
    }
    if (!collapsed(zoneOrSort) && m_collapsedCount < m_collapsed.size()) {
        m_collapsed[m_collapsedCount++] = zoneOrSort;
    }
}

// -------------------------------------------------------------------------------------------------------
// Watches.

i32 QuestJournal::watchIndex(u32 questId) const {
    for (u32 w = 0; w < m_watchCount; ++w) {
        if (m_watches[w].questId == questId) {
            return static_cast<i32>(w);
        }
    }
    return -1;
}

bool QuestJournal::watched(u32 questId) const {
    return questId != 0 && watchIndex(questId) >= 0;
}

void QuestJournal::removeWatch(u32 index) {
    // tremove keeps the order: the watch frame lists them as they were added.
    for (u32 w = index; w + 1 < m_watchCount; ++w) {
        m_watches[w] = m_watches[w + 1];
    }
    --m_watchCount;
}

QuestWatchToggle QuestJournal::toggleWatch(u32 questId, u32 objectives) {
    if (const i32 index = watchIndex(questId); index >= 0) {
        removeWatch(static_cast<u32>(index));
        return QuestWatchToggle::Removed;
    }
    if (objectives == 0) {
        return QuestWatchToggle::NoObjectives;
    }
    if (m_watchCount >= kMaxWatchedQuests) {
        return QuestWatchToggle::TooMany;
    }
    m_watches[m_watchCount++] = {questId, false, 0};
    return QuestWatchToggle::Added;
}

void QuestJournal::autoWatch(u32 questId, u32 nowMs) {
    if (!m_autoWatch || questId == 0) {
        return;
    }
    // AutoQuestWatch_Update: a quest watched for good stays so; one watched for a while starts its five
    // minutes again, moved to the end of the list.
    if (const i32 index = watchIndex(questId); index >= 0) {
        if (!m_watches[static_cast<u32>(index)].timed) {
            return;
        }
        removeWatch(static_cast<u32>(index));
        m_watches[m_watchCount++] = {questId, true, nowMs + kAutoWatchMs};
        return;
    }
    // AutoQuestWatch_Insert: a free place, else the timed watch closest to expiring gives its place; five
    // watched for good: nothing.
    if (m_watchCount < kMaxWatchedQuests) {
        m_watches[m_watchCount++] = {questId, true, nowMs + kAutoWatchMs};
        return;
    }
    i32 lowest = -1;
    for (u32 w = 0; w < m_watchCount; ++w) {
        if (m_watches[w].timed &&
            (lowest < 0 ||
             static_cast<i32>(m_watches[w].expiresMs - m_watches[static_cast<u32>(lowest)].expiresMs) <= 0)) {
            lowest = static_cast<i32>(w);
        }
    }
    if (lowest >= 0) {
        removeWatch(static_cast<u32>(lowest));
        m_watches[m_watchCount++] = {questId, true, nowMs + kAutoWatchMs};
    }
}

void QuestJournal::update(u32 nowMs) {
    for (u32 w = 0; w < m_watchCount;) {
        if (m_watches[w].timed && static_cast<i32>(nowMs - m_watches[w].expiresMs) >= 0) {
            removeWatch(w);
        } else {
            ++w;
        }
    }
}

// -------------------------------------------------------------------------------------------------------

bool questSharable(const QuestInfo& quest) {
    return (quest.flags & kQuestFlagSharable) != 0;
}

u32 questTimeLeft(const QuestLogEntry& entry, u32 serverNow) {
    // A failed timed quest keeps 1 in its timer (Player::FailQuest): long gone.
    return entry.timer > serverNow ? entry.timer - serverNow : 0;
}

u64 packedTimeToUnix(u32 packed) {
    std::tm t{};
    t.tm_min = static_cast<int>(packed & 63u);
    t.tm_hour = static_cast<int>((packed >> 6) & 31u);
    t.tm_mday = static_cast<int>((packed >> 14) & 63u) + 1;
    t.tm_mon = static_cast<int>((packed >> 20) & 15u);
    t.tm_year = static_cast<int>((packed >> 24) & 0xFFu) + 100;
    t.tm_isdst = -1;
    if (t.tm_min > 59 || t.tm_hour > 23 || t.tm_mday > 31 || t.tm_mon > 11) {
        return 0;
    }
    const std::time_t at = std::mktime(&t);
    return at > 0 ? static_cast<u64>(at) : 0;
}

} // namespace vd::game
