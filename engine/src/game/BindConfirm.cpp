#include "game/BindConfirm.h"

#include "game/Gameplay.h"
#include "game/ItemDefs.h"

namespace vd::game {
namespace {

using Kind = ItemPlace::Kind;

const net::ItemQueryInfo* templateOf(Gameplay& play, const ItemInstance* item) {
    return item && !item->empty() && item->entry != 0 ? play.queries().item(item->entry) : nullptr;
}

// Its template binds it `when` and it is not bound yet.
bool bindsWhen(Gameplay& play, const ItemInstance* item, u32 when) {
    const net::ItemQueryInfo* info = templateOf(play, item);
    return info && !item->soulbound() && info->bonding == when;
}

bool equippable(Gameplay& play, const ItemInstance* item) {
    const net::ItemQueryInfo* info = templateOf(play, item);
    return info && (info->inventoryType != kInvTypeNonEquip || info->containerSlots > 0);
}

} // namespace

BindAsk lootBindAsk(Gameplay& play, u32 itemEntry) {
    const net::ItemQueryInfo* info = itemEntry != 0 ? play.queries().item(itemEntry) : nullptr;
    return info && info->bonding == kBindOnPickup ? BindAsk::Loot : BindAsk::None;
}

BindAsk itemActionBindAsk(Gameplay& play, const ItemPlace& place, ItemAction action) {
    if (place.kind != Kind::Slot) {
        return BindAsk::None;
    }
    const ItemInstance* item = play.inventory().at(place.pos);
    switch (action) {
    case ItemAction::Equip:
        return !place.pos.equipment() && bindsWhen(play, item, kBindOnEquip) ? BindAsk::AutoEquip
                                                                             : BindAsk::None;
    case ItemAction::Use: return bindsWhen(play, item, kBindOnUse) ? BindAsk::Use : BindAsk::None;
    default: return BindAsk::None;
    }
}

BindAsk dropBindAsk(Gameplay& play, const ItemPlace& from, const ItemPlace& to) {
    if (from.kind != Kind::Slot) {
        return BindAsk::None;
    }
    const Inventory& inv = play.inventory();
    const ItemPos src = from.pos;
    const ItemInstance* carried = inv.at(src);
    if (to.kind == Kind::Equip) { // the character window, not on a slot: equipped where it goes
        return !src.equipment() && bindsWhen(play, carried, kBindOnEquip) ? BindAsk::AutoEquip
                                                                          : BindAsk::None;
    }
    if (to.kind != Kind::Slot || to.pos == src) {
        return BindAsk::None;
    }
    const ItemPos dst = to.pos;
    if (dst.equipment() && !src.equipment()) {
        return bindsWhen(play, carried, kBindOnEquip) ? BindAsk::Equip : BindAsk::None;
    }
    if (dst.wornBagSlot() && !src.wornBagSlot()) { // a bag into a bag slot (anything else goes into that bag)
        const net::ItemQueryInfo* info = templateOf(play, carried);
        return info && info->containerSlots > 0 && bindsWhen(play, carried, kBindOnEquip) ? BindAsk::Equip
                                                                                          : BindAsk::None;
    }
    if (src.equipment() && !dst.equipment()) { // the item in that cell takes the worn one's place
        const ItemInstance* other = inv.at(dst);
        return equippable(play, other) && bindsWhen(play, other, kBindOnEquip) ? BindAsk::Equip
                                                                               : BindAsk::None;
    }
    return BindAsk::None;
}

BindAsk useItemBindAsk(Gameplay& play, u32 itemEntry) {
    const Inventory& inv = play.inventory();
    // Where Gameplay::useItem finds it: the bags first, then what is worn.
    for (i32 container = kContainerBackpack; container <= kContainerLastBag; ++container) {
        const u32 size = inv.containerSize(container);
        for (u32 i = 0; i < size; ++i) {
            const ItemInstance* item = inv.containerItem(container, i);
            if (item && item->entry == itemEntry) {
                return bindsWhen(play, item, kBindOnUse) ? BindAsk::Use : BindAsk::None;
            }
        }
    }
    for (u8 slot = 0; slot < kEquipmentSlotEnd; ++slot) {
        const ItemInstance* worn = inv.equipped(slot);
        if (worn && worn->entry == itemEntry) {
            return bindsWhen(play, worn, kBindOnUse) ? BindAsk::Use : BindAsk::None;
        }
    }
    return BindAsk::None;
}

} // namespace vd::game
