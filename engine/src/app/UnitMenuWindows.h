#pragma once

#include "app/Dialogs.h"
#include "core/Hash.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 13 (social): the pure views of the player's menus and raid target icons. Like app/Dialogs they draw a
// view-model and return what the player did (app/UnitMenuScreens fills them from the game).

constexpr u64 kRaidTargetIconsTexture = assetPathHash("Interface\\TargetingFrame\\UI-RaidTargetingIcons.blp");

// SetRaidTargetIconTexture (TargetFrame.lua): icon 1..8 of the 4 x 4 grid of 64-texel cells.
ui::TexCoords raidTargetIconCoords(u8 icon);
// One icon (1..8) stretched over `r`; nothing for 0.
void drawRaidTargetIcon(ui::DrawList& draw, const ui::Rect& r, u8 icon, u32 tint = 0xFFFFFFFF);

// --- UIDropDownMenu ("MENU" display mode) --------------------------------------------------------------------

// One button of a DropDownList as UIDropDownMenu_AddButton lays it out: the check column (UI-CheckBox-Check) left
// of the text unless notCheckable, the icon (15 units) or the expand arrow (ChatFrameExpandArrow) on the right.
struct DropDownItemView {
    std::string_view text;
    u32 color = 0;          // its textR/G/B; 0: HIGHLIGHT_FONT_COLOR (white)
    bool title = false;     // isTitle: NORMAL_FONT_COLOR, disabled, not checkable
    bool enabled = true;    // disabled ones are GRAY_FONT_COLOR
    bool checkable = false; // !notCheckable
    bool checked = false;
    bool hasArrow = false; // a second level
    u8 raidIcon = 0;        // 1..8: info.icon = UI-RaidTargetingIcons
};
struct DropDownMenuView {
    // Where level 1 hangs: its top left corner (logical pixels) as ToggleDropDownMenu anchors it ("TOPLEFT" to
    // the owner's "BOTTOMLEFT" plus the caller's offsets, or the cursor), moved to stay on the screen as the
    // client does (above the anchor when it would go off the bottom, left of it off the right).
    f32 x = 0.0f;
    f32 y = 0.0f;
    std::span<const DropDownItemView> level1;
    // The open second level (empty: none) and the row of level 1 it hangs from (TOPLEFT to that row's
    // TOPRIGHT).
    std::span<const DropDownItemView> level2;
    i32 level2Row = -1;
};
struct DropDownMenuResult {
    i32 level = 0;       // 1 or 2: where `picked` is (0: nothing)
    i32 picked = -1;     // a button tapped (not a title, not disabled)
    i32 toggled = -1;    // level 1: a row with an arrow tapped (the touch screen's hover: opens / closes it)
    bool closed = false; // a tap outside every list (the menu closes)
};
// Touch adaptation: the buttons are tall enough for a finger (16 units at least, more on small screens) and the
// second level opens with a tap on its row instead of the mouse over it; a tap outside closes the menu (the
// client hides it after UIDROPDOWNMENU_SHOW_TIME away from it). Modal while open.
DropDownMenuResult dropDownMenu(DialogContext& c, std::string_view name, const DropDownMenuView& v);
// The size level 1 takes (to place it before drawing it).
ui::Rect dropDownMenuRect(const DialogContext& c, const DropDownMenuView& v);

// --- AutoFollowStatus (ZoneText.xml) -------------------------------------------------------------------------

// "Following Aeryn." in GameFontNormal (20 units) at the center of the world, faded by `alpha` (0..1): shown
// while following, and for AUTOFOLLOW_STATUS_FADETIME seconds after with "You stop following Aeryn.".
void autoFollowStatus(DialogContext& c, std::string_view text, f32 alpha);

} // namespace vd
