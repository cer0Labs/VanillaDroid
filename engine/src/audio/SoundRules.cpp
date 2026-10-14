#include "audio/SoundRules.h"

#include <algorithm>
#include <initializer_list>

namespace vd::audio {
namespace {

struct NamedVocal {
    std::string_view name;
    u32 vocal;
};

u32 lookup(std::string_view name, std::initializer_list<NamedVocal> table) {
    for (const NamedVocal& v : table) {
        if (v.name == name) {
            return v.vocal;
        }
    }
    return vocal::kNone;
}

} // namespace

u32 vocalForCastFailure(std::string_view reason, u32 powerType) {
    if (reason == "NO_POWER") {
        return powerType == 1 ? vocal::kNoRage : (powerType == 3 ? vocal::kNoEnergy : vocal::kNoMana);
    }
    if (reason == "NOT_READY") {
        // "That spell isn't ready yet" for the casters, "That ability isn't ready yet" for the others.
        return powerType == 0 ? vocal::kSpellCooling : vocal::kAbilityCooling;
    }
    return lookup(reason, {
                              {"OUT_OF_RANGE", vocal::kOutOfRange},
                              {"BAD_TARGETS", vocal::kInvalidTarget},
                              {"BAD_IMPLICIT_TARGETS", vocal::kNoTarget},
                              {"ITEM_NOT_READY", vocal::kItemCooling},
                              {"NO_AMMO", vocal::kNoAmmo},
                              {"NEED_AMMO", vocal::kNoAmmo},
                              {"CHEST_IN_USE", vocal::kChestInUse},
                              {"EQUIPPED_ITEM", vocal::kMustEquipItem},
                              {"EQUIPPED_ITEM_CLASS", vocal::kMustEquipItem},
                              {"EQUIPPED_ITEM_CLASS_MAINHAND", vocal::kMustEquipItem},
                              {"EQUIPPED_ITEM_CLASS_OFFHAND", vocal::kMustEquipItem},
                          });
}

u32 vocalForInventoryError(std::string_view result) {
    return lookup(result, {
                              {"INVENTORY_FULL", vocal::kInventoryFull},
                              {"CANT_EQUIP_LEVEL_I", vocal::kNoEquipLevel},
                              {"YOU_CAN_NEVER_USE_THAT_ITEM", vocal::kNoEquipEver},
                              {"YOU_CAN_NEVER_USE_THAT_ITEM2", vocal::kNoEquipEver},
                              {"CANT_DROP_SOULBOUND", vocal::kSoulboundNoDrop},
                              {"CANT_EQUIP_SKILL", vocal::kEquipNoSkill},
                              {"NO_REQUIRED_PROFICIENCY", vocal::kNoProficiency},
                              {"ITEM_DOESNT_GO_TO_SLOT", vocal::kWrongSlot},
                              {"ONLY_AMMO_CAN_GO_HERE", vocal::kOnlyAmmo},
                              {"NOT_A_BAG", vocal::kNotABag},
                              {"ITEM_DOESNT_GO_INTO_BAG", vocal::kCantPutInBag},
                              {"ITEM_DOESNT_GO_INTO_BAG2", vocal::kCantPutInBag},
                              {"BAG_FULL", vocal::kBagFull},
                              {"BAG_FULL3", vocal::kBagFull},
                              {"BAG_FULL4", vocal::kBagFull},
                              {"BAG_FULL6", vocal::kBagFull},
                              {"CANT_CARRY_MORE_OF_THIS", vocal::kItemMaxCount},
                              {"ITEM_CANT_BE_EQUIPPED", vocal::kNotEquippable},
                              {"CANT_EQUIP_WITH_TWOHANDED", vocal::kTwoHandFull},
                              {"NOT_ENOUGH_MONEY", vocal::kNotEnoughMoney},
                              {"ITEM_LOCKED", vocal::kLocked},
                          });
}

u32 vocalForLootError(std::string_view error) {
    return lookup(error, {
                             {"DIDNT_KILL", vocal::kLootDidntKill},
                             {"TOO_FAR", vocal::kLootTooFar},
                             {"LOCKED", vocal::kLootLocked},
                         });
}

u32 vocalForBuyError(std::string_view error) {
    return lookup(error, {
                             {"NOT_ENOUGHT_MONEY", vocal::kNotEnoughMoney}, // sic, the server's name
                             {"CANT_CARRY_MORE", vocal::kItemMaxCount},
                         });
}

u32 vocalForBankSlotError(std::string_view error) {
    return error == "INSUFFICIENT_FUNDS" ? vocal::kBankCantAfford : vocal::kNone;
}

f32 strideLength(f32 speed, bool mounted) {
    if (mounted) {
        return std::clamp(speed / 6.0f, 0.6f, 3.0f);
    }
    return 0.9f + 0.2f * speed;
}

} // namespace vd::audio
