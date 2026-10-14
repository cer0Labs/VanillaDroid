#pragma once

#include "app/Dialogs.h"
#include "app/UiPanels.h"
#include "app/UiSound.h"
#include "app/WorldStateWindows.h"
#include "core/Types.h"
#include "game/WorldStateUi.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace vd::pack {
class PackReader;
}
namespace vd::game {
class AreaData;
class Gameplay;
class WorldMapData;
} // namespace vd::game

namespace vd {

class GameSession;
class Hud;
struct HudFrame;
class UiResources;

// Phase 17: the battleground's and outdoor PvP's interface over the world (WorldStateFrame.xml / .lua,
// Blizzard_BattlefieldMinimap): WorldStateAlwaysUpFrame (the flags, resources and bases of WorldStateUI.dbc
// at the top of the screen), WorldStateCaptureBar (the outdoor capture points), WorldStateScoreFrame
// (MSG_PVP_LOG_DATA, the victory) and BattlefieldMinimap (the battleground's map with the group and the flag
// carriers).
//
// Phase 17 (scores) fills them from game::WorldStateUi and game::BattlefieldScore as the 1.12.1 Lua does:
//  - WorldStateAlwaysUpFrame_Update on UPDATE_WORLD_STATES: each GetWorldStateUIInfo with a state > 0 is a
//    line (its icon, text, the dynamic icon flashing at state 2) or, with an ExtendedUI ("CAPTUREPOINT"), a
//    capture bar (CaptureBar_Update with its first extended state; the arrow shows where it last moved);
//    nothing outside a battleground with HIDE_OUTDOOR_WORLD_STATE. The capture bars push the quest tracker
//    down (UIParent_ManageFramePositions).
//  - WorldStateScoreFrame (UIPanelWindows "center", whileDead: app/UiPanels UiPanel::WorldStateScore): shown
//    by ToggleWorldStateScoreFrame while a queue is active, and by itself once there is a winner
//    (UPDATE_WORLD_STATES or UPDATE_BATTLEFIELD_SCORE with GetBattlefieldWinner); hidden by every world
//    entry. While shown it asks RequestBattlefieldScoreData each frame (every 5 s at most:
//    game::BattlefieldScore). Its tab starts on All (SetBattlefieldScoreFaction(nil)) each time it shows; the
//    tabs play igCharacterInfoTab.
//  - BattlefieldMinimap (ToggleBattlefieldMinimap; SHOW_BATTLEFIELD_MINIMAP opens it by itself in a
//    battleground, WorldStateFrame_ToggleMinimap; a world entry with nothing to show hides it): the
//    battleground's map, the player's arrow, the party or raid dots, the teammates out of the group and the
//    flag carrier, with BattlefieldMinimapOptions (opacity 0.7, locked, show teammates) kept in the settings.
//    While it or the world map shows, RequestBattlefieldPositions is asked each frame (every 5 s at most).
// Development: VD_WORLDSTATE_DEMO=wsg|ab|av|ep|si (sample world states over the offline --walk HUD; wsg
// cycles its flags and shows the battle map with two carriers) and VD_SCORE_DEMO=wsg|av|victory (the score
// frame over it). Owned by GameScreens (m_worldStateScreens). Main thread; nothing allocates per frame once
// warmed up.
class WorldStateScreens {
public:
    WorldStateScreens();
    // The tables (WorldStateUI.dbc, ChatChannels.dbc), the battlegrounds' map art.
    void init(const pack::PackReader& db, const game::AreaData* areas, const game::WorldMapData* maps,
              UiResources* resources);
    const game::WorldStateUiData& data() const { return *m_source; }
    // Development (app/WorldStateGallery): another instance's tables and art.
    void share(const WorldStateScreens& game, UiResources* resources);
    const game::WorldMapData* maps() const { return m_maps; }
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }

    // ToggleWorldStateScoreFrame: shows the score (only while a battle is active, as
    // MiniMapBattlefieldFrame.status == "active") or hides it.
    void toggleScore(GameSession& session);
    bool scoreShown() const { return m_scoreShown; }
    void closeScore() { m_scoreShown = false; } // HideUIPanel (its slot went to another window)
    // ToggleBattlefieldMinimap.
    void toggleBattlefieldMinimap(GameSession& session);
    bool battlefieldMinimapShown() const { return m_minimapShown; }

    // Before the HUD draws: where the player is and faces (the battle map's arrow).
    void prepareHud(const HudFrame& hud);
    // In the world, every frame before drawing: the tables on the session's game state, the events (the
    // lines, the victory opening the score, a world entry closing it), the score's and the positions'
    // requests, the settings' SHOW_BATTLEFIELD_MINIMAP.
    void update(GameSession& session, SettingsView& settings, bool worldMapOpen, u32 nowMs);
    // The lines and the capture bars (under the panels), clear of the touch HUD.
    void drawHud(DialogContext& c, const Hud& hud, u64 nowMs);
    // What the capture bars shown take under the minimap (the quest tracker goes under them).
    f32 captureBarsHeight(const DialogContext& c) const;
    // BattlefieldMinimap (under the panels).
    void drawBattlefieldMinimap(GameSession& session, DialogContext& c, const Hud& hud,
                                SettingsView& settings, u64 nowMs);
    // WorldStateScoreFrame in its panel's place (UiPanel::WorldStateScore).
    void drawScore(GameSession& session, DialogContext& c, u32 nowMs);
    // That place: both slots (it is as wide as two windows); on the short wide screens also their whole
    // height and the stick's column, left of the action buttons (its 22 lines would be unreadable in the
    // slots' height).
    static ui::Rect scorePlace(const DialogContext& c, const UiPanelPlaces& places);
    void leaveWorld();

