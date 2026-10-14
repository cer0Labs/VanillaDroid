#pragma once

#include "app/BattlefieldWindows.h"
#include "app/Dialogs.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "app/UiSound.h"
#include "app/UnitMenuWindows.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

class GameSession;
class PopupScreens;
class WorldStateScreens;

// Phase 17 (queue): the battleground queue's side of the touch UI, as BattlefieldFrame.lua, Minimap.xml and
// StaticPopup.lua run it over game::Battlegrounds and game::BattlefieldQueueing:
//  - BattlefieldFrame (UIPanelWindows "left"): opens on BATTLEFIELDS_SHOW (SetSelectedBattlefield(0)), closes
//    on BATTLEFIELDS_CLOSED, Cancel or Join; its rows' and Join as Group's tooltips on a long press
//    (OnEnter); the igCharacterInfoOpen / Close sounds of its OnShow / OnHide.
//  - BattlefieldFrame_UpdateStatus: PVPENTERQUEUE and the shine for a new queue, PVPTHROUGHQUEUE and
//    CONFIRM_BATTLEFIELD_ENTRY (app/PopupScreens, one per queue) for an invitation, the popup hidden once its
//    queue leaves "confirm".
//  - MiniMapBattlefieldFrame: shown while a queue is not "none" (and the map is), its faction's icon, its
//    tooltip worded again every frame while it shows, MiniMapBattlefieldDropDown (CHANGE_INSTANCE,
//    LEAVE_QUEUE, ENTER_BATTLE), and its OnClick while a battle is "active": ToggleWorldStateScoreFrame, or
//    with Shift ToggleBattlefieldMinimap (app/WorldStateScreens).
// Touch: no hover and no right button. A tap on the button shows its tooltip (a left click does nothing there
// in 1.12 unless a battle is active: then it is the score); a second tap within kDoubleTapMs is the Shift +
// click (the battle map); a long press is the right click (the menu), or the tooltip when there is no menu
// (only the active battle). The frame's tooltips are a long press on the row or the button. Owned by
// GameScreens. Main thread; fixed storage (nothing allocates per frame).
//
// Development: VD_BATTLEFIELD_DEMO=queued|confirm|active over the offline (--walk) HUD: sample queues in
// game::Battlegrounds (demoStatus), the button, its tooltip and menu, and CONFIRM_BATTLEFIELD_ENTRY in a
// popup stack of its own (what the player taps goes to the log, "Battlefield demo: ...").
class BattlefieldScreens {
public:
    static constexpr u32 kMaxRows = 40;       // the list's rows kept ("First Available" and 39 battles)
    static constexpr u32 kTooltipMs = 4000;   // a tooltip a tap or a long press opened stays this long
    static constexpr u32 kDoubleTapMs = 300;  // a second tap this soon on the button: the Shift + click
    static constexpr u32 kButtonFadeMs = 150; // CHAT_FRAME_FADE_TIME
    static constexpr u32 kShineFadeMs = 500;  // BattlegroundShineFadeIn / FadeOut

    void init(PopupScreens* popups, WorldStateScreens* worldStates);
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }

    // Every frame in the world, before the panels: what the game said (the list, the queues' updates).
    void update(GameSession& session, u32 nowMs);
    // UiPanel::Battlefield: wanted while open; the panel rules close it (HideUIPanel: CloseBattlefield,
    // nothing sent).
    bool frameOpen() const { return m_frameOpen; }
    void closeFrame() { m_frameOpen = false; }
    // After the panels synced: OnShow / OnHide's sounds.
    void panelShown(bool shown);
    // In its panel place.
    void drawFrame(GameSession& session, DialogContext& c, u32 nowMs);
    // After the minimap cluster: MiniMapBattlefieldFrame (hidden with the map) and its tooltip.
    void drawMinimapButton(GameSession& session, DialogContext& c, bool minimapShown, u32 nowMs);
    // Over the panels: the frame's tooltips and the button's menu (modal while open).
    void drawOverlay(GameSession& session, DialogContext& c, u32 nowMs);
    // Offline: VD_BATTLEFIELD_DEMO's queues over the HUD (nothing without the switch).
    void drawOffline(GameSession& session, DialogContext& c, bool minimapShown, u32 nowMs);

    // Development (galleries): the button's tooltip text (BattlefieldFrame_UpdateStatus(1)) and menu
    // (MiniMapBattlefieldDropDown_Initialize) for the session's queues. Returns the text / the number of
    // items.
    std::string_view tooltipText(GameSession& session, u32 nowMs);
    u32 buildMenu(GameSession& session);
    std::span<const DropDownItemView> menuItems() const { return {m_menuItems.data(), m_menuCount}; }

