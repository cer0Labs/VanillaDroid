#pragma once

#include "core/Types.h"
#include "game/ItemActions.h"

namespace vd::game {

class Gameplay;

// The soulbinding questions of the 1.12 client (parity pass, popups). vmangos never asks: it binds an item
// when it is looted (bind on pickup), equipped (bind on equip; bags too, in a bag slot) or used (bind on
// use), and the client decides by itself from the item's template (ItemBondingType) and the item's soulbound
// flag whether to ask first: UIParent's LOOT_BIND_CONFIRM, EQUIP_BIND_CONFIRM (an item put in a slot),
// AUTOEQUIP_BIND_CONFIRM (an item equipped by a click) and USE_BIND_CONFIRM, answered by the LOOT_BIND /
// EQUIP_BIND / AUTOEQUIP_BIND / USE_BIND popups. An item whose template is not known yet is not asked about
// (it is queried on sight; the server binds it anyway).
enum class BindAsk : u8 {
    None,
    Loot,      // LOOT_NO_DROP, then the loot slot is taken
    Equip,     // EQUIP_NO_DROP, then the drop is done
    AutoEquip, // EQUIP_NO_DROP, then the item is equipped
    Use,       // USE_NO_DROP, then the item is used
};

// Taking loot of this template binds it (bind on pickup).
BindAsk lootBindAsk(Gameplay& play, u32 itemEntry);
// An item window's action (game/ItemActions): Equip on an unbound bind-on-equip item, Use on an unbound
// bind-on-use one.
BindAsk itemActionBindAsk(Gameplay& play, const ItemPlace& place, ItemAction action);
// A carried item dropped (game::dropItem): onto an equipment or bag slot, or the character window, equips it;
// a worn item dropped onto a bag cell equips whatever was in that cell.
BindAsk dropBindAsk(Gameplay& play, const ItemPlace& from, const ItemPlace& to);
// The action bar's item (Gameplay::useItem): the first stack of `itemEntry` in the bags, or a worn one.
BindAsk useItemBindAsk(Gameplay& play, u32 itemEntry);

} // namespace vd::game
