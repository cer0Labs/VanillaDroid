#include "game/ItemCompare.h"

#include "game/ItemDefs.h"

namespace vd::game {

CompareSlots compareSlots(u32 inventoryType, u32 offhandType) {
    CompareSlots out;
    const auto add = [&](u8 slot) {
        if (out.count < out.slot.size()) {
            out.slot[out.count++] = slot;
        }
    };
    switch (inventoryType) {
    case kInvTypeHead: add(kSlotHead); break;
    case kInvTypeNeck: add(kSlotNeck); break;
    case kInvTypeShoulders: add(kSlotShoulders); break;
    case kInvTypeBody: add(kSlotShirt); break;
    case kInvTypeChest:
    case kInvTypeRobe: add(kSlotChest); break;
    case kInvTypeWaist: add(kSlotWaist); break;
    case kInvTypeLegs: add(kSlotLegs); break;
    case kInvTypeFeet: add(kSlotFeet); break;
    case kInvTypeWrists: add(kSlotWrists); break;
    case kInvTypeHands: add(kSlotHands); break;
    case kInvTypeFinger:
        add(kSlotFinger1);
        add(kSlotFinger2);
        break;
    case kInvTypeTrinket:
        add(kSlotTrinket1);
        add(kSlotTrinket2);
        break;
    case kInvTypeCloak: add(kSlotBack); break;
    case kInvTypeTabard: add(kSlotTabard); break;
    case kInvTypeWeapon:
        add(kSlotMainHand);
        if (offhandType == kInvTypeWeapon || offhandType == kInvTypeWeaponOffHand) {
            add(kSlotOffHand); // dual wielding: either hand could take it
        }
        break;
    case kInvType2HWeapon:
        add(kSlotMainHand);
        if (offhandType != kInvTypeNonEquip) {
            add(kSlotOffHand); // it takes both hands
        }
        break;
    case kInvTypeWeaponMainHand: add(kSlotMainHand); break;
    case kInvTypeShield:
    case kInvTypeWeaponOffHand:
    case kInvTypeHoldable: add(kSlotOffHand); break;
    case kInvTypeRanged:
    case kInvTypeThrown:
    case kInvTypeRangedRight: add(kSlotRanged); break;
    default: break; // not equipment, bags, ammo, quivers, relics
    }
    return out;
}

} // namespace vd::game
