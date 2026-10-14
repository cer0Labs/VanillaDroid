#pragma once

#include "app/CharacterWindows.h"
#include "app/Dialogs.h"
#include "app/ModelFrame.h"
#include "app/ModelViews.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/CharacterData.h"
#include "game/FactionList.h"
#include "game/PaperDoll.h"
#include "game/SkillList.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd {

class Portraits;

class Actors;
class GameSession;
class ModelViews;
class PopupScreens;
namespace game {
class AbilityData;
class GameData;
class ItemData;
struct ActorLook;
} // namespace game
namespace pack {
class PackReader;
}

// The game's side of the character window of phase 15 (app/CharacterWindows): fills the paper doll's model,
// numbers and tooltips for app/InventoryScreens (which keeps the slots and the item actions), draws the
// Reputation, Skills and Honor tabs and the ReputationWatchBar, and the InspectFrame's model and honor tab
// for app/UnitMenuScreens, from the session (game::PaperDoll, game::FactionList, game::SkillList,
// game::CharacterSheet), and applies what the player does there (collapsing, the watched and inactive
// factions, at war, unlearning a profession behind UNLEARN_SKILL). GameScreens owns one. Main thread; the
// view storage is fixed (nothing allocates per frame).
class CharacterScreens {
public:
    void init(const pack::PackReader& db, const game::GameData* data, const game::AbilityData* abilities,
              const game::ItemData* items);
    // The 3D model frames (app/ModelViews) and where a unit's look comes from (its actor's: Actors::look of
    // the actor the query gives for a GUID, as app/Portraits reaches them).
    using ActorQuery = u32 (*)(const void* context, u64 guid);
    void setModelSources(ModelViews* views, Actors* actors, ActorQuery query, const void* context) {
        m_views = views;
        m_actors = actors;
        m_actorQuery = query;
        m_actorContext = context;
    }
    // Retouches (character frame): CharacterFramePortrait / InspectFramePortrait (SetPortraitTexture of the unit:
    // app/Portraits; invalid while it renders or without them).
    void setPortraits(Portraits* portraits) { m_portraits = portraits; }
    TextureHandle portrait(u64 guid);
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Where the UIErrorsFrame's lines go (GameScreens' messages): FactionToggleAtWar's ERR_NOT_IN_COMBAT.
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void setMessageSink(MessageSink sink, void* context) {
        m_message = sink;
        m_messageContext = context;
    }

    // --- The character window (InventoryScreens' Character panel) ---
    // The paper doll's title, level and guild lines, model and numbers into `view` (the slots and tabs are
    // the caller's); then what the player did with them.
    void fillPaperDoll(GameSession& session, PaperDollView& view);
    void paperDollDone(const PaperDollResult& result);
    // CharacterFrame's other pages, on the frame the paper doll's window drew (retouches: their 1.12 art). skillsTab
    // returns true when SkillFrameCancelButton closes the window.
    void reputationTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame);
    bool skillsTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame);
    void honorTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame);
    // Every frame in the world, before the windows: whether the character window shows (CharacterFrame_OnShow
    // / OnHide: the watch bar's text at the top level) and the InspectFrame (InspectFrame_OnHide:
    // ClearInspectPlayer).
    void update(GameSession& session, bool characterShown, bool inspectShown);
    // UnitHasRelicSlot("player"): the ranged slot's empty art is the relic's, and the ammo slot hides.
    bool relicSlot(const GameSession& session) const;

    // --- The HUD ---
    // ReputationWatchBar over the experience bar `xpBar` (app/Hud::experienceBarRect); a tap shows its text a
    // while.
    void drawWatchBar(GameSession& session, DialogContext& c, const ui::Rect& xpBar, u32 nowMs);
    // Development (VD_HUD_DEMO, offline): a watched faction's bar on the sample experience bar
    // (VD_WATCH_BAR_DEMO=60: the top level's, replacing it).
    static void drawWatchBarDemo(DialogContext& c, const ui::Rect& xpBar);

    // --- InspectFrame (app/UnitMenuScreens) ---
    // The InspectFrame of `unit` with its tabs (InspectPaperDollFrame, InspectHonorFrame): `view` comes with
    // the slots, the title and the level line; the model and the honor tab are filled here. Returns the
    // slots' results.
    PaperDollResult inspectWindow(GameSession& session, DialogContext& c, u64 unit, PaperDollView& view,
                                  ItemDrag& drag);
    void inspectClosed(); // InspectFrame_OnHide: the next one starts on its first tab, facing 0.61

    // Development (the gallery): the windows with sample data.
    game::PaperDoll& paperDollState() { return m_doll; }

private:
    Portraits* m_portraits = nullptr; // retouches (character frame)
    TextureHandle model(ModelViewId id, u64 guid, const ModelFrameState& state);
    void message(u32 color, std::string_view line);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    const game::ItemData* m_items = nullptr;
    game::CharacterData m_tables;
    ModelViews* m_views = nullptr;
    Actors* m_actors = nullptr;
    ActorQuery m_actorQuery = nullptr;
    const void* m_actorContext = nullptr;
    PopupScreens* m_popups = nullptr;
    UiSoundSink m_uiSound;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;
    bool m_trace = false;
    u64 m_player = 0; // whose state this is (another character: everything starts over)

    // The paper doll.
    game::PaperDoll m_doll;
    std::array<PaperDollStatView, game::kPaperDollStats> m_statViews{};
    ModelFrameState m_characterModel;
    i32 m_statShown = -1;
    bool m_characterShown = false;
    std::array<char, 96> m_title{};
    std::array<char, 96> m_level{};
    std::array<char, 128> m_guild{};

    // Reputation.
    game::FactionList m_factions;
    std::vector<FactionRowView> m_factionViews; // as many as game::FactionList's rows
    bool m_factionDetail = false;
    i32 m_reputationTip = -1;
    // Skills.
    std::vector<game::SkillLineSource> m_skillSources; // game::kSkillSlots
    game::SkillList m_skills;
    std::vector<SkillLineView> m_skillViews;        // game::kSkillSlots * 2
    std::vector<std::array<char, 40>> m_skillRanks; // as many
    std::array<char, 1024> m_skillDescription{};
    i32 m_skillTip = -1;
    // Honor.
    i32 m_honorTip = -1;
    std::array<char, 64> m_rankTitle{};
    std::array<char, 32> m_rankNumber{};
    std::array<char, 64> m_highestRank{};
    // The watch bar.
    std::array<char, 96> m_watchText{};
    u32 m_watchTextUntilMs = 0;
    // Inspect.
    ModelFrameState m_inspectModel;
    u32 m_inspectTab = 0;
    u64 m_inspected = 0;
    i32 m_inspectHonorTip = -1;
};

} // namespace vd
