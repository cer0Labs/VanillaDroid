#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "core/Types.h"

#include <span>

namespace vd {

// Phase 15 (items): a comparison tooltip (ShoppingTooltipTemplate, GameTooltipTemplate.xml): the "Currently
// Equipped" header in GameFontNormalSmall, the equipped item's name in GameFontNormal and its other lines in
// the small fonts, on the tooltip's backdrop; no buttons. `lines[0]` is the header, `lines[1]` the name. A
// pure view: it only takes the taps that land on it (they do not close the item's tooltip).

// Its height at `width` (as the item tooltip's).
f32 compareTooltipHeight(const DialogContext& c, std::span<const TooltipLineView> lines, f32 width);
void compareTooltip(DialogContext& c, u32 index, const ui::Rect& r, std::span<const TooltipLineView> lines);

} // namespace vd
