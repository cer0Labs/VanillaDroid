#pragma once

#include "core/Types.h"

#include <array>

namespace vd::game {

// Phase 15 (items): what an item is compared with (ShoppingTooltip1 / ShoppingTooltip2 and their "Currently
// Equipped" header). 1.12 shows them from its C side (SHOW_COMPARE_TOOLTIP, Blizzard_AuctionUI's
// SetAuctionCompareItem): one tooltip per equipment slot the item would go in, both fingers for a ring, both
// trinket slots for a trinket; a one-hand weapon is compared with the main hand and, when the off hand holds
// a weapon (dual wield), with that too; a two-handed one with the main hand and whatever the off hand holds
// (equipping it replaces both). Bags, ammo, quivers and relics are not compared.
struct CompareSlots {
    std::array<u8, 2> slot{}; // equipment slots (EQUIPMENT_SLOT_*), in the order the tooltips show them
    u32 count = 0;
};

// The slots an item of `inventoryType` (InventoryType) is compared with. `offhandType`: the InventoryType of
// what the off hand holds (0: nothing). Empty slots are left to the caller (nothing to compare there).
CompareSlots compareSlots(u32 inventoryType, u32 offhandType);

} // namespace vd::game
