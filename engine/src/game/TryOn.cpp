#include "game/TryOn.h"

#include "game/Entities.h"
#include "game/ItemDefs.h"
#include "game/Queries.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

namespace uf = net::uf;
constexpr u32 kVisibleItemStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;

// The armor slot an inventory type shows in (0xFF: none or a weapon).
u8 armorSlot(u32 inventoryType) {
    switch (inventoryType) {
    case kInvTypeHead: return kSlotHead;
    case kInvTypeShoulders: return kSlotShoulders;
    case kInvTypeBody: return kSlotShirt;
    case kInvTypeChest:
    case kInvTypeRobe: return kSlotChest;
    case kInvTypeWaist: return kSlotWaist;
    case kInvTypeLegs: return kSlotLegs;
    case kInvTypeFeet: return kSlotFeet;
    case kInvTypeWrists: return kSlotWrists;
    case kInvTypeHands: return kSlotHands;
    case kInvTypeCloak: return kSlotBack;
    case kInvTypeTabard: return kSlotTabard;
    default: return 0xFF;
    }
}

bool mainHandWeapon(u32 inventoryType) {
    return inventoryType == kInvTypeWeapon || inventoryType == kInvType2HWeapon ||
           inventoryType == kInvTypeWeaponMainHand || inventoryType == kInvTypeThrown ||
           inventoryType == kInvTypeRangedRight;
}

bool offHandItem(u32 inventoryType) {
    return inventoryType == kInvTypeShield || inventoryType == kInvTypeRanged ||
           inventoryType == kInvTypeHoldable || inventoryType == kInvTypeWeaponOffHand;
}

// WoW.exe 0x504BC0: whether a main-hand item and an off-hand item may be shown together.
bool together(u32 mainType, u32 offType, bool canDualWield) {
    if (mainType != kInvTypeWeapon && mainType != kInvTypeWeaponMainHand) {
        return false;
    }
    if (offType == kInvTypeShield || offType == kInvTypeHoldable) {
        return true;
    }
    return (offType == kInvTypeWeapon || offType == kInvTypeWeaponOffHand) && canDualWield;
}

void put(CharacterLook& look, u8 slot, const TryOnItem& item) {
    look.items[slot] = item.displayId;
    look.inventoryTypes[slot] = item.inventoryType;
    look.sheaths[slot] = item.sheath;
}

void takeOff(CharacterLook& look, u8 slot) {
    look.items[slot] = 0;
    look.inventoryTypes[slot] = 0;
    look.sheaths[slot] = 0;
}

} // namespace

bool canTryOn(u32 inventoryType) {
    return armorSlot(inventoryType) != 0xFF || mainHandWeapon(inventoryType) || offHandItem(inventoryType);
}

bool tryOn(CharacterLook& look, const TryOnItem& item, bool canDualWield) {
    const u32 type = item.inventoryType;
    if (item.displayId == 0) {
        return false;
    }
    if (const u8 slot = armorSlot(type); slot != 0xFF) {
        put(look, slot, item);
        return true;
    }
    if (!mainHandWeapon(type) && !offHandItem(type)) {
        return false;
    }
    const bool mainHeld = look.items[kSlotMainHand] != 0, offHeld = look.items[kSlotOffHand] != 0;
    const u32 mainType = look.inventoryTypes[kSlotMainHand], offType = look.inventoryTypes[kSlotOffHand];
    // A one-hand weapon joins one already in the main hand when both hands can hold one.
    bool offHand = offHandItem(type);
    if (type == kInvTypeWeapon && mainHeld && canDualWield &&
        (mainType == kInvTypeWeapon || mainType == kInvTypeWeaponMainHand)) {
        offHand = true;
    }
    if (offHand) {
        if (mainHeld && !together(mainType, type, canDualWield)) {
            takeOff(look, kSlotMainHand);
        }
        put(look, kSlotOffHand, item);
    } else {
        if (offHeld && !together(type, offType, canDualWield)) {
            takeOff(look, kSlotOffHand);
        }
        put(look, kSlotMainHand, item);
    }
    return true;
}

bool characterLookOf(const Entity& player, QueryCache& queries, CharacterLook& out) {
    out = CharacterLook{};
    out.race = player.byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    out.gender = player.byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    out.skin = player.byteValue(uf::PLAYER_BYTES, 0);
    out.face = player.byteValue(uf::PLAYER_BYTES, 1);
    out.hairStyle = player.byteValue(uf::PLAYER_BYTES, 2);
    out.hairColor = player.byteValue(uf::PLAYER_BYTES, 3);
    out.facialHair = player.byteValue(uf::PLAYER_BYTES_2, 0);
    bool complete = true;
    for (u32 slot = 0; slot < kEquipSlots; ++slot) {
        const u32 entry =
            player.value(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride));
        if (entry == 0) {
            continue;
        }
        if (const net::ItemQueryInfo* item = queries.item(entry)) {
            out.items[slot] = item->displayId;
            out.inventoryTypes[slot] = static_cast<u8>(item->inventoryType);
            out.sheaths[slot] = static_cast<u8>(item->sheath);
        } else {
            complete = false; // asked: the caller tries again when it arrives
        }
    }
    return complete;
}

} // namespace vd::game
