#pragma once

#include "app/BattlefieldWindows.h"
#include "app/Dialogs.h"
#include "app/MinimapView.h"
#include "app/PopupWindows.h"
#include "core/Types.h"

#include <array>
#include <string_view>

namespace vd {

// Development (phase 17, queue): VD_UI_GALLERY pages of the battleground queue with sample data worded by
// core/Text as app/BattlefieldScreens words it:
//  - battlefield: BattlefieldFrame for Warsong Gulch (the 50-59 bracket's five battles, "In Queue" on one,
//    "Ready to Enter" on another, the queued one selected), the player alone (Join as Group disabled);
//  - battlefield2: Alterac Valley, no battle running and queued for the first available one, the player
//    leading a party (Alterac Valley takes no groups: no Join as Group);
//  - battlefield3: Arathi Basin, the player leading a party (Join as Group enabled), the First Available
//    row's tooltip open (a long press on a row or the group button opens its own);
//  - bgbutton: the MinimapCluster with MiniMapBattlefieldFrame for three queues (Warsong Gulch active, Arathi
//    Basin queued, Alterac Valley invited), its tooltip (a tap toggles it), its menu (a long press toggles
//    it) and CONFIRM_BATTLEFIELD_ENTRY.
// What the player taps goes to the log ("UI gallery: battlefield ..."); each page logs its frames' CPU time
// every 600 frames.
class BattlefieldGallery {
public:
    // "battlefield", "battlefield2", "battlefield3", "bgbutton": true when it is one of them (the page to
    // draw).
    bool select(std::string_view page);
    void draw(DialogContext& c, u64 nowMs);

private:
    enum class Page : u8 { Frame, AlteracValley, Group, Button };
    void frame(DialogContext& c, u64 nowMs);
    void button(DialogContext& c, u64 nowMs);

    Page m_page = Page::Frame;
    i32 m_selected = -1; // the selected row (-1: the page's own)
    i32 m_tipRow = -2;   // the row whose tooltip shows (-2: the page's own, -1: none), 100: Join as Group's
    ui::Rect m_tipAnchor;
    std::array<BattlefieldRowView, 8> m_rows{};
    std::array<std::array<char, 48>, 8> m_rowTexts{};
    // bgbutton.
    MinimapGesture m_gesture;
    bool m_tooltip = true;
    bool m_menu = true;
    std::array<BattlefieldQueueView, 3> m_queues{};
    std::array<char, 1024> m_tipText{};
    std::array<DropDownItemView, 11> m_items{};
    std::array<BattlefieldMenuAction, 11> m_actions{};
    std::array<u8, 11> m_slots{};
    std::array<f32, 4> m_popupHeights{};
    std::array<char, 200> m_popupText{};
    // The page's frames' CPU time.
    u32 m_costFrames = 0;
    f32 m_costSum = 0.0f;
    f32 m_costWorst = 0.0f;
};

} // namespace vd
