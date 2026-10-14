#pragma once

#include "app/Dialogs.h"
#include "app/Hud.h"
#include "app/MinimapView.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Minimap.h"

#include <array>

namespace vd {

class GameSession;
class PopupScreens;

namespace game {
class GameData;
class MinimapData;
} // namespace game

// The minimap's side of the touch UI (parity pass): fills the MinimapCluster's view (app/MinimapView) from
// the session's game state every frame (zone, zoom, blips, the ping, tracking, mail, the game clock) and
// applies what the player does there: zoom (the client's two zoom levels, outdoors and indoors, kept in the
// settings), the toggle, pings (game::Minimap), cancelling tracking, and the tooltips the client shows on
// hover, here on a tap. Owned by GameScreens. Main thread; fixed storage (nothing allocates per frame).
class MinimapScreens {
public:
    static constexpr u32 kMaxBlips = 128;
    // A tooltip opened by a tap stays this long (or until the next tap).
    static constexpr u32 kTooltipMs = 4000;

    // `data` (tracking spells' icons) and `minimap` (locks, tracking spells) may be null.
    // `resources`: phase 15, the indoor minimaps' tables and tiles (nullptr: the terrain's map indoors too).
    void init(const game::GameData* data, const game::MinimapData* minimap, UiResources* resources = nullptr);
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Phase 16 (alerts): where MiniMapMeetingStoneFrame's click asks CONFIRM_LEAVE_QUEUE.
    void setPopups(PopupScreens* popups) { m_popups = popups; }

    // Before the HUD draws (also without a server: the zone bar, the map, the clock): the cluster's view in
    // hud.minimap. Reads hud.position, facing, map, indoors, gameTime and the zone name TravelScreens put
    // there.
    void prepareHud(GameSession& session, HudFrame& hud, const SettingsView& settings, u32 nowMs);
    // After the HUD drew: what the player did on the cluster, then the tooltip open over the HUD.
    void drawGame(GameSession& session, const MinimapResult& result, DialogContext& c, SettingsView& settings,
                  u32 nowMs);

    bool shown() const { return m_shown; }

private:
    struct TappedBlip {
        game::BlipKind kind = game::BlipKind::PartyMember;
        u64 guid = 0;
    };

    void collectBlips(GameSession& session, const HudFrame& hud, u32 nowMs);
    void demoBlips(u32 nowMs); // VD_HUD_DEMO without a server: one of each
    void openTooltip(MinimapTip tip, u32 nowMs);
    void tooltipLines(GameSession& session); // the open tooltip's lines, worded again every frame
    void interiorTiles(const HudFrame& hud);  // phase 15: m_view's indoor map
    std::span<char> tipBuffer(u32 line) { return m_tipText[line]; }

    const game::GameData* m_data = nullptr;
    const game::MinimapData* m_minimapData = nullptr;
    UiResources* m_resources = nullptr; // phase 15
    UiSoundSink m_uiSound;
    bool m_demo = false;  // VD_HUD_DEMO
    bool m_trace = false; // VD_TRACE_GAMEPLAY: pings and zoom to the log
    // Phase 16 (alerts): MiniMapMeetingStoneFrame. VD_MEETINGSTONE_DEMO=<AreaTable id> shows it offline
    // (development).
    PopupScreens* m_popups = nullptr;
    u32 m_meetingStoneDemo = 0;

    // This frame.
    MinimapView m_view;
    f32 m_radiusYards = 0.0f;
    bool m_indoors = false;
    u32 m_minutes = 0; // the game clock, minutes since midnight
    u32 m_trackingSpell = 0;
    std::array<game::MinimapBlip, kMaxBlips> m_blips{};
    u32 m_blipCount = 0;
    std::array<MinimapBlipView, kMaxBlips> m_blipViews{};
    u32 m_blipViewCount = 0;
    // Phase 15: the indoor minimap's tiles this frame (the building's, in its window around the player).
    static constexpr u32 kMaxInteriorTiles = 64;
    std::array<MinimapInteriorTile, kMaxInteriorTiles> m_interiorTiles{};
    u32 m_interiorTileCount = 0;

    // State.
    bool m_shown = true;                // MinimapToggleButton (not kept between sessions, as in the client)
    u32 m_lastPingSerial = 0;           // the sound of each new ping
    game::MinimapPingState m_localPing; // offline (development): pings shown without a server

    // The tooltip a tap opened.
    MinimapTip m_tip = MinimapTip::None;
    u32 m_tipUntilMs = 0;
    ui::Rect m_tipAnchor;
    std::array<TappedBlip, 6> m_tappedBlips{};
    u32 m_tappedBlipCount = 0;
    std::array<std::array<char, 96>, 8> m_tipText{};
    std::array<MinimapTooltipLine, 8> m_tipLines{};
    u32 m_tipLineCount = 0;
};

} // namespace vd