    // Without a server (development): VD_WORLDSTATE_DEMO / VD_SCORE_DEMO over the offline HUD; `scorePlace`
    // is where the score goes (the panels' place for it).
    bool demo() const { return m_demoPlace != 0 || m_demoScore != 0; }
    void drawOffline(GameSession& session, DialogContext& c, const Hud& hud, SettingsView& settings,
                     const UiPanelPlaces& places, u64 nowMs);

    // The views as the game fills them (shared with app/WorldStateGallery): the lines and bars of `play`'s
    // world states (`hideOutdoor`: HIDE_OUTDOOR_WORLD_STATE), the score frame's, the battle map's.
    void buildHud(const game::Gameplay& play, bool hideOutdoor);
    std::span<const AlwaysUpLineView> lines() const { return {m_lines.data(), m_lineCount}; }
    std::span<const CaptureBarView> bars() const { return {m_bars.data(), m_barCount}; }
    WorldStateScoreView buildScore(game::Gameplay& play, std::string_view playerName, u32 nowMs);
    BattlefieldMinimapView buildMinimap(game::Gameplay& play, u32 zoneId, bool hasPlayer, f64 x, f64 y,
                                        f32 facing, std::string_view playerName, const SettingsView& settings,
                                        u32 nowMs);
    WorldStateScoreState& scoreState() { return m_scoreState; }
    BattlefieldMinimapState& minimapState() { return m_minimapState; }
    // A score frame's result applied to `play` (the tabs, Leave Battle, the close button).
    void applyScore(game::Gameplay& play, const WorldStateScoreResult& r, u32 nowMs);
    // A battle map's result applied to the settings (BattlefieldMinimapOptions).
    void applyMinimap(const BattlefieldMinimapResult& r, SettingsView& settings);

private:
    void readDemoSwitches(); // VD_WORLDSTATE_DEMO, VD_SCORE_DEMO
    void readEvents(GameSession& session, SettingsView& settings, u32 nowMs);
    void showScore(game::Gameplay& play, u32 nowMs); // ShowUIPanel: OnShow selects the All tab
    void demoStart(GameSession& session, u32 nowMs);
    void demoUpdate(GameSession& session, u32 nowMs);
    WorldStateHudAvoid avoid(const DialogContext& c, const Hud& hud) const;

    game::WorldStateUiData m_data;
    const game::WorldStateUiData* m_source = &m_data;
    const game::AreaData* m_areas = nullptr;
    const game::WorldMapData* m_maps = nullptr;
    UiResources* m_resources = nullptr;
    UiSoundSink m_uiSound;
    bool m_scoreShown = false;
    bool m_minimapShown = false;

    // The player (prepareHud).
    f64 m_x = 0.0, m_y = 0.0;
    f32 m_facing = 0.0f;

    // WorldStateAlwaysUpFrame / the capture bars, rebuilt on UPDATE_WORLD_STATES.
    static constexpr u32 kMaxLines = 8;
    std::array<AlwaysUpLineView, kMaxLines> m_lines{};
    std::array<std::array<char, 128>, kMaxLines> m_lineTexts{};
    u32 m_lineCount = 0;
    static constexpr u32 kMaxBars = 4;
    std::array<CaptureBarView, kMaxBars> m_bars{};
    std::array<f32, kMaxBars> m_barPositions{}; // each bar's oldValue (CaptureBar_Update)
    std::array<bool, kMaxBars> m_barKnown{};
    u32 m_barCount = 0;
    u32 m_hudStates = ~0u, m_hudList = ~0u; // the world states' and the list's changes built
    bool m_hudHidden = false;
    ui::Rect m_linesRect; // where the lines went this frame (the battle map keeps clear of them)

    // WorldStateScoreFrame.
    WorldStateScoreState m_scoreState;
    std::vector<ScoreRowView> m_rows;                 // reserved: 80
    std::vector<std::array<char, 64>> m_rowRaceClass; // their tooltips' "Night Elf Warrior"
    std::array<ScoreColumnView, 7> m_columns{};
    u32 m_scoreTab = 0;
    u32 m_scoreChanges = ~0u;
    u32 m_scoreTabBuilt = ~0u;
    std::array<char, 48> m_label{};
    std::array<char, 96> m_playerCount{};
    std::array<char, 64> m_runTime{};
    std::array<char, 32> m_timer{};
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0;
    u32 m_worldEntries = 0;

    // BattlefieldMinimap.
    BattlefieldMinimapState m_minimapState;
    std::array<BattlefieldMapDot, 40> m_party{};
    std::array<BattlefieldMapDot, 80> m_team{};
    std::array<BattlefieldMapFlag, 2> m_flags{};
    i8 m_minimapCvar = -1; // a toggle's SHOW_BATTLEFIELD_MINIMAP for update() to save (-1: none)
    bool m_minimapPending = false; // a world entry: the first world states decide whether it shows

    // Development.
    u8 m_demoPlace = 0; // VD_WORLDSTATE_DEMO: 1 wsg, 2 ab, 3 av, 4 ep, 5 si
    u8 m_demoScore = 0; // VD_SCORE_DEMO: 1 wsg, 2 av, 3 victory
    bool m_demoStarted = false;
    u32 m_demoStepMs = 0;
    u32 m_demoStep = 0;
    // CPU of the demo's frames (logged every 600 frames).
    u32 m_costFrames = 0;
    f32 m_costSum = 0.0f, m_costWorst = 0.0f;
};

} // namespace vd
