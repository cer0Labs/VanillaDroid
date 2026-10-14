#pragma once

#include "app/PetWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Appearance.h"
#include "game/CombatFeedback.h"
#include "game/GameData.h"
#include "game/PetData.h"

#include <array>

namespace vd {

class GameSession;
class Hud;
class ModelViews;
class Portraits;

// Phase 15 (pets): the game's side of the pet windows and HUD pieces of app/PetWindows (the view-model glue,
// like app/CombatScreens): the stable (PetStableFrame, a UIPanelWindows "left" panel open while the server
// keeps the list), the character window's Pet tab (PetPaperDollFrame), and over the HUD the raid group number
// above the player frame and the hit indicators on the player's and the pet's portraits (CombatFeedback, from
// the combat log's UNIT_COMBATs: game/CombatFeedback). Reads game::Gameplay (its game::PetStable,
// game::Abilities, game::Social), fills the views and applies what the player did. GameScreens owns it. Main
// thread; no allocation per frame once warmed up (texts in fixed buffers; a look is built when the shown pet
// changes).
class PetScreens {
public:
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* text);

    void init(const game::GameData* data, MessageSink sink, void* sinkContext);
    void setPetData(const game::PetData* pets) { m_pets = pets; }
    void setModelViews(ModelViews* views) { m_views = views; }
    // The pet's and a stabled pet's looks (Portraits::look / appearance).
    void setPortraits(Portraits* portraits) { m_portraits = portraits; }
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }

    // Once a frame: the stable's news (ERR_NOT_ENOUGH_MONEY), the stable master's reach, the combat log's
    // UNIT_COMBATs.
    void update(GameSession& session, u32 nowMs);

    // --- The stable ---
    static bool stableOpen(const GameSession& session);
    void drawStable(GameSession& session, DialogContext& c, u32 nowMs);
    void closeStable(GameSession& session); // ClosePetStables (its panel hidden: OnHide)

    // --- The character window's Pet tab ---
    // HasPetUI: a hunter's or a warlock's pet is out (UNIT_FIELD_PETNUMBER): the tab shows (PetTab_Update).
    static bool hasPetUI(const GameSession& session);
    void petTab(GameSession& session, DialogContext& c, const ui::Rect& body);

    // --- Over the HUD (after the pet frame drew: `petFrame`, empty without a pet) ---
    void drawHud(GameSession& session, const Hud& hud, DialogContext& c, const ui::Rect& petFrame, u32 nowMs);
    // Development (gallery): the indicators' state.
    game::CombatFeedback& playerFeedback() { return m_playerFeedback; }
    game::CombatFeedback& petFeedback() { return m_petFeedback; }

private:
    using Text = std::array<char, 96>;
    void readEvents(GameSession& session);
    void readCombat(GameSession& session);
    void message(u32 color, const char* text);
    // The family of a creature entry (its query: asked for when missing), 0 unknown.
    u32 familyOf(GameSession& session, u32 entry) const;
    // "Name Level 12 Wolf" (PetStableLevelText) / "Level 12 Wolf" (PetLevelText) into `out`.
    std::string_view levelText(Text& out, std::string_view name, u32 level, u32 family) const;
    const game::ActorLook* stableLook(GameSession& session, i32 index, u64 petGuid);

    const game::GameData* m_data = nullptr;
    const game::PetData* m_pets = nullptr;
    ModelViews* m_views = nullptr;
    Portraits* m_portraits = nullptr;
    UiSoundSink m_uiSound;
    MessageSink m_sink = nullptr;
    void* m_sinkContext = nullptr;
    u64 m_player = 0;
    u32 m_eventCursor = 0;
    u32 m_combatCursor = 0;
    u32 m_nowMs = 0;

    // The stable.
    bool m_stableShown = false;
    ModelFrameState m_stableModel;
    StableView m_stableView;
    Text m_stableLevel{}, m_stableLoyalty{}, m_stableDiet{};
    std::array<Text, 3> m_slotSub{};
    u32 m_stableDisplay = 0; // the display id m_stableLook was built for
    game::ActorLook m_stableLook;

    // The Pet tab.
    PetPaperDollState m_paperDoll;
    PetPaperDollView m_paperDollView;
    Text m_name{}, m_level{}, m_loyalty{}, m_diet{}, m_training{};
    std::array<Text, 5> m_statLabel{}, m_statValue{}, m_statTip{}, m_combatValue{}, m_combatTip{},
        m_resistValue{};
    std::array<std::array<char, 200>, 2> m_combatSub{};
    std::array<char, 200> m_armorSub{};

    // The hit indicators.
    game::CombatFeedback m_playerFeedback;
    game::CombatFeedback m_petFeedback;
};

} // namespace vd
