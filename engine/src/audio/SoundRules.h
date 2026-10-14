#pragma once

#include "core/Types.h"

#include <string_view>

namespace vd::audio {

// What the game sounds like when something happens (parity pass): pure rules over the client's tables and the
// protocol's result names (net::spellCastResultName, inventoryResultName, lootErrorName, buyResultName), tested
// without packs. app/GameAudio applies them.

// VocalUISounds.dbc's enum: one row per value and race, with a male and a female line. What each value is comes
// from the names of its SoundEntries rows (HumanMale_InventoryFull, HumanMale_CantCastNoMana...).
namespace vocal {
constexpr u32 kNone = 0xFFFFFFFFu;
constexpr u32 kInventoryFull = 0;
constexpr u32 kNoAmmo = 1;
constexpr u32 kNoEquipLevel = 2;
constexpr u32 kNoEquipEver = 3;
constexpr u32 kSoulboundNoDrop = 4;
constexpr u32 kItemCooling = 5;
constexpr u32 kTargetTooFar = 10;
constexpr u32 kInvalidTarget = 11;
constexpr u32 kSpellCooling = 12;
constexpr u32 kLocked = 14;
constexpr u32 kNoMana = 15;
constexpr u32 kBankCantAfford = 22;
constexpr u32 kNotABag = 25;
constexpr u32 kCantPutInBag = 26;
constexpr u32 kWrongSlot = 27;
constexpr u32 kOnlyAmmo = 28;
constexpr u32 kBagFull = 29;
constexpr u32 kItemMaxCount = 30;
constexpr u32 kLootDidntKill = 31;
constexpr u32 kLootLocked = 33;
constexpr u32 kLootTooFar = 35;
constexpr u32 kNotEnoughMoney = 40;
constexpr u32 kEquipNoSkill = 41;
constexpr u32 kTwoHandFull = 42;
constexpr u32 kNotEquippable = 44;
constexpr u32 kNoTarget = 45;
constexpr u32 kOutOfRange = 46;
constexpr u32 kNoProficiency = 48;
constexpr u32 kMustEquipItem = 49;
constexpr u32 kAbilityCooling = 50;
constexpr u32 kChestInUse = 52;
constexpr u32 kTaxiNoMoney = 54;
constexpr u32 kNoRage = 63;
constexpr u32 kNoEnergy = 64;
} // namespace vocal

// The character's complaint for a failed cast (SpellCastResult name) by its power type (UNIT_FIELD_BYTES_0 byte 3:
// 0 mana, 1 rage, 3 energy); vocal::kNone when the original says nothing.
u32 vocalForCastFailure(std::string_view reason, u32 powerType);
// ... for an InventoryResult (SMSG_INVENTORY_CHANGE_FAILURE), a LootError (SMSG_LOOT_RESPONSE), a BuyResult
// (SMSG_BUY_FAILED), a bank slot result (SMSG_BUY_BANK_SLOT_RESULT).
u32 vocalForInventoryError(std::string_view result);
u32 vocalForLootError(std::string_view error);
u32 vocalForBuyError(std::string_view error);
u32 vocalForBankSlotError(std::string_view error);

// Footsteps: yards between two steps at `speed` yards per second. On foot about two steps per animation cycle (the
// M2's $FSD events: 1.4 yd walking, 2.3 yd running); mounted the four hooves of the mount's walk and run ($FSD four
// times per cycle: about six a second at a run).
f32 strideLength(f32 speed, bool mounted);

} // namespace vd::audio
