#pragma once

#include "core/Types.h"
#include "game/Inventory.h"

#include <span>
#include <string_view>

namespace vd::game {

class Gameplay;

// What a tap or a drop on an item does (phase 8): the rules between the item windows and game::Inventory, kept in
// the game layer so they are tested end to end (the windows only map their cells and zones to places).

// Where an item is, or where a carried item is dropped, as the windows know it.
struct ItemPlace {
    enum class Kind : u8 {
        None,
        Slot,     // a player slot or a bag's slot (pos)
        Backpack, // the backpack's button: into the backpack
        Ammo,     // the character window's ammo slot
        Vendor,   // a vendor row (index)
        Buyback,  // a buyback slot (index 0..11)
        Bags,     // a bags grid, not on a cell
        Bank,     // the bank's grid, not on a cell
        Sell,     // the vendor's list, not on a row
        Equip,    // the character window, not on a slot
        // Phase 8 (game::Exchange).
        MailAttachment, // the item of the message being written
        AuctionSale,    // the auction house's sale slot
        Trade,          // a slot of the player's side of the trade window (index 0..6)
    };
    Kind kind = Kind::None;
    ItemPos pos;
    u32 index = 0;

    static ItemPlace slot(ItemPos p) { return {Kind::Slot, p, 0}; }
    static ItemPlace of(Kind kind, u32 index = 0) { return {kind, {}, index}; }
};

// The buttons of an item's tooltip, in the order they are offered.
enum class ItemAction : u8 {
    Use,      // "Use" / "Read" / "Open" (useLabel tells which)
    Equip,
    Unequip,  // into the bags
    SetAmmo,  // "Ammo"
    Sell,
    Repair,
    ToBank,
    FromBank, // "To Bags"
    Split,    // asks how many first
    Destroy,  // asks first
    Buy,
    BuyMany,  // asks how many first
    Buyback,
    // Phase 8 (game::Exchange): offered first while their window is open.
    Attach,  // "Attach": to the message being written
    Auction, // "Auction": to the auction house's sale slot
    Trade,   // "Trade": into the first free slot of the trade window
};

// The actions that apply to the item at `place` now (vendor or bank open, its template known...). Returns how many
// were written (at most out.size()).
u32 itemActions(Gameplay& play, const ItemPlace& place, std::span<ItemAction> out);
// The label of ItemAction::Use for the item: USE, VD_INV_READ or VD_INV_OPEN (core/Text).
std::string_view useLabel(Gameplay& play, const ItemPlace& place);
// The label of an action's button (core/Text: VD_INV_EQUIP, BUYBACK...).
std::string_view actionLabel(ItemAction action);
// Does an action that needs no question (Split, Destroy and BuyMany do: then use Inventory::split / destroy / buy).
// True when a packet went out (or, for Attach and Auction, the item was set aside for the window).
bool doItemAction(Gameplay& play, const ItemPlace& place, ItemAction action);
// A carried item dropped: swap with or move onto a slot (bags into bag slots, other items into the bag there), into
// the backpack, onto the ammo slot, equip, sell to the vendor, buy from it or buy back, to or from the bank; attach it
// to the mail, set it for sale at the auction house, put it in (or take it out of) the trade window. True when done.
bool dropItem(Gameplay& play, const ItemPlace& from, const ItemPlace& to);

} // namespace vd::game
