#include "game/ActionBar.h"

#include "game/AbilityData.h"
#include "game/Inventory.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/geometric.hpp>

namespace vd::game {
namespace {

namespace uf = net::uf;

constexpr u32 kEffectApplyAura = 6;       // SPELL_EFFECT_APPLY_AURA
constexpr u32 kAuraModShapeshift = 36;    // SPELL_AURA_MOD_SHAPESHIFT
constexpr u32 kAttrExNoAuraIcon = 0x10000000; // SPELL_ATTR_EX_NO_AURA_ICON: the warrior stances
constexpr u32 kFormBear = 5, kFormDireBear = 8;
constexpr u32 kBearFormLevel = 10; // Bear Form's level (Spell.dbc 5487): where Dire Bear Form sits on the bar
constexpr f32 kMeleeRange = 5.0f;       // ATTACK_DISTANCE: the "combat range" of melee abilities (SpellRange 2)
constexpr f32 kMinCombatReach = 1.5f;   // Unit::GetCombatReach(forMeleeRange)
constexpr f32 kMeleeOffset = 1.333333373069763f; // BASE_MELEERANGE_OFFSET
constexpr f32 kMeleeSpellBonus = 1.0f;  // Spell::CheckRange's range_mod for combat range spells
constexpr f32 kRangeGive = 1.25f;       // Spell::CheckRange: the strict check's give for players

u32 bit(u32 value) {
    return value >= 1 && value <= 32 ? 1u << (value - 1) : 0u;
}

f32 floatField(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The shapeshift form a spell applies, 0 if none.
u32 formOf(const SpellDetails& spell) {
    for (const SpellEffectData& e : spell.effects) {
        if (e.effect == kEffectApplyAura && e.aura == kAuraModShapeshift && e.miscValue > 0) {
            return static_cast<u32>(e.miscValue);
        }
    }
    return 0;
}

} // namespace

// --- Slots -------------------------------------------------------------------------------------------------

u32 bonusBarOffset(u32 form, const AbilityData* data) {
    if (data) {
        return data->formBonusBar(form);
    }
    switch (form) {
    case 0x01: return 1; // cat
    case 0x05:
    case 0x08: return 3; // bear, dire bear
    case 0x11: return 1; // battle stance
    case 0x12: return 2; // defensive stance
    case 0x13: return 3; // berserker stance
    case 0x1E: return 1; // stealth
    default: return 0;
    }
}

u32 mainBarSlot(u32 page, u32 button, u32 bonusOffset) {
    button = std::min(button, kActionBarButtons - 1);
    if (page == 0 && bonusOffset > 0) {
        return (kActionBarPages + bonusOffset - 1) * kActionBarButtons + button;
    }
    return (page % kActionBarPages) * kActionBarButtons + button;
}

u32 multiBarSlot(u32 page, u32 button) {
    return (std::clamp(page, 1u, kActionBarPages) - 1) * kActionBarButtons + std::min(button, kActionBarButtons - 1);
}

// --- Button states -----------------------------------------------------------------------------------------

u32 spellPowerCost(const SpellInfo& spell, const Entity& me, const SpellMods* mods) {
    return modifiedPowerCost(modTarget(spell), spell.school, spell.powerType, spell.powerCost, spell.powerCostPercent,
                             &me, mods);
}

ActionUsable spellUsable(const SpellInfo& spell, const SpellDetails* details, const Entity& me, const Entity* target,
                         u32 comboPoints, const AbilityData* forms, const SpellMods* mods,
                         const SpellRequirements* needs, const SpellNeedsState* state) {
    ActionUsable out;
    if (isDead(me)) {
        out.usable = false;
        return out;
    }
    if (details) {
        if (details->casterAuraState != 0 && (me.value(uf::UNIT_FIELD_AURASTATE) & bit(details->casterAuraState)) == 0) {
            out.usable = false;
        }
        if (details->targetAuraState != 0 &&
            (!target || (target->value(uf::UNIT_FIELD_AURASTATE) & bit(details->targetAuraState)) == 0)) {
            out.usable = false;
        }
        if (details->needsComboPoints() && comboPoints == 0) {
            out.usable = false;
        }
        if (forms && !forms->castableInForm(*details, me.byteValue(uf::UNIT_FIELD_BYTES_1, 2))) {
            out.usable = false; // Overpower out of Battle Stance, Claw out of Cat Form, a heal as a bear...
        }
    }
    if (needs && state && spellNeedMissing(spell, details, *needs, me, target, *state) != SpellNeed::Met) {
        out.usable = false; // phase 20 (classes): Ambush out of stealth, Shield Bash without a shield...
    }
    // Short of power only: blue (a spell that is unusable for another reason stays grey).
    const u32 powerType = std::min<u32>(spell.powerType, 4);
    if (me.value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + powerType)) < spellPowerCost(spell, me, mods)) {
        out.noMana = out.usable;
        out.usable = false;
    }
    return out;
}

