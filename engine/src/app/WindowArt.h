#pragma once

#include "core/Types.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd::art {

// Retouches (windows): pieces of 1.12's own window art the touch windows share, drawn from the client's
// textures as their FrameXML templates lay them out, at sizes a finger can hit. Pure drawing (no game state).

// CharacterFrameTabButtonTemplate (TalentTabTemplate, the character, mail, auction and merchant tabs): the
// 3-slice UI-Character-InActiveTab, the selected one UI-Character-ActiveTab 5 units into its frame with
// GameFontHighlightSmall (white) instead of GameFontNormalSmall (gold), UI-Character-Tab-Highlight (ADD)
// while pressed. The client hangs these tabs under its frames; ours stand on top of their pane, so the art is
// flipped to rise from it. Tabs are as wide as PanelTemplates_TabResize makes them (the text, `padding`
// units, the 20-unit ends), at least `minWidth`, and overlap by 15 units as PanelTemplates anchors them;
// squeezed when they do not fit `r`. `r`: the row (its height the tab's 32 units). Returns the tab tapped (-1
// none).
i32 characterTabBar(ui::Gui& g, std::string_view name, const ui::Rect& r,
                    std::span<const std::string_view> tabs, u32 selected, f32 textSize, f32 minWidth,
                    f32 padding = 10.0f);

// Friends frame: the same tabs turned a quarter counter-clockwise to stand along a frame's right edge instead of
// hanging under it (FriendsFrameTab1..4): the tab's top, the side that meets the frame, at the left and its text
// reading upwards; stacked down from the top of `r` in their order, each as long as PanelTemplates_TabResize makes it
// (at least `minLength`) and overlapping the next by 14 units as FriendsFrame anchors them, squeezed when they do not
// fit. `r`: the column, as wide as the tab's 32 units, its left where an unselected tab's top lies (the frame's
// border); the selected one reaches 5 units further left, into the frame. `disabled`: a bit per tab that cannot be
// chosen (PanelTemplates_DisableTab: its text gray). `minHitWidth`: each tab's target grows rightwards, away from the
// frame, to at least that (a finger's width). Returns the tab tapped (-1 none).
i32 characterTabColumn(ui::Gui& g, std::string_view name, const ui::Rect& r, std::span<const std::string_view> tabs,
                       u32 selected, f32 textSize, f32 minLength, u32 disabled = 0, f32 minHitWidth = 0.0f,
                       f32 padding = 10.0f);

// UIRadioButtonTemplate (SendMailRadioButtonTemplate: the send money / C.O.D. choice, the auction durations):
// the 16-unit UI-RadioButton (unchecked, checked quarters; the highlight quarter ADD while pressed) and its
// label in GameFontNormalSmall after it, side by side across `r` (each cell a target). Returns the one tapped
// (-1 none).
i32 radioRow(ui::Gui& g, ui::WidgetId id, const ui::Rect& r, std::span<const std::string_view> labels,
             u32 selected, f32 textSize);

// A page button of the spell book's art (UI-SpellbookIcon-PrevPage-* / NextPage-*: Up, Down while pressed,
// Disabled), centered in the target `r`, with UI-Common-MouseHilight (ADD) while pressed. `label`: its text
// beside it (AuctionFrame's BrowsePrevPageButton: PREV right of the arrow; BrowseNextPageButton: NEXT left of
// it), in GameFontNormal; empty: none. Returns true when tapped.
bool pageButton(ui::Gui& g, ui::WidgetId id, const ui::Rect& r, bool next, bool enabled,
                std::string_view label = {}, f32 textSize = 0.0f);
// The width pageButton needs for an arrow `arrow` wide and its label.
f32 pageButtonWidth(const ui::Gui& g, f32 arrow, std::string_view label, f32 textSize);

// A StatusBar with UI-Character-Skills-Bar (cropped to `fill`) tinted `color` over `background` (0: none),
// and its UI-Character-Skills-BarBorder: where, in the client's units of that frame (the bar barW x barH
// units, the border w x h units from x, y of the bar's top left). GroupLootFrameTimer: 152 x 10, its border
// 156 x 20 at (-2, -5); TradeSkillRankFrame: 268 x 15, its border 281 x 32 at (-5, -8.5).
struct BarBorder {
    f32 barW, barH;
    f32 x, y, w, h;
};
constexpr BarBorder kLootTimerBorder{152.0f, 10.0f, -2.0f, -5.0f, 156.0f, 20.0f};
constexpr BarBorder kRankFrameBorder{268.0f, 15.0f, -5.0f, -8.5f, 281.0f, 32.0f};
void skillsBar(ui::Gui& g, const ui::Rect& bar, f32 fill, u32 color, const BarBorder& border,
               u32 background = 0);

// TradeHighlightTemplate: UI-TradeFrame-Highlight's top and bottom 16 units and its stretched middle over `r`
// (the side of the trade that accepted).
void tradeHighlight(ui::Gui& g, const ui::Rect& r);

} // namespace vd::art
