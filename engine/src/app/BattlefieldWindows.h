#pragma once

#include "app/BattlefieldText.h"
#include "app/Dialogs.h"
#include "app/MinimapView.h"
#include "app/UnitMenuWindows.h"
#include "core/Hash.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 17 (queue): the pure views of the battleground queue (BattlefieldFrame.xml and Minimap.xml's
// MiniMapBattlefieldFrame). Like app/Dialogs they draw a view-model and return what the player did;
// app/BattlefieldScreens fills them from game::Battlegrounds and game::BattlefieldQueueing. The menu of the
// minimap button is UnitMenuWindows' dropDownMenu (MiniMapBattlefieldDropDown is a UIDropDownMenu).

constexpr u64 kBattlefieldIconAlliance =
    assetPathHash("Interface\\BattlefieldFrame\\Battleground-Alliance.blp");
constexpr u64 kBattlefieldIconHorde = assetPathHash("Interface\\BattlefieldFrame\\Battleground-Horde.blp");

// --- BattlefieldFrame (UIPanelWindows "left")
// ----------------------------------------------------------------

// One BattlefieldButtonTemplate row: "First Available" or "<map> <instance>", and the queue's status at its
// right.
struct BattlefieldRowView {
    std::string_view text;
    std::string_view status; // BATTLEFIELD_QUEUE_STATUS / BATTLEFIELD_CONFIRM_STATUS, or empty
};
struct BattlefieldFrameView {
    std::string_view title; // BattlefieldFrameFrameLabel: the map's name
    std::span<const BattlefieldRowView> rows;
    i32 selected = 0;             // the row with LockHighlight (GetSelectedBattlefield: 0 is First Available)
    std::string_view description; // BattlefieldFrameZoneDescription (GameFontBlack on the frame's lower half)
    bool groupJoinShown = false;  // CanJoinBattlefieldAsGroup
    bool groupJoinEnabled = false; // a party or raid the player leads
};
struct BattlefieldFrameResult {
    bool closed = false;      // the X or Cancel: HideUIPanel
    i32 select = -1;          // a row tapped (BattlefieldButton_OnClick)
    bool join = false;        // BATTLEFIELD_JOIN
    bool joinAsGroup = false; // BATTLEFIELD_GROUP_JOIN
    i32 rowTip = -1;          // a row held: its tooltip (OnEnter: the title, then its NEWBIE_TOOLTIP_*)
    bool groupTip = false;    // Join as Group held: NEWBIE_TOOLTIP_BATTLEFIELD_GROUP_JOIN
    ui::Rect tipAnchor;       // what was held
};
// The frame: UI-Battlefield-Icon as its portrait, the map's name as its title, BATTLEFIELD_NAME over the list
// ("First Available" first, the selected one highlighted with UI-Listbox-Highlight2 in gold), the
// description, then Join as Group (when shown), Join Battle and Cancel. Wide places put the description
// beside the list.
BattlefieldFrameResult battlefieldFrame(DialogContext& c, const BattlefieldFrameView& v);

// GameTooltip as BattlefieldButtonTemplate's OnEnter fills it: `title` white, `text` gold and wrapped, beside
// `anchor` (right of it, else left; kept on the screen).
void battlefieldTooltip(DialogContext& c, const ui::Rect& anchor, std::string_view title,
                        std::string_view text);

// --- MiniMapBattlefieldFrame
// -----------------------------------------------------------------------------------

// Its top left in the cluster's units. The original: 33 x 33 at the Minimap's BOTTOMLEFT + (13, -13), its centre
// 77.9 units from the map's (105, 92) at 239 degrees; there the micro menu's gear (app/MicroMenu: 14 units out of the
// ring at 215 degrees) covers it, so the button goes down the ring to 255 degrees at the same distance.
struct BattlefieldButtonPlace {
    f32 x = 68.3f, y = 150.8f;
};
// Its touch target (the 33 x 33 frame grown a little for a finger).
ui::Rect battlefieldButtonRect(const MinimapLayout& l, BattlefieldButtonPlace p);
struct BattlefieldButtonView {
    u64 icon = kBattlefieldIconAlliance; // MiniMapBattlefieldIcon: Battleground-<UnitFactionGroup("player")>
    f32 alpha = 1.0f;                    // the frame's (UIFrameFadeIn on a new queue)
    f32 shine = 0.0f;                    // BattlegroundShine's alpha (ComboPoint's star, added)
    BattlefieldButtonPlace place;        // where (battlefieldButtonPlace)
};
struct BattlefieldButtonResult {
    bool tapped = false;    // a tap (OnClick, LeftButton)
    bool longPress = false; // held still (the right button's place on a touch screen)
    ui::Rect rect;          // where it is (the tooltip's and the menu's anchor)
};
// The icon (32 x 32, one unit left of the centre), MiniMap-TrackingBorder (52 x 52 at its TOPLEFT) and the
// shine (32 x 32 at the centre + (3, 3)); the zoom buttons' highlight while pressed.
BattlefieldButtonResult battlefieldMinimapButton(DialogContext& c, const MinimapLayout& l,
                                                 const BattlefieldButtonView& v);
// The menu's top left: ToggleDropDownMenu(1, nil, MiniMapBattlefieldDropDown, "MiniMapBattlefieldFrame", 10,
// -5).
ui::Rect battlefieldMenuAnchor(const MinimapLayout& l, BattlefieldButtonPlace p);

// GameTooltip:SetText(this.tooltip) with SetOwner(this, "ANCHOR_LEFT"): the text's lines (and its |c..|r
// colours) over SetText's default NORMAL_FONT_COLOR (WoW.exe 0x531B90 reads 0xC0D3E8 = 255, 210, 0), its
// bottom right at the button's top left, kept on the screen.
void battlefieldButtonTooltip(DialogContext& c, const ui::Rect& anchor, std::string_view text);

} // namespace vd