u64 spellLandsOn(SpellTargetKind kind, u64 player, u64 target, bool targetFriendly) {
    switch (kind) {
    case SpellTargetKind::Enemy: return target;
    case SpellTargetKind::AnyUnit: return target ? target : player;
    case SpellTargetKind::Friend: return target && targetFriendly ? target : player;
    default: return 0;
    }
}

ActionRange spellRange(const SpellInfo& spell, const Entity& me, const Entity* unit, u32 nowMs,
                       const SpellMods* mods) {
    if (!unit || unit == &me || unit->guid == me.guid || spell.maxRange <= 0.0f) {
        return ActionRange::None;
    }
    const f32 maxRange = mods ? mods->apply(modTarget(spell), SpellModOp::Range, spell.maxRange) : spell.maxRange;
    const glm::vec3 a = me.positionAt(nowMs);
    const glm::vec3 b = unit->positionAt(nowMs);
    const f32 myReach = std::max(kMinCombatReach, floatField(me, uf::UNIT_FIELD_COMBATREACH));
    const f32 itsReach = std::max(kMinCombatReach, floatField(*unit, uf::UNIT_FIELD_COMBATREACH));
    if (maxRange <= kMeleeRange + 0.01f && spell.minRange <= 0.0f) {
        // Melee abilities: both reaches and the melee offset, at least 5 yards, on the ground plane.
        const f32 reach = std::max(kMeleeRange, myReach + itsReach + kMeleeSpellBonus + kMeleeOffset);
        const f32 dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy < reach * reach ? ActionRange::In : ActionRange::Out;
    }
    const f32 distance = std::max(0.0f, glm::length(a - b) - myReach - itsReach);
    if (distance > maxRange + kRangeGive) {
        return ActionRange::Out;
    }
    return spell.minRange > 0.0f && distance < spell.minRange ? ActionRange::Out : ActionRange::In;
}

// --- Stance / shapeshift bar -------------------------------------------------------------------------------

u32 shapeshiftForms(std::span<const u32> knownSpells, const SpellText& spells, std::span<ShapeshiftForm> out) {
    u32 count = 0;
    for (const u32 spellId : knownSpells) {
        SpellDetails details;
        if (!spells.details(spellId, details) || details.passive()) {
            continue;
        }
        u32 form = formOf(details);
        if (form == 0) {
            continue;
        }
        const u32 level = form == kFormDireBear ? kBearFormLevel : std::max(details.spellLevel, details.baseLevel);
        // One button per form: the latest rank (Stealth's ranks; Dire Bear Form over Bear Form).
        const u32 slotForm = form == kFormDireBear ? kFormBear : form;
        ShapeshiftForm* same = nullptr;
        for (u32 i = 0; i < count; ++i) {
            const u32 other = out[i].form == kFormDireBear ? kFormBear : out[i].form;
            if (other == slotForm) {
                same = &out[i];
            }
        }
        if (same) {
            SpellDetails kept;
            const u32 keptLevel = spells.details(same->spellId, kept) ? std::max(kept.spellLevel, kept.baseLevel) : 0;
            if (form == kFormDireBear ||
                (same->form != kFormDireBear && std::max(details.spellLevel, details.baseLevel) > keptLevel)) {
                *same = {spellId, form, level};
            }
            continue;
        }
        if (count < out.size()) {
            out[count++] = {spellId, form, level};
        }
    }
    std::sort(out.begin(), out.begin() + count, [](const ShapeshiftForm& a, const ShapeshiftForm& b) {
        return a.level != b.level ? a.level < b.level : a.form < b.form;
    });
    return count;
}

FormTap formTap(const SpellDetails& spell, bool active) {
    if (!active) {
        return FormTap::Cast;
    }
    return (spell.attributesEx & kAttrExNoAuraIcon) != 0 ? FormTap::Nothing : FormTap::Cancel;
}

// --- Phase 20 (classes): what else a spell needs -------------------------------------------------------------

