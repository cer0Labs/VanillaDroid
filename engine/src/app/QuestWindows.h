#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 13 (quests): the quest log as QuestLogFrame shows it (zone headers that fold, the watch check, the
// detail with its timer, objectives, required money, description and rewards, Abandon / Share / Exit), the
// QuestWatchFrame and the QuestTimerFrame. Pure views over view-models (app/QuestScreens fills them from
// game::Gameplay); the texts the player reads come from core/Text by the client's keys.

// A line of the list (QuestLogTitleButtonTemplate): a zone header with its (+)/(-), or a quest.
struct QuestLogRowView {
    std::string_view title; // the header's zone or sort name; the quest's title
    u32 level = 0;          // quests: colors the title by difficulty (GetDifficultyColor)
    bool header = false;
    bool collapsed = false; // headers: the plus (folded) or the minus
    bool watched = false;   // the check after the title (IsQuestWatched)
    bool complete = false;  // "(Complete)"
    bool failed = false;    // "(Failed)"
    std::string_view tag;   // "Elite", "Dungeon"... (QuestInfo.dbc) unless complete or failed
    u32 groupMates = 0;     // nearby party members on it: "[n]"
};

// The spell a quest teaches (GetQuestLogRewardSpell).
struct QuestSpellRewardView {
    u64 iconHash = 0;
    std::string_view name;   // empty: no spell
    bool tradeskill = false; // REWARD_TRADESKILL_SPELL instead of REWARD_SPELL
};

// The selected quest (QuestLog_UpdateQuestDetails).
struct QuestLogDetailView {
    std::string_view title;
    bool failed = false; // " - (Failed)" after the title
    std::string_view objectivesText;
    std::string_view timeLeft; // SecondsToTime of a timed quest (empty: untimed)
    std::span<const ObjectiveView> objectives;
    u32 requiredMoney = 0;       // copper (REQUIRED_MONEY), 0: none
    bool notEnoughMoney = false; // the coins in red
    std::string_view description;
    std::span<const ItemView> choices; // REWARD_CHOICES
    QuestSpellRewardView spell;
    std::span<const ItemView> rewards; // REWARD_ITEMS / REWARD_ITEMS_ONLY
    u32 money = 0;                     // copper rewarded
};

struct QuestLogPanelView {
    std::span<const QuestLogRowView> rows;
    i32 selected = -1;                          // the selected quest's row (-1 none)
    u32 quests = 0;                             // "Quests: n/20"
    bool allCollapsed = false;                  // the "All" button shows the plus
    bool anyWatched = false;                    // QuestLogTrackTracking: green with any watch, red without
    bool canShare = false;                      // QuestFramePushQuestButton enabled
    const QuestLogDetailView* detail = nullptr; // null: nothing selected (empty log)
};

struct QuestLogPanelResult {
    i32 row = -1;           // a row tapped: a header folds or unfolds, a quest is selected
    i32 linkRow = -1;       // a quest tapped while the chat's edit box is open: its title goes into the chat
    bool toggleAll = false; // QuestLogCollapseAllButton
    bool track = false;     // the Track Quest toggle (touch: the desktop's Shift + click on the title)
    bool abandon = false;
    bool share = false;
    bool closed = false;
    bool scrolled = false; // a scroll bar's button scrolled (PlaySound("UChatScrollButton"))
};
// Quest log: QuestLogFrame with its 1.12 art (app/QuestLogLayout): in a wide place the list beside the parchment
// (not 1.12's); in a tall one the original's arrangement, the list over it.
QuestLogPanelResult questLogPanel(DialogContext& c, const QuestLogPanelView& v);

// A watched quest (QuestWatch_Update): its title, brighter once every objective is met, and its objectives.
struct QuestWatchEntryView {
    std::string_view title;
    std::span<const ObjectiveView> objectives;
};
struct QuestFrameResult {
    i32 tapped = -1;   // the entry tapped (-1 none)
    f32 bottom = 0.0f; // where the frame ended (the next one goes under it)
    ui::Rect bounds;   // questWatchFrame: what it covered (empty: nothing shown)
};
// QuestWatchFrame: text only, as the original (phase 20: without the faint backing it had behind each entry). Its place
// (not 1.12's): over the chat at the left, from `left`, its last entry ending at `bottom` and the first at most up to
// `top` (the entries that do not fit are left out), at most `maxWidth` wide. It takes no finger (the movement stick's
// corner): a quick tap on an entry that nothing moved opens the log on it.
QuestFrameResult questWatchFrame(DialogContext& c, f32 left, f32 top, f32 bottom, f32 maxWidth,
                                 std::span<const QuestWatchEntryView> quests);
// QuestTimerFrame: the dialog box with "Quest Timers" on its header and one SecondsToTime line per timed
// quest; a tap on a line opens the log on its quest (QuestTimerButton_OnClick).
QuestFrameResult questTimerFrame(DialogContext& c, f32 right, f32 top,
                                 std::span<const std::string_view> timers);

// GetQuestLogLeaderBoard's line of an objective: "Kobold Vermin slain: 3/10" (QUEST_MONSTERS_KILLED), "Tough
// Wolf Meat: 8/8" (QUEST_ITEMS_NEEDED / QUEST_OBJECTS_FOUND), or the text alone. Returns what was written.
std::string_view questObjectiveLine(std::span<char> out, const ObjectiveView& o);

} // namespace vd
