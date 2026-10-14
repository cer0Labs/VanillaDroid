#include "game/Consumables.h"

#include "game/GameData.h"
#include "game/SpellText.h"

#include <algorithm>

namespace vd::game {
namespace {

// Spell.dbc numbers (vMaNGOS SpellDefines.h / SharedDefines.h).
constexpr u32 kEffectHeal = 10;              // SPELL_EFFECT_HEAL
constexpr u32 kEffectEnergize = 30;          // SPELL_EFFECT_ENERGIZE (misc: the power)
constexpr u32 kEffectApplyAura = 6;          // SPELL_EFFECT_APPLY_AURA
constexpr u32 kAuraPeriodicHeal = 8;         // SPELL_AURA_PERIODIC_HEAL: a bandage's channel
constexpr u32 kAuraObsModHealth = 20;        // SPELL_AURA_OBS_MOD_HEALTH: a share of the health each tick
constexpr u32 kAuraObsModMana = 21;          // SPELL_AURA_OBS_MOD_MANA
constexpr u32 kAuraModRegen = 84;            // SPELL_AURA_MOD_REGEN: eating
constexpr u32 kAuraModPowerRegen = 85;       // SPELL_AURA_MOD_POWER_REGEN: drinking (misc: the power)
constexpr u32 kAttrNotInCombat = 0x10000000; // SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL
constexpr u32 kTargetCaster = 1;             // TARGET_UNIT_CASTER
constexpr u32 kPowerMana = 0, kPowerRage = 1, kPowerEnergy = 3;
// Aura::HandleModRegen / HandleModPowerRegen: the ticks of eating and drinking when the effect gives no
// amplitude.
constexpr u32 kFoodTickMs = 5000;
constexpr u32 kDrinkTickMs = 2000;

// An effect's points at their highest (base points plus the die).
u32 points(const SpellEffectData& e) {
    const i32 value = e.basePoints + std::max(e.dieSides, static_cast<i32>(e.baseDice));
    return value > 0 ? static_cast<u32>(value) : 0u;
}

// Points per tick over the aura's duration (the client's $o: 17 every 5 s for 18 s, 61).
u32 overTime(const SpellDetails& spell, const SpellEffectData& e, u32 defaultTickMs) {
    const u32 tick = e.amplitudeMs != 0 ? e.amplitudeMs : defaultTickMs;
    if (spell.durationMs == 0 || tick == 0) {
        return points(e);
    }
    return static_cast<u32>(static_cast<u64>(points(e)) * spell.durationMs / tick);
}

u32 amountFor(TouchConsumable kind, const ConsumableItem& item, u32 maxHealth, u32 maxMana) {
    const bool power = kind == TouchConsumable::Mana || kind == TouchConsumable::Drink ||
                       kind == TouchConsumable::Rage || kind == TouchConsumable::Energy;
    const u32 amount = power ? item.use.power : item.use.health;
    if ((item.use.flags & kConsumePercent) != 0) {
        return static_cast<u32>(static_cast<u64>(amount) * (power ? maxMana : maxHealth) / 100u);
    }
    return amount;
}

} // namespace

ConsumableUse classifyConsumable(const SpellInfo& info, const SpellDetails& details, u32 itemCategory) {
    ConsumableUse use;
    const bool peaceful = (details.attributes & kAttrNotInCombat) != 0;
    bool extra = false;
    for (u32 i = 0; i < details.effects.size(); ++i) {
        const SpellEffectData& e = details.effects[i];
        if (e.effect == 0) {
            continue;
        }
        const bool onCaster = info.implicitTargetA[i] == kTargetCaster;
        const bool aura = e.effect == kEffectApplyAura && onCaster;
        if (peaceful) {
            // Eating and drinking; anything else beside them (Well Fed's trigger, an instant heal) is more
            // than that.
            if (aura && e.aura == kAuraModRegen) {
                use.flags |= kConsumeFood;
                use.health += overTime(details, e, kFoodTickMs);
            } else if (aura && e.aura == kAuraObsModHealth) {
                use.flags |= kConsumeFood | kConsumePercent;
                use.health += overTime(details, e, kFoodTickMs);
            } else if (aura && e.aura == kAuraModPowerRegen && e.miscValue == static_cast<i32>(kPowerMana)) {
                use.flags |= kConsumeDrink;
                use.power += overTime(details, e, kDrinkTickMs);
            } else if (aura && e.aura == kAuraObsModMana) {
                use.flags |= kConsumeDrink | kConsumePercent;
                use.power += overTime(details, e, kDrinkTickMs);
            } else {
                extra = true;
            }
            continue;
        }
        if (details.channeled() && e.effect == kEffectApplyAura && e.aura == kAuraPeriodicHeal) {
            use.flags |=
                kConsumeBandage; // First Aid: channeled on a friendly unit (the player or its target)
            use.health += overTime(details, e, 1000);
        } else if (e.effect == kEffectHeal && onCaster) {
            use.flags |= kConsumeHealth;
            use.health += points(e);
        } else if (e.effect == kEffectEnergize && onCaster) {
            const u32 power = static_cast<u32>(std::max(0, e.miscValue));
            use.flags |= power == kPowerMana     ? kConsumeMana
                         : power == kPowerRage   ? kConsumeRage
                         : power == kPowerEnergy ? kConsumeEnergy
                                                 : 0;
            use.power += power == kPowerMana || power == kPowerRage || power == kPowerEnergy ? points(e) : 0u;
        }
    }
    if (extra && (use.flags & (kConsumeFood | kConsumeDrink)) != 0) {
        use.flags |= kConsumeExtra;
    }
    if ((use.flags & (kConsumeHealth | kConsumeMana)) != 0 &&
        (info.category == kHealthstoneCategory || itemCategory == kHealthstoneCategory)) {
        use.flags |= kConsumeStone;
    }
    if (!use.any()) {
        use = {};
    }
    return use;
}

u16 consumableWanted(TouchConsumable kind, bool inCombat) {
    switch (kind) {
    case TouchConsumable::Health: return inCombat ? kConsumeHealth : kConsumeFood;
    case TouchConsumable::Mana: return inCombat ? kConsumeMana : kConsumeDrink;
    case TouchConsumable::Rage: return kConsumeRage;     // in and out of combat (the bandage has its own slot)
    case TouchConsumable::Energy: return kConsumeEnergy;
    case TouchConsumable::Bandage: return kConsumeBandage;
    case TouchConsumable::Food: return kConsumeFood;
    case TouchConsumable::Drink: return kConsumeDrink;
    default: return 0;
    }
}

bool consumableFits(TouchConsumable kind, const ConsumableUse& use) {
    switch (kind) {
    case TouchConsumable::Health: return (use.flags & (kConsumeHealth | kConsumeFood)) != 0;
    case TouchConsumable::Mana: return (use.flags & (kConsumeMana | kConsumeDrink)) != 0;
    case TouchConsumable::Rage: return (use.flags & kConsumeRage) != 0;
    case TouchConsumable::Energy: return (use.flags & kConsumeEnergy) != 0;
    case TouchConsumable::Bandage: return (use.flags & kConsumeBandage) != 0;
    case TouchConsumable::Food: return (use.flags & kConsumeFood) != 0;
    case TouchConsumable::Drink: return (use.flags & kConsumeDrink) != 0;
    default: return false;
    }
}

TouchConsumable consumableKindOf(const ConsumableUse& use, TouchConsumable fallback) {
    if ((use.flags & (kConsumeHealth | kConsumeFood)) != 0) {
        return TouchConsumable::Health;
    }
    if ((use.flags & (kConsumeMana | kConsumeDrink)) != 0) {
        return TouchConsumable::Mana;
    }
    if ((use.flags & kConsumeRage) != 0) {
        return TouchConsumable::Rage;
    }
    if ((use.flags & kConsumeEnergy) != 0) {
        return TouchConsumable::Energy;
    }
    if ((use.flags & kConsumeBandage) != 0) {
        return TouchConsumable::Bandage;
    }
    return fallback;
}

i32 chooseConsumable(TouchConsumable kind, bool inCombat, std::span<const ConsumableItem> items,
                     u32 maxHealth, u32 maxMana) {
    const u16 wanted = consumableWanted(kind, inCombat);
    // The amount that counts: what the button restores (out of combat, Rage and Energy are the bandages:
    // health).
    const TouchConsumable measure = wanted == kConsumeBandage ? TouchConsumable::Bandage : kind;
    const bool stonesFirst = inCombat && (kind == TouchConsumable::Health || kind == TouchConsumable::Mana);
    i32 best = -1;
    const auto better = [&](const ConsumableItem& a, const ConsumableItem& b) {
        if (a.ready != b.ready) {
            return a.ready;
        }
        const bool aStone = stonesFirst && (a.use.flags & kConsumeStone) != 0;
        const bool bStone = stonesFirst && (b.use.flags & kConsumeStone) != 0;
        if (aStone != bStone) {
            return aStone;
        }
        if (a.conjured != b.conjured) {
            return a.conjured;
        }
        const u32 aAmount = amountFor(measure, a, maxHealth, maxMana);
        const u32 bAmount = amountFor(measure, b, maxHealth, maxMana);
        if (aAmount != bAmount) {
            return aAmount > bAmount;
        }
        return a.entry < b.entry;
    };
    for (u32 i = 0; i < items.size(); ++i) {
        const ConsumableItem& item = items[i];
        if (item.count == 0 || !item.usable || (item.use.flags & wanted) == 0 ||
            (item.use.flags & kConsumeExtra) != 0) {
            continue;
        }
        if (best < 0 || better(item, items[static_cast<u32>(best)])) {
            best = static_cast<i32>(i);
        }
    }
    return best;
}

} // namespace vd::game
