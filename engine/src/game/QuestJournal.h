#pragma once

#include "core/Types.h"
#include "game/Quests.h"

#include <array>
#include <span>

namespace vd::game {

class QueryCache;
class QuestData;

constexpr u32 kMaxWatchedQuests = 5;     // MAX_WATCHABLE_QUESTS (QuestLogFrame.lua)
constexpr u32 kAutoWatchMs = 300 * 1000; // MAX_QUEST_WATCH_TIMER: an automatic watch lasts five minutes
constexpr u32 kQuestFlagSharable = 0x08; // QUEST_FLAGS_SHARABLE (vMaNGOS QuestDef.h): GetQuestLogPushable
constexpr u32 kQuestFlagAutoRewarded =
    0x400; // QUEST_FLAGS_AUTO_REWARDED: "never appear in quest log client side"

// A line of the quest log's list as GetQuestLogTitle gives it: a zone (or sort) header, or a quest under it.
struct QuestLogRow {
    u32 questId = 0;    // 0: a header
    i32 zoneOrSort = 0; // the header's (and its quests'): > 0 AreaTable, < 0 -QuestSort
    bool header = false;
    bool collapsed = false; // a header whose quests are hidden (CollapseQuestHeader)
};

// What a Shift + click on a quest title did to its watch (QuestLogTitleButton_OnClick).
enum class QuestWatchToggle : u8 {
    Added,
    Removed,
    NoObjectives, // QUEST_WATCH_NO_OBJECTIVES
    TooMany,      // QUEST_WATCH_TOO_MANY (MAX_WATCHABLE_QUESTS)
};

// A watched quest (QUEST_WATCH_LIST): for good, or for a while after one of its objectives progressed.
struct QuestWatch {
    u32 questId = 0;
    bool timed = false; // AUTO_QUEST_WATCH: it goes away at expiresMs
    u32 expiresMs = 0;
};

// Phase 13 (quests): what the 1.12 client keeps of the quest log on its own side, behind QuestLogFrame.lua
// and its C functions (GetQuestLogTitle, Expand/CollapseQuestHeader, SelectQuestLogEntry,
// Add/RemoveQuestWatch, GetQuestTimers): the zone headers and which are collapsed, the selected quest, the
// watched quests and the timed quests' countdowns. The server knows none of it. Fixed storage: nothing
// allocates.
//
// Headers: one per zone or sort of the quests' templates (zoneOrSort: AreaTable when positive, QuestSort when
// negative), sorted by their names, each with its quests by level and then title (WoW.exe 1.12.1 builds the
// list so: a qsort of the zones by name, 0x4DE8F0, then of the lines by zone, header first, level and title,
// 0x4DEAC0). Quests whose template has not come yet are left out until it does, as the client does. Watches
// follow QuestLogFrame.lua: a Shift + click toggles one for good (at most five); with AUTO_QUEST_WATCH an
// objective that progresses watches its quest for five minutes (AutoQuestWatch_Update / _Insert / _OnUpdate),
// replacing the timed watch closest to expiring when five are watched; a quest that leaves the log stops
// being watched (AutoQuestWatch_CheckDeleted).
class QuestJournal {
public:
    static constexpr u32 kMaxRows = kQuestLogSize * 2; // every quest under its own header

    void reset(); // a new character

    // The list again from the log and the templates known (call once per frame or when either changed);
    // `names` gives the headers' names their order (nullptr: by zone number). Also forgets the watches of
    // quests no longer in the log and fixes the selection.
    void refresh(const QuestLog& log, const QueryCache& queries, const QuestData* names = nullptr);
    std::span<const QuestLogRow> rows() const { return {m_rows.data(), m_rowCount}; }
    u32 headerCount() const { return m_headerCount; }
    // Every header collapsed (QuestLog_Update: the "All" button shows the plus).
    bool allCollapsed() const;

    // ExpandQuestHeader / CollapseQuestHeader of one header; with 0, every header (the "All" button).
    void expand(i32 zoneOrSort);
    void collapse(i32 zoneOrSort);
    bool collapsed(i32 zoneOrSort) const;

    // The quest shown in the detail (SelectQuestLogEntry); 0 = none. A quest no longer listed (abandoned, or
    // under a collapsed header) gives way to the first one listed at the next refresh().
    u32 selected() const { return m_selected; }
    void select(u32 questId) { m_selected = questId; }

    // --- Watches ---
    // `objectives`: how many objectives the quest has (GetNumQuestLeaderBoards).
    QuestWatchToggle toggleWatch(u32 questId, u32 objectives);
    bool watched(u32 questId) const;
    std::span<const QuestWatch> watches() const { return {m_watches.data(), m_watchCount}; }
    // QUEST_WATCH_UPDATE (an objective progressed: SMSG_QUESTUPDATE_ADD_KILL / ADD_ITEM) with
    // AUTO_QUEST_WATCH on.
    void autoWatch(u32 questId, u32 nowMs);
    // AutoQuestWatch_OnUpdate: timed watches run out.
    void update(u32 nowMs);
    void setAutoWatch(bool on) { m_autoWatch = on; }
    bool autoWatchEnabled() const { return m_autoWatch; }

private:
    void removeWatch(u32 index);
    i32 watchIndex(u32 questId) const;

    std::array<QuestLogRow, kMaxRows> m_rows{};
    u32 m_rowCount = 0;
    u32 m_headerCount = 0;
    std::array<i32, kQuestLogSize> m_collapsed{}; // zones whose header is collapsed
    u32 m_collapsedCount = 0;
    u32 m_selected = 0;
    std::array<QuestWatch, kMaxWatchedQuests> m_watches{};
    u32 m_watchCount = 0;
    bool m_autoWatch = true; // the AUTO_QUEST_WATCH option ("1" by default)
};

// A quest of the log the player may share with the party: its template says so (GetQuestLogPushable).
bool questSharable(const QuestInfo& quest);

// Seconds left of a timed quest (QuestLogEntry::timer, the server's unix time when it fails) at `serverNow`;
// 0 when it has no timer or it ran out.
u32 questTimeLeft(const QuestLogEntry& entry, u32 serverNow);

// The server's unix time from the packed date of SMSG_LOGIN_SETTIMESPEED (vMaNGOS secsToTimeBitFields: minute
// 6 bits, hour 5, weekday 3, day of the month from 0 (6), month from 0 (4), years since 2000 (5)), read as
// local time like the server wrote it. 0 when it is not a date.
u64 packedTimeToUnix(u32 packed);

} // namespace vd::game
