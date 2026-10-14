#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

namespace vd::game {

struct Entity;
class QueryCache;

// Phase 15 (items): the dressing room's rules (DressUpModel, WoW.exe 0x504540 behind its TryOn method): which
// items it puts on and where. Also builds a player's look from its update fields, for every model frame that
// shows a changed outfit (the dressing room, the auction house's, the tabard designer's preview).

// What TryOn needs of an item (its query: ItemDisplayInfo id, InventoryType, sheath type).
struct TryOnItem {
    u32 displayId = 0;
    u8 inventoryType = 0;
    u8 sheath = 0;
};

// Whether TryOn changes the model for an item of `inventoryType`: armor that shows (head, shoulders, shirt,
// chest and robes, waist, legs, feet, wrists, hands, back, tabard) and every weapon, shield and held item.
// Necklaces, rings, trinkets, relics, bags, ammo and quivers leave it as it is.
bool canTryOn(u32 inventoryType);

// DressUpModel:TryOn on `look`. Armor replaces its slot. Weapons follow WoW.exe: a one-hand weapon goes to
// the main hand, or to the off hand when the main hand already holds a one-hand or main-hand weapon and the
// character can dual wield; two-handers, main-hand, thrown and "right-handed" ranged weapons (guns,
// crossbows, wands) go to the main hand; shields, held items, off-hand weapons and bows to the off hand. What
// the other hand holds stays only when the two go together (a one-hand or main-hand weapon with a shield or
// held item, or with an off-hand or one-hand weapon when dual wielding); otherwise it is taken off. False
// when nothing changed (see canTryOn).
bool tryOn(CharacterLook& look, const TryOnItem& item, bool canDualWield);

// The look of a player as its update fields tell it (race and gender, PLAYER_BYTES' skin, face, hair and
// facial hair, the items of PLAYER_VISIBLE_ITEM_*) into `out`: DressUpModel:SetUnit("player") and Dress (the
// dressing room's Reset). Item templates come from `queries` (asked for when missing): false while one is
// still unknown (the look lacks it; call again once it arrives).
bool characterLookOf(const Entity& player, QueryCache& queries, CharacterLook& out);

} // namespace vd::game
