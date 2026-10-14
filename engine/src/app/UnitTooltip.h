#pragma once

#include "core/Types.h"
#include "game/ItemTooltip.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

// Parity pass: the GameTooltip of the HUD (GameTooltipTemplate: UI-Tooltip-Border around the dark blue
// background, the first line in GameTooltipHeaderText, the rest in GameTooltipText) for units
// (GameTooltip:SetUnit, lines from game::buildUnitTooltip), the player frame's PvP badge and the experience
// bar's rest state. Pure views.

struct HudTooltipView {
    std::span<const game::TooltipLine> lines;  // the first one is the header
    std::span<const std::string_view> actions; // buttons under the text (a kept unit tooltip: "Clear target")
    bool interactive = false; // takes its fingers: a tap on it closes it (kept tooltips), its buttons work
};

struct HudTooltipResult {
    bool closed = false; // tapped (interactive only)
    i32 action = -1;     // index into actions
};

// The size `v` needs, at most `maxWidth` wide (wrapping lines wrap to it).
struct HudTooltipSize {
    f32 w = 0.0f;
    f32 h = 0.0f;
};
HudTooltipSize measureHudTooltip(const ui::Gui& g, const HudTooltipView& v, f32 maxWidth);
// Draws it in `r` (sized by measureHudTooltip).
HudTooltipResult hudTooltip(ui::Gui& g, ui::WidgetId id, const ui::Rect& r, const HudTooltipView& v);

// Where tooltips go. The default anchor (GameTooltip_SetDefaultAnchor: the screen's bottom right corner) is
// where the touch HUD keeps its action buttons (`buttons`): above them, right-aligned, when there is room
// below `topLimit` (the minimap cluster), else beside them on the left, bottom-aligned.
ui::Rect defaultTooltipAnchor(const ui::Gui& g, const ui::Rect& safe, const ui::Rect& buttons, f32 topLimit,
                              HudTooltipSize size);
// ANCHOR_RIGHT / ANCHOR_LEFT of something on screen (the PvP badge, the rest tick), kept inside `safe`: right
// of it when `right`, else left; top-aligned with it, or above it when it sits low on the screen.
ui::Rect besideTooltipAnchor(const ui::Gui& g, const ui::Rect& safe, const ui::Rect& owner, bool right,
                             HudTooltipSize size);

} // namespace vd