namespace {

constexpr u32 kAttrUsesRangedSlot = 0x00000002;        // SPELL_ATTR_USES_RANGED_SLOT: every ranged ability
constexpr u32 kAttrOnlyStealthed = 0x00020000;         // SPELL_ATTR_ONLY_STEALTHED
constexpr u32 kAttrNotInCombat = 0x10000000;           // SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL
constexpr u32 kAttrExOnlyPeacefulTargets = 0x00000100; // SPELL_ATTR_EX_ONLY_PEACEFUL_TARGETS
constexpr u32 kAttrEx2AutoRepeat = 0x00000020;         // SPELL_ATTR_EX2_AUTO_REPEAT
constexpr u32 kAttrEx3OffHand = 0x01000000;            // SPELL_ATTR_EX3_REQUIRES_OFFHAND_WEAPON
constexpr u32 kTargetFlagItem = 0x00000010;            // TARGET_FLAG_ITEM: aimed at an item, nothing need be worn
constexpr u32 kUnitByte1Creep = 0x02;                  // UNIT_VIS_FLAGS_CREEP (UNIT_FIELD_BYTES_1 byte 3)
constexpr u32 kEffectWeaponDamageNoSchool = 17, kEffectWeaponDamage = 58; // CheckItems' ammunition check
constexpr i32 kWeaponClass = 2, kArmorClass = 4;
constexpr i32 kSubclassBow = 2, kSubclassGun = 3, kSubclassThrown = 16, kSubclassCrossbow = 18;
constexpr u32 kWornMainHand = 15, kWornOffHand = 16, kWornRanged = 17;
constexpr u32 kCreatureTypeBeast = 1, kCreatureTypeHumanoid = 7;
constexpr u32 kCurseOfDoom = 603; // CheckTargetCreatureType: never on a player or its pet (its only rank)

bool fits(const WornItem& item, const SpellInfo& spell) {
    if (item.unknown) {
        return true;
    }
    if (item.itemClass < 0 || item.itemClass != spell.equippedItemClass) {
        return false;
    }
    return spell.equippedItemSubClassMask == 0 || (spell.equippedItemSubClassMask & (1u << item.subclass)) != 0;
}

// A ranged ability (GetWeaponAttackType's RANGED_ATTACK: the ranged damage class, the wands' auto-repeat).
bool rangedAttack(const SpellInfo& spell) {
    return (spell.attributes & kAttrUsesRangedSlot) != 0 || (spell.attributesEx2 & kAttrEx2AutoRepeat) != 0;
}

// Spell::CheckItems' weapon damage at range (Auto Shot, Shoot Bow, Throw): it needs its ammunition.
bool rangedWeaponDamage(const SpellInfo& spell) {
    bool weaponDamage = false;
    for (const u32 effect : spell.effects) {
        weaponDamage = weaponDamage || effect == kEffectWeaponDamage || effect == kEffectWeaponDamageNoSchool;
    }
    return weaponDamage && rangedAttack(spell);
}

bool firesAmmunition(const WornItem& ranged) {
    return ranged.subclass == kSubclassBow || ranged.subclass == kSubclassGun || ranged.subclass == kSubclassCrossbow;
}

u64 guidField(const Entity& unit, u16 index) {
    return unit.value(index) | (u64{unit.value(static_cast<u16>(index + 1))} << 32);
}

} // namespace

bool wornFitsSpell(const SpellInfo& spell, std::span<const WornItem> worn) {
    if (spell.equippedItemClass < 0 || worn.size() < kWornSlots || (spell.targets & kTargetFlagItem) != 0) {
        return true;
    }
    if (spell.equippedItemClass == kWeaponClass) {
        // Spell::CheckItems ignores the other hand's weapon (GetWeaponForAttack of the other attack type).
        const bool offHand = (spell.attributesEx3 & kAttrEx3OffHand) != 0;
        const bool ranged = rangedAttack(spell);
        if ((ranged || !offHand) && fits(worn[kWornMainHand], spell)) {
            return true;
        }
        if ((ranged || offHand) && fits(worn[kWornOffHand], spell)) {
            return true;
        }
        return fits(worn[kWornRanged], spell);
    }
    if (spell.equippedItemClass == kArmorClass) {
        for (u32 slot = 0; slot + 1 < kWornSlots; ++slot) { // the tabard has no dependent spells
            if (slot != kWornMainHand && fits(worn[slot], spell)) {
                return true;
            }
        }
        return false;
    }
    return true; // a class the server does not handle either
}

u32 playerCreatureType(u32 form) {
    switch (form) {
    case 1:  // Cat Form
    case 3:  // Travel Form
    case 4:  // Aquatic Form
    case 5:  // Bear Form
    case 8:  // Dire Bear Form
    case 16: // Ghost Wolf
        return kCreatureTypeBeast;
    default: return kCreatureTypeHumanoid;
    }
}

