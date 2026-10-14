#pragma once

#include "app/Dialogs.h"
#include "app/QuestWindows.h"
#include "core/Types.h"
#include "game/Gameplay.h"
#include "game/QuestData.h"
#include "game/QuestJournal.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

namespace game {
class GameData;
} // namespace game
namespace pack {
class PackReader;
}

// Phase 13 (quests): the glue of the quest log, the QuestWatchFrame and the QuestTimerFrame
// (app/QuestWindows) to game::Gameplay — its QuestLog (the server's update fields) and QuestJournal (headers,
// selection, watches) — and the lines of quest sharing, watching and fishing for the UIErrorsFrame. It reads
// a Gameplay, its entities and the player's GUID (a GameSession's, or the gallery's offline one), so the same
// code fills the game and the samples. Main thread; the views' storage is reserved once (nothing allocates
// per frame once warmed up).
class QuestScreens {
public:
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    // Without the client's tables (the gallery): the icon of an item and the spell a quest teaches, from
    // samples.
    struct Sources {
        u64 (*itemIcon)(void* context, u32 entry) = nullptr;
        QuestSpellRewardView (*spell)(void* context, u32 spellId) = nullptr;
        void* context = nullptr;
    };
    // What a frame of the game gives the screens.
    struct Frame {
        game::Gameplay& play;
        const game::EntityStore& entities;
        u64 player = 0;
        u64 serverNow = 0;     // the server's unix time (the timed quests' countdowns)
        bool autoWatch = true; // the AUTO_QUEST_WATCH option
        u32 nowMs = 0;
    };

    QuestScreens();
    // `data`: item icons and spells (Spell.dbc); `areas`: the headers' zone names.
    void init(const pack::PackReader& db, const game::GameData* data, const game::AreaData* areas);
    // The tables set from elsewhere (the gallery's samples).
    game::QuestData& data() { return m_data; }
    void setSources(Sources sources) { m_sources = sources; }
    void setMessageSink(MessageSink sink, void* context) {
        m_sink = sink;
        m_sinkContext = context;
    }
    // A new character in the world: its events are read from now on.
    void restart(const game::Gameplay& play);

    // Once per frame before the windows: AUTO_QUEST_WATCH, the journal's list, the events' lines, the views.
    void update(const Frame& f);
    // The quest log panel (UiPanel::QuestLog). `abandonOut`: a quest whose Abandon was tapped (the caller
    // asks the ABANDON_QUEST popup); `scrolledOut`: a scroll bar's button scrolled (the caller plays
    // UChatScrollButton). Returns true when the log was closed.
    bool drawLog(const Frame& f, DialogContext& c, u32& abandonOut, bool& scrolledOut);
    // QuestTimerFrame under the minimap, at the right from `top`. Returns the quest tapped (0: none): the caller opens
    // the log on it (select() already done).
    u32 drawTimers(const Frame& f, DialogContext& c, f32 right, f32 top);
    // QuestWatchFrame over the chat at the left (questWatchFrame's place: `left`, its bottom at `bottom`, up to `top`
    // at most, `maxWidth`); `bounds` what it covered. Returns the quest tapped as drawTimers.
    u32 drawWatch(const Frame& f, DialogContext& c, f32 left, f32 top, f32 bottom, f32 maxWidth, ui::Rect& bounds);
    bool timersShown() const { return !m_timerQuests.empty(); }
    bool watchShown() const { return !m_watchViews.empty(); }
    // The log shows this quest (a header folded over it unfolds).
    void select(game::Gameplay& play, u32 questId);

private:
    void readEvents(const Frame& f);
    ItemView itemView(game::Gameplay& play, u32 entry, u32 count);
    QuestSpellRewardView spellView(u32 spellId) const;
    void line(u32 color, const char* text);
    void buildViews(const Frame& f);
    // A quest's objectives into m_objectiveViews (resolved names), returns their range.
    std::span<const ObjectiveView> objectives(game::Gameplay& play, u32 questId);
    u32 groupMates(const Frame& f, u32 questId) const;
    // The quest's text with $N, $C, $R, $G and $B resolved for the player (the client's
    // GetQuestLogQuestText).
    std::string_view expand(const Frame& f, std::string_view text, u32 buffer);
    std::string_view partyName(const Frame& f, u64 guid) const;

    game::QuestData m_data;
    const game::GameData* m_gameData = nullptr;
    Sources m_sources;
    MessageSink m_sink = nullptr;
    void* m_sinkContext = nullptr;
    u32 m_eventCursor = 0;

    // Views (storage reserved once).
    std::vector<QuestLogRowView> m_rows;
    std::vector<ObjectiveView> m_objectiveViews;
    std::array<game::QuestObjectiveView, 8> m_objectiveScratch{};
    std::vector<ItemView> m_choices;
    std::vector<ItemView> m_rewards;
    QuestLogDetailView m_detail;
    bool m_hasDetail = false;
    i32 m_selectedRow = -1;
    std::array<char, 64> m_timeLeft{};
    std::array<std::string, 2> m_expanded; // the detail's objectives text and description (capacity kept)
    std::vector<QuestWatchEntryView> m_watchViews;
    std::vector<u32> m_watchQuests;
    std::vector<std::string> m_timerTexts; // SecondsToTime of each timed quest (capacity kept)
    std::vector<std::string_view> m_timerViews;
    std::vector<u32> m_timerQuests;
};

} // namespace vd
