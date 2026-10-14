#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>
#include <span>

namespace vd {

// Phase 15 (items): where an item's tooltip and its comparison tooltips go (pure, unit tested).
// Blizzard_AuctionUI puts ShoppingTooltip1 at the GameTooltip's TOPRIGHT (10 units lower) and
// ShoppingTooltip2 at ShoppingTooltip1's TOPRIGHT. Here the tooltip goes beside the item's cell (right of it
// when everything fits, else left of it, the comparisons then mirrored to its left); without room for three
// side by side the second comparison goes under the first; everything stays inside the safe area.
struct TooltipPlacement {
    ui::Rect main;
    std::array<ui::Rect, 2> compare{};
};
// `anchor`: the cell (w 0: none, the tooltip is centred); `size`: the tooltip's width and height;
// `compareHeights`: each comparison's height (as wide as the tooltip); `margin`: the gap to the cell and to
// the safe area's edges; `drop`: how much lower the comparisons start than the tooltip.
TooltipPlacement placeTooltips(const ui::Rect& safe, const ui::Rect& anchor, f32 width, f32 height,
                               std::span<const f32> compareHeights, f32 margin, f32 drop);

} // namespace vd