SpellNeed spellNeedMissing(const SpellInfo& spell, const SpellDetails* details, const SpellRequirements& needs,
                           const Entity& me, const Entity* target, const SpellNeedsState& state) {
    if ((spell.attributes & kAttrOnlyStealthed) != 0 &&
        (me.byteValue(uf::UNIT_FIELD_BYTES_1, 3) & kUnitByte1Creep) == 0) {
        return SpellNeed::Stealth;
    }
    if ((spell.attributes & kAttrNotInCombat) != 0 && inCombat(me)) {
        return SpellNeed::NotInCombat;
    }
    // The rules on the target: only where the spell lands on it (an enemy's or any unit's), never on the caster.
    const SpellTargetKind kind = spell.targetKind();
    if (target && target->guid != me.guid && (kind == SpellTargetKind::Enemy || kind == SpellTargetKind::AnyUnit)) {
        if (details && (details->attributesEx & kAttrExOnlyPeacefulTargets) != 0 && inCombat(*target)) {
            return SpellNeed::PeacefulTarget;
        }
        // Unit::GetCreatureTypeMask: 1 << (type - 1); a type the client does not know yet passes.
        const bool player = target->type == ObjectType::Player;
        u32 mask = needs.targetCreatureType;
        if (spell.id == kCurseOfDoom) {
            const u64 owner = guidField(*target, uf::UNIT_FIELD_CHARMEDBY) != 0
                                  ? guidField(*target, uf::UNIT_FIELD_CHARMEDBY)
                                  : guidField(*target, uf::UNIT_FIELD_SUMMONEDBY);
            if (player || (owner != 0 && (owner >> 48) == 0)) {
                return SpellNeed::CreatureType;
            }
            mask = 0x7FF;
        }
        const u32 type =
            player ? playerCreatureType(target->byteValue(uf::UNIT_FIELD_BYTES_1, 2)) : state.targetCreatureType;
        if (mask != 0 && type != 0 && (mask & bit(type)) == 0) {
            return SpellNeed::CreatureType;
        }
    }
    if (!state.worn.empty() && !wornFitsSpell(spell, state.worn)) {
        return SpellNeed::EquippedItem;
    }
    if (!state.carried) {
        return SpellNeed::Met;
    }
    if (rangedWeaponDamage(spell) && state.worn.size() >= kWornSlots) {
        // A thrown weapon is its own ammunition; a gun, bow or crossbow needs PLAYER_AMMO_ID carried.
        const WornItem& ranged = state.worn[kWornRanged];
        if (!ranged.unknown && ranged.itemClass == kWeaponClass) {
            if (ranged.subclass == kSubclassThrown && ranged.count == 0 &&
                state.carried(state.context, ranged.entry) == 0) {
                return SpellNeed::Ammo;
            }
            const u32 ammo = me.value(uf::PLAYER_AMMO_ID);
            if (firesAmmunition(ranged) && (ammo == 0 || state.carried(state.context, ammo) == 0)) {
                return SpellNeed::Ammo;
            }
        }
    }
    for (u32 i = 0; i < needs.reagents.size(); ++i) {
        if (needs.reagents[i] != 0 && needs.reagentCounts[i] > 0 &&
            state.carried(state.context, needs.reagents[i]) < needs.reagentCounts[i]) {
            return SpellNeed::Reagents;
        }
    }
    for (const u32 totem : needs.totems) {
        if (totem != 0 && state.carried(state.context, totem) == 0) {
            return SpellNeed::Totem;
        }
    }
    return SpellNeed::Met;
}

u32 spellShownCount(const SpellInfo& spell, const SpellRequirements& needs, const Entity& me,
                    const SpellNeedsState& state) {
    if (!state.carried) {
        return 0;
    }
    if (needs.reagents[0] != 0) {
        return state.carried(state.context, needs.reagents[0]);
    }
    if (!rangedWeaponDamage(spell) || state.worn.size() < kWornSlots) {
        return 0;
    }
    const WornItem& ranged = state.worn[kWornRanged];
    if (ranged.unknown || ranged.itemClass != kWeaponClass) {
        return 0;
    }
    if (ranged.subclass == kSubclassThrown) {
        return ranged.count + state.carried(state.context, ranged.entry);
    }
    const u32 ammo = me.value(uf::PLAYER_AMMO_ID);
    return firesAmmunition(ranged) && ammo != 0 ? state.carried(state.context, ammo) : 0;
}

u32 CarriedCounts::count(const Inventory& inventory, u32 entry) const {
    if (entry == 0) {
        return 0;
    }
    if (m_version != inventory.version()) {
        m_version = inventory.version();
        m_used = 0;
        m_next = 0;
    }
    for (u32 i = 0; i < m_used; ++i) {
        if (m_entries[i] == entry) {
            return m_counts[i];
        }
    }
    const u32 at = m_used < kEntries ? m_used++ : (m_next++ % kEntries);
    m_entries[at] = entry;
    m_counts[at] = inventory.count(entry);
    return m_counts[at];
}

} // namespace vd::game
