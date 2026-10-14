#include "game/ItemActions.h"

#include "core/Text.h"
#include "game/Gameplay.h"
#include "net/ItemMessages.h"

namespace vd::game {
namespace {

using Kind = ItemPlace::Kind;

const ItemInstance* itemAt(const Inventory& inv, const ItemPlace& place) {
    switch (place.kind) {
    case Kind::Slot: return inv.at(place.pos);
    case Kind::Buyback:
        return place.index < 12 ? inv.at({255, static_cast<u8>(kBuybackSlotStart + place.index)}) : nullptr;
    default: return nullptr;
    }
}

u32 entryAt(const Inventory& inv, const ItemPlace& place) {
    if (place.kind == Kind::Vendor) {
        const auto items = inv.vendor().items();
        return place.index < items.size() ? items[place.index].itemId : 0;
    }
    if (place.kind == Kind::Ammo) {
        return inv.ammo();
    }
    const ItemInstance* item = itemAt(inv, place);
    return item ? item->entry : 0;
}

bool usable(const net::ItemQueryInfo& info) {
    for (const net::ItemQueryInfo::Spell& spell : info.spells) {
        if (spell.id != 0 && spell.trigger == net::kItemSpellTriggerOnUse) {
            return true;
        }
    }
    return info.startQuest != 0;
}

} // namespace

std::string_view actionLabel(ItemAction action) {
    switch (action) {
    case ItemAction::Use: return text::get("USE");
    case ItemAction::Equip: return text::get("VD_INV_EQUIP");
    case ItemAction::Unequip: return text::get("VD_INV_UNEQUIP");
    case ItemAction::SetAmmo: return text::get("VD_INV_SET_AMMO");
    case ItemAction::Sell: return text::get("VD_INV_SELL");
    case ItemAction::Repair: return text::get("VD_INV_REPAIR");
    case ItemAction::ToBank: return text::get("VD_INV_TO_BANK");
    case ItemAction::FromBank: return text::get("VD_INV_TO_BAGS");
    case ItemAction::Split: return text::get("VD_INV_SPLIT");
    case ItemAction::Destroy: return text::get("VD_INV_DESTROY");
    case ItemAction::Buy: return text::get("VD_INV_BUY");
    case ItemAction::BuyMany: return text::get("VD_INV_BUY_SEVERAL");
    case ItemAction::Buyback: return text::get("BUYBACK");
    case ItemAction::Attach: return text::get("VD_INV_ATTACH");
    case ItemAction::Auction: return text::get("VD_INV_AUCTION");
    case ItemAction::Trade: return text::get("TRADE");
    }
    return {};
}

std::string_view useLabel(Gameplay& play, const ItemPlace& place) {
    const Inventory& inv = play.inventory();
    const ItemInstance* item = itemAt(inv, place);
    const net::ItemQueryInfo* info = item ? play.queries().item(item->entry) : nullptr;
    if (!info || usable(*info)) {
        return text::get("USE");
    }
    if (info->pageText != 0 || item->itemTextId != 0) {
        return text::get("VD_INV_READ");
    }
    return text::get("VD_INV_OPEN");
}

u32 itemActions(Gameplay& play, const ItemPlace& place, std::span<ItemAction> out) {
    u32 count = 0;
    const auto add = [&](ItemAction action) {
        if (count < out.size()) {
            out[count++] = action;
        }
    };
    const Inventory& inv = play.inventory();
    const u32 entry = entryAt(inv, place);
    const net::ItemQueryInfo* info = entry ? play.queries().item(entry) : nullptr;
    if (place.kind == Kind::Vendor) {
        if (info && inv.vendor().open()) {
            add(ItemAction::Buy);
            if (info->stackable > 1) {
                add(ItemAction::BuyMany);
            }
        }
        return count;
    }
    if (place.kind == Kind::Buyback) {
        if (itemAt(inv, place) && inv.vendor().open()) {
            add(ItemAction::Buyback);
        }
        return count;
    }
    const ItemInstance* item = place.kind == Kind::Slot ? inv.at(place.pos) : nullptr;
    if (!item || !info) {
        return count;
    }
    const ItemPos pos = place.pos;
    const bool vendor = inv.vendor().open(), bank = inv.banker() != 0;
    // Phase 8: what the open mailbox, auction house or trade window would take, first (items of the bags).
    const i32 container = inv.containerOf(pos);
    if (container >= kContainerBackpack && container <= kContainerLastBag) {
        const Exchange& ex = play.exchange();
        const u64 guid = item->guid;
        if (ex.mail().open() && ex.mail().attachment() != guid && ex.mail().itemProblem(guid).empty()) {
            add(ItemAction::Attach);
        }
        if (ex.auction().open() && ex.auction().staged() != guid && ex.auction().itemProblem(guid).empty()) {
            add(ItemAction::Auction);
        }
        const Trade& trade = ex.trade();
        if (trade.open() && trade.slotOf(guid) < 0 && trade.freeSlot() >= 0 && !item->soulbound()) {
            add(ItemAction::Trade);
        }
    }
    const bool usableHere = !pos.bank() && !pos.buyback(); // nothing is used from the bank
    if (usableHere && (usable(*info) || info->pageText != 0 || item->itemTextId != 0 ||
                       (info->flags & (kItemFlagLootable | kItemFlagCharter)) || info->lockId != 0)) {
        add(ItemAction::Use);
    }
    if (pos.equipment() || pos.wornBagSlot() || pos.bankBagSlot()) {
        add(ItemAction::Unequip);
    } else if (info->inventoryType == kInvTypeAmmo) {
        add(ItemAction::SetAmmo);
    } else if (info->inventoryType != kInvTypeNonEquip && !pos.bank()) {
        add(ItemAction::Equip);
    }
    if (vendor && !pos.bank() && info->sellPrice > 0) {
        add(ItemAction::Sell);
    }
    if (vendor && inv.vendor().canRepair && item->maxDurability > 0 && item->durability < item->maxDurability) {
        add(ItemAction::Repair);
    }
    if (bank && pos.bank()) {
        add(ItemAction::FromBank);
    } else if (bank && !pos.equipment()) {
        add(ItemAction::ToBank);
    }
    ItemPos room;
    if (item->count > 1 && inv.findEmptySlot(item->entry, pos.bank(), room)) {
        add(ItemAction::Split);
    }
    if (!(info->flags & kItemFlagIndestructible)) {
        add(ItemAction::Destroy);
    }
    return count;
}

bool doItemAction(Gameplay& play, const ItemPlace& place, ItemAction action) {
    Inventory& inv = play.inventory();
    const ItemPos pos = place.pos;
    switch (action) {
    case ItemAction::Use: return place.kind == Kind::Slot && play.useItemAt(pos);
    case ItemAction::Equip: return inv.equip(pos);
    case ItemAction::Unequip: return inv.store(pos);
    case ItemAction::SetAmmo: return inv.setAmmo(entryAt(inv, place));
    case ItemAction::Sell: return inv.sell(pos);
    case ItemAction::Repair: return inv.repair(pos);
    case ItemAction::ToBank: return inv.toBank(pos);
    case ItemAction::FromBank: return inv.fromBank(pos);
    case ItemAction::Buy: return inv.buy(place.index, 1);
    case ItemAction::Buyback: return inv.buyback(place.index);
    case ItemAction::Attach:
    case ItemAction::Auction:
    case ItemAction::Trade: { // phase 8
        const ItemInstance* item = place.kind == Kind::Slot ? inv.at(pos) : nullptr;
        Exchange& ex = play.exchange();
        const i32 free = ex.trade().freeSlot();
        if (!item) {
            return false;
        }
        return action == ItemAction::Attach    ? ex.mail().attach(item->guid)
               : action == ItemAction::Auction ? ex.auction().stage(item->guid)
                                               : free >= 0 && ex.trade().setItem(static_cast<u8>(free), pos);
    }
    case ItemAction::Split:
    case ItemAction::Destroy:
    case ItemAction::BuyMany: return false; // a question first
    }
    return false;
}

bool dropItem(Gameplay& play, const ItemPlace& from, const ItemPlace& to) {
    Inventory& inv = play.inventory();
    const ItemPos src = from.pos;
    switch (from.kind) {
    case Kind::Vendor: // buying: into an empty cell, else anywhere in the bags
        if (to.kind == Kind::Slot && !inv.at(to.pos) && inv.buyInto(from.index, to.pos)) {
            return true;
        }
        return (to.kind == Kind::Slot || to.kind == Kind::Bags || to.kind == Kind::Backpack) && inv.buy(from.index, 1);
    case Kind::Buyback:
        return (to.kind == Kind::Slot || to.kind == Kind::Bags || to.kind == Kind::Backpack) && inv.buyback(from.index);
    case Kind::Slot: break;
    // Phase 8: out of the mail, the auction house's sale slot or the trade window, back to the bags (or into
    // another trade slot).
    case Kind::MailAttachment:
    case Kind::AuctionSale:
    case Kind::Trade: {
        Exchange& ex = play.exchange();
        if (from.kind == Kind::Trade && to.kind == Kind::Trade && from.index < net::kTradeSlotCount) {
            const ItemInstance* item = inv.find(ex.trade().mine().items[from.index]);
            return item && ex.trade().setItem(static_cast<u8>(to.index), item->pos);
        }
        if (to.kind != Kind::Slot && to.kind != Kind::Bags && to.kind != Kind::Backpack) {
            return false;
        }
        if (from.kind == Kind::Trade) {
            return ex.trade().clearItem(static_cast<u8>(from.index));
        }
        if (from.kind == Kind::MailAttachment) {
            ex.mail().detach();
        } else {
            ex.auction().unstage();
        }
        return true;
    }
    default: return false;
    }
    switch (to.kind) {
    case Kind::Slot: {
        const ItemPos dst = to.pos;
        if (dst == src) {
            return false;
        }
        if (dst.wornBagSlot() || dst.bankBagSlot()) {
            // A bag goes into the slot; anything else into the bag that is there.
            const ItemInstance* carried = inv.at(src);
            const net::ItemQueryInfo* info = carried ? play.queries().item(carried->entry) : nullptr;
            if ((info && info->containerSlots > 0) || !inv.at(dst)) {
                return inv.swap(src, dst);
            }
            return inv.store(src, dst.slot);
        }
        return inv.swap(src, dst);
    }
    case Kind::Backpack: return inv.store(src, 255);
    case Kind::Ammo: return inv.setAmmo(entryAt(inv, from));
    case Kind::Vendor:
    case Kind::Buyback:
    case Kind::Sell: return !src.bank() && inv.sell(src);
    case Kind::Bags:
        if (src.bank()) {
            return inv.fromBank(src);
        }
        return (src.equipment() || src.wornBagSlot()) && inv.store(src);
    case Kind::Bank: return !src.bank() && inv.toBank(src);
    case Kind::Equip: return !src.equipment() && inv.equip(src);
    // Phase 8 (game::Exchange).
    case Kind::MailAttachment: return inv.at(src) && play.exchange().mail().attach(inv.at(src)->guid);
    case Kind::AuctionSale: return inv.at(src) && play.exchange().auction().stage(inv.at(src)->guid);
    case Kind::Trade: return play.exchange().trade().setItem(static_cast<u8>(to.index), src);
    default: return false;
    }
}

} // namespace vd::game
