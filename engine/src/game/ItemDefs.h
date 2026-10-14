#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

namespace vd::game {

// Item values of the server (Objects/ItemPrototype.h, ItemDefines.h, Player.h, Bag.h; build 5875), shared by the
// inventory model, the tooltip and the windows (phase 8).

// ItemClass.
constexpr u32 kItemClassConsumable = 0;
constexpr u32 kItemClassContainer = 1;
constexpr u32 kItemClassWeapon = 2;
constexpr u32 kItemClassArmor = 4;
constexpr u32 kItemClassReagent = 5;
constexpr u32 kItemClassProjectile = 6;
constexpr u32 kItemClassTradeGoods = 7;
constexpr u32 kItemClassRecipe = 9;
constexpr u32 kItemClassQuiver = 11;
constexpr u32 kItemClassQuest = 12;
constexpr u32 kItemClassKey = 13;
constexpr u32 kItemClassJunk = 15;
// ItemSubclassWeapon values with rules of their own.
constexpr u32 kWeaponBow = 2;
constexpr u32 kWeaponGun = 3;
constexpr u32 kWeaponThrown = 16;
constexpr u32 kWeaponCrossbow = 18;
constexpr u32 kWeaponWand = 19;
constexpr u32 kWeaponFishingPole = 20;
// ItemSubclassArmor.
constexpr u32 kArmorMisc = 0;
constexpr u32 kArmorShield = 6;
// ItemSubclassContainer / ItemSubclassQuiver.
constexpr u32 kContainerSoulBag = 1;
constexpr u32 kQuiverQuiver = 2;
constexpr u32 kQuiverAmmoPouch = 3;

// InventoryType.
constexpr u32 kInvTypeNonEquip = 0;
constexpr u32 kInvTypeHead = 1;
constexpr u32 kInvTypeNeck = 2;
constexpr u32 kInvTypeShoulders = 3;
constexpr u32 kInvTypeBody = 4;
constexpr u32 kInvTypeChest = 5;
constexpr u32 kInvTypeWaist = 6;
constexpr u32 kInvTypeLegs = 7;
constexpr u32 kInvTypeFeet = 8;
constexpr u32 kInvTypeWrists = 9;
constexpr u32 kInvTypeHands = 10;
constexpr u32 kInvTypeFinger = 11;
constexpr u32 kInvTypeTrinket = 12;
constexpr u32 kInvTypeWeapon = 13;
constexpr u32 kInvTypeShield = 14;
constexpr u32 kInvTypeRanged = 15;
constexpr u32 kInvTypeCloak = 16;
constexpr u32 kInvType2HWeapon = 17;
constexpr u32 kInvTypeBag = 18;
constexpr u32 kInvTypeTabard = 19;
constexpr u32 kInvTypeRobe = 20;
constexpr u32 kInvTypeWeaponMainHand = 21;
constexpr u32 kInvTypeWeaponOffHand = 22;
constexpr u32 kInvTypeHoldable = 23;
constexpr u32 kInvTypeAmmo = 24;
constexpr u32 kInvTypeThrown = 25;
constexpr u32 kInvTypeRangedRight = 26;
constexpr u32 kInvTypeQuiver = 27;
constexpr u32 kInvTypeRelic = 28;

// ItemBondingType.
constexpr u32 kBindNone = 0;
constexpr u32 kBindOnPickup = 1;
constexpr u32 kBindOnEquip = 2;
constexpr u32 kBindOnUse = 3;
constexpr u32 kBindQuest = 4;

// ItemPrototypeFlags (the template's flags).
constexpr u32 kItemFlagConjured = 0x00000002;
constexpr u32 kItemFlagLootable = 0x00000004; // "<Right Click to Open>"
constexpr u32 kItemFlagIndestructible = 0x00000020;
constexpr u32 kItemFlagWrapper = 0x00000200;
constexpr u32 kItemFlagCharter = 0x00002000; // phase 15 (guild): ITEM_FLAG_CHARTER, a guild charter
constexpr u32 kItemFlagHasText = 0x00004000;
// ItemDynFlags (ITEM_FIELD_FLAGS of an item object).
constexpr u32 kItemDynFlagBound = 0x00000001; // soulbound
constexpr u32 kItemDynFlagUnlocked = 0x00000004;
constexpr u32 kItemDynFlagWrapped = 0x00000008;
constexpr u32 kItemDynFlagReadable = 0x00000200;

// BagFamily: what a special bag holds (the item's own family must match).
constexpr u32 kBagFamilyNone = 0;
constexpr u32 kBagFamilyArrows = 1;
constexpr u32 kBagFamilyBullets = 2;
constexpr u32 kBagFamilySoulShards = 3;
constexpr u32 kBagFamilyHerbs = 6;
constexpr u32 kBagFamilyEnchanting = 7;
constexpr u32 kBagFamilyEngineering = 8;
constexpr u32 kBagFamilyKeys = 9;

// ItemModType of the template's stats.
constexpr u32 kItemModMana = 0;
constexpr u32 kItemModHealth = 1;
constexpr u32 kItemModAgility = 3;
constexpr u32 kItemModStrength = 4;
constexpr u32 kItemModIntellect = 5;
constexpr u32 kItemModSpirit = 6;
constexpr u32 kItemModStamina = 7;

// EnchantmentSlot of ITEM_FIELD_ENCHANTMENT (3 words each: id, duration, charges).
constexpr u32 kEnchantmentSlots = 7;
constexpr u32 kEnchantPermanent = 0;
constexpr u32 kEnchantTemporary = 1;
constexpr u32 kEnchantProperty0 = 3; // random properties ("of the Bear") use 3..6

// Player slots (bag 255) and bags. The equipment slots are game::EquipSlot (game/Appearance.h) plus these.
constexpr u8 kEquipmentSlotEnd = 19; // EQUIPMENT_SLOT_END: 0 head .. 18 tabard
constexpr u8 kSlotFinger1 = 10;
constexpr u8 kSlotFinger2 = 11;
constexpr u8 kSlotTrinket1 = 12;
constexpr u8 kSlotTrinket2 = 13;
constexpr u8 kBankItemSlotStart = 39; // BANK_SLOT_ITEM_START: the bank's own 24 slots
constexpr u8 kBankItemSlotEnd = 63;
constexpr u8 kBankBagSlotStart = 63; // the 6 bank bag slots
constexpr u8 kBankBagSlotEnd = 69;
constexpr u8 kBuybackSlotStart = 69; // 12 items sold, to buy back
constexpr u8 kBuybackSlotEnd = 81;
constexpr u8 kKeyringSlotStart = 81;
constexpr u8 kKeyringSlotEnd = 97;
constexpr u8 kPlayerSlotCount = 97; // bag 255's slots the model keeps (to the keyring's end)
constexpr u8 kMaxBagSize = 36;      // MAX_BAG_SIZE
constexpr u8 kNullBag = 0;          // NULL_BAG: "anywhere" in CMSG_AUTOSTORE_BAG_ITEM
constexpr u8 kNullSlot = 255;       // NULL_SLOT

} // namespace vd::game
