#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <span>

namespace vd::game {

struct SpellDetails;
struct SpellInfo;

// Phase 20 (smart, docs/MOBILE_CONTROLS.md §6.1): the touch cluster's automatic consumables. What an item
// restores is read from its on-use spell in Spell.dbc, never from lists of items (the server hides the
// subclass of consumables): an instant heal on the caster (potions; healthstones, spell category 30), an
// energize of mana, rage or energy on the caster (potions, Thistle Tea; mana gems, category 30), eating
// (SPELL_AURA_MOD_REGEN in a spell that cannot be used in combat: Food 433), drinking
// (SPELL_AURA_MOD_POWER_REGEN of mana: Drink 430) and the First Aid channel (a channeled periodic heal: First
// Aid 746). Then the best one carried for a button now (§6.1 "Reglas para elegir"). Plain functions over
// fixed data; no allocation.

constexpr u16 kConsumeHealth = 0x0001; // an instant heal usable in combat
constexpr u16 kConsumeMana = 0x0002;   // instant mana usable in combat
constexpr u16 kConsumeRage = 0x0004;
constexpr u16 kConsumeEnergy = 0x0008;
constexpr u16 kConsumeFood = 0x0010;    // eating (not in combat)
constexpr u16 kConsumeDrink = 0x0020;   // drinking mana (not in combat)
constexpr u16 kConsumeBandage = 0x0040; // the First Aid channel
constexpr u16 kConsumeStone = 0x0080;   // a healthstone or mana gem (category 30): before potions in combat
constexpr u16 kConsumeExtra = 0x0100;   // food or drink doing more than restore ("Well Fed"): never automatic
constexpr u16 kConsumePercent = 0x0200; // food or drink restoring a share of the maximum (amounts in percent)

constexpr u32 kHealthstoneCategory = 30; // SpellCategory of Healthstones and Replenish Mana (the mana gems)

// What an on-use spell restores: its flags and amounts (an effect's base points plus its die; eating,
// drinking and a bandage: per tick times the ticks of the aura's duration). health / power 0 when it restores
// none.
struct ConsumableUse {
    u16 flags = 0;
    u32 health = 0;
    u32 power = 0; // mana, rage or energy (one kind: the flags say which)
    bool any() const {
        return (flags & (kConsumeHealth | kConsumeMana | kConsumeRage | kConsumeEnergy | kConsumeFood |
                         kConsumeDrink | kConsumeBandage)) != 0;
    }
};

// The on-use spell of an item (`info` and `details` of the same spell; `itemCategory`: the item's cooldown
// category for that spell from the item query, 0 none). Nothing (no flags) when it is not a consumable the
// cluster uses.
ConsumableUse classifyConsumable(const SpellInfo& info, const SpellDetails& details, u32 itemCategory = 0);

// One kind of item the player carries, for the choice.
struct ConsumableItem {
    u32 entry = 0;
    u32 count = 0;
    ConsumableUse use;
    bool conjured = false; // ITEM_FLAG_CONJURED: it vanishes (logging out, a while away): used first
    bool usable = true; // the player's level and skills allow it (the item's required level, skill and rank)
    bool ready = true;  // its cooldown is over
};

// Whether an item answers the button `kind` at all (the hold's list of what can be fixed there: every potion,
// stone, food or drink of that kind, those with more effects included).
bool consumableFits(TouchConsumable kind, const ConsumableUse& use);
// The automatic button an item fixed on a consumable slot stands for (its "Automatic" entry): its first kind;
// `fallback` when it fits none.
TouchConsumable consumableKindOf(const ConsumableUse& use, TouchConsumable fallback);
// The best of `items` for the button `kind` now (§6.1), -1 when none fits. Only usable ones; the ready ones
// first (a potion still cooling down when the healthstone is ready); in combat, the healthstone or mana gem
// before the potions; conjured before the rest; then what restores most (`maxHealth` / `maxMana` turn the
// percent ones into points). Food and drink with more than eating never. Health and Mana are the potions and
// stones in combat and eating and drinking out of it; Rage and Energy the potions in combat and the bandages
// out of it.
i32 chooseConsumable(TouchConsumable kind, bool inCombat, std::span<const ConsumableItem> items,
                     u32 maxHealth = 0, u32 maxMana = 0);
// What `kind` looks for now (one kConsume* bit; the stones are among the potions).
u16 consumableWanted(TouchConsumable kind, bool inCombat);

} // namespace vd::game