private:
    enum class Tip : u8 { None, Button, Row, GroupJoin };

    void readEvents(GameSession& session, u32 nowMs);
    void
    queueJoined(u32 nowMs); // PVPENTERQUEUE, UIFrameFadeIn(MiniMapBattlefieldFrame), BattlegroundShineFadeIn
    std::string_view queueName(GameSession& session, u32 slot); // "<map>" or "<map> <instance>"
    void button(GameSession& session, DialogContext& c, bool minimapShown, u32 nowMs, bool online);
    void menu(GameSession& session, DialogContext& c, bool online, u32 nowMs);
    void pick(GameSession& session, BattlefieldMenuAction action, u32 slot, bool online, u32 nowMs);
    void queueViews(GameSession& session, u32 nowMs);          // m_queueViews from the session's queues
    void click(GameSession& session, bool shift, bool online); // OnClick while "active"
    f32 buttonAlpha(u32 nowMs) const;
    f32 shineAlpha(u32 nowMs) const;
    void demoStart(GameSession& session, u32 nowMs);
    void demoPopups(GameSession& session, DialogContext& c, u32 nowMs);

    PopupScreens* m_popups = nullptr;
    WorldStateScreens* m_worldStates = nullptr;
    UiSoundSink m_uiSound;
    bool m_trace = false;
    u32 m_cursor = 0;
    u64 m_cursorsPlayer = 0;

    // BattlefieldFrame.
    bool m_frameOpen = false;
    bool m_frameShown = false; // last frame (its sounds)
    std::array<BattlefieldRowView, kMaxRows> m_rows{};
    std::array<std::array<char, 64>, kMaxRows> m_rowTexts{};

    // MiniMapBattlefieldFrame.
    bool m_buttonShown = false;
    u32 m_buttonFadeAtMs = 0; // UIFrameFadeIn started (0: none)
    bool m_shineFaded =
        false; // BattlegroundShine starts at alpha 1 until its first fade (as the XML leaves it)
    u32 m_shineAtMs = 0;
    u32 m_pendingTapMs = 0; // a tap waiting for a second one (the score or the battle map)
    bool m_pendingTap = false;
    ui::Rect m_buttonRect;
    BattlefieldButtonPlace m_place;
    std::array<std::array<char, 96>, 3> m_names{};
    std::array<BattlefieldQueueView, 3> m_queueViews{};
    std::array<char, 1024> m_tipText{};

    // The tooltips.
    Tip m_tip = Tip::None;
    i32 m_tipRow = -1;
    ui::Rect m_tipAnchor;
    u32 m_tipUntilMs = 0;

    // MiniMapBattlefieldDropDown.
    bool m_menuOpen = false;
    std::array<DropDownItemView, 11> m_menuItems{};
    std::array<BattlefieldMenuAction, 11> m_menuActions{};
    std::array<u8, 11> m_menuSlots{};
    u32 m_menuCount = 0;

    // VD_BATTLEFIELD_DEMO.
    u32 m_demo = 0; // 0 none, 1 queued, 2 confirm, 3 active
    bool m_demoStarted = false;
    PopupStack m_demoStack;
    std::array<PopupView, PopupStack::kSlots> m_demoViews{};
    std::array<f32, PopupStack::kSlots> m_demoHeights{};
    std::array<std::array<char, 200>, PopupStack::kSlots> m_demoTexts{};
};

} // namespace vd
