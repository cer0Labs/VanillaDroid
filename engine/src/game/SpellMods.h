#pragma once

#include "core/Types.h"

#include <array>
#include <span>

namespace vd::game {

struct SpellInfo;
struct SpellDetails;
struct Entity;

// SpellModOp (vMaNGOS Spells/SpellDefines.h): what a talent or an aura modifies in the spells it names.
enum class SpellModOp : u8 {
    Damage = 0,
    Duration = 1,
    Threat = 2,
    AttackPower = 3,
    Charges = 4,
    Range = 5,
    Radius = 6,
    CritChance = 7,
    AllEffects = 8,
    NotLoseCastingTime = 9,
    CastingTime = 10,
    Cooldown = 11,
    Speed = 12,
    Cost = 14,
    CritDamageBonus = 15,
    ResistMissChance = 16,
    JumpTargets = 17,
    ChanceOfSuccess = 18,
    ActivationTime = 19,
    EffectPastFirst = 20,
    GlobalCooldown = 21,
    Dot = 22,
    Haste = 23,
    SpellBonusDamage = 24,
    MultipleValue = 27,
};
constexpr u32 kSpellModOps = 29;  // MAX_SPELLMOD
constexpr u32 kSpellModBits = 64; // SpellFamilyFlags

// What a spell modifier looks at in a spell.
struct SpellModTarget {
    u32 family = 0;      // SpellFamilyName
    u64 familyFlags = 0; // SpellFamilyFlags
    u32 attributesEx3 = 0;
};
SpellModTarget modTarget(const SpellInfo& spell);
SpellModTarget modTarget(const SpellDetails& spell);

// Phase 13 (spells): the player's spell modifiers (SMSG_SET_FLAT_SPELL_MODIFIER /
// SMSG_SET_PCT_SPELL_MODIFIER), the two tables [bit][op] of WoW.exe 1.12.1 and its rule to apply them (read
// from the client, which the server's Player::ApplySpellMod matches): a spell takes the modifiers of every
// bit set in its SpellFamilyFlags when its SpellFamilyName is the player's class family (ChrClasses.dbc
// m_spellClassSet) and it does not ignore caster modifiers (SPELL_ATTR_EX3_IGNORE_CASTER_MODIFIERS); the flat
// values add up, the percentages add up over 100, and the value becomes (value + flat) * percent / 100. The
// server sends each bit's total, never a change. The client keeps them across teleports; a new login sends
// them again (before SMSG_LOGIN_VERIFY_WORLD). Main thread.
class SpellMods {
public:
    void reset();
    // The family the player's class casts in (0: none known yet: nothing applies).
    void setClassFamily(u32 family);
    u32 classFamily() const { return m_classFamily; }

    // SMSG_SET_FLAT_SPELL_MODIFIER (`flat`) or SMSG_SET_PCT_SPELL_MODIFIER. False when malformed.
    bool handle(bool flat, std::span<const u8> payload);
    void set(bool flat, u32 bit, u32 op, i32 value);
    i32 flat(u32 bit, SpellModOp op) const;
    i32 percent(u32 bit, SpellModOp op) const;
    // Bumped whenever a value changes (tooltips and the action bar rebuild then).
    u32 version() const { return m_version; }

    // The flat and percent (100 = unchanged) modifiers of `op` for the spell; false (flat 0, percent 100)
    // when none applies.
    bool modifiers(const SpellModTarget& spell, SpellModOp op, i32& flat, i32& percent) const;
    // The value with the modifiers: integers truncated towards zero as the client's, floats exact.
    i32 apply(const SpellModTarget& spell, SpellModOp op, i32 value) const;
    f32 apply(const SpellModTarget& spell, SpellModOp op, f32 value) const;

private:
    static u32 index(u32 bit, u32 op) { return bit * kSpellModOps + op; }

    std::array<i32, kSpellModBits * kSpellModOps> m_flat{};
    std::array<i32, kSpellModBits * kSpellModOps> m_percent{};
    u32 m_classFamily = 0;
    u32 m_version = 1;
};

// The family of a class's spells in 1.12 (ChrClasses.dbc m_spellClassSet: warrior 4, paladin 10, hunter 9,
// rogue 8, priest 6, shaman 11, mage 3, warlock 5, druid 7), for when the table is not at hand; 0 unknown.
u32 classSpellFamily(u32 cls);

// The numbers WoW.exe computes for a spell's tooltip and buttons, with the modifiers (`mods` may be null) and
// the caster's fields (`me` may be null: no level or school scaling):
// - power cost: ManaCost plus ManaCostPercentage of the base mana, plus the caster's
//   UNIT_FIELD_POWER_COST_MODIFIER of the school, times 1 + its UNIT_FIELD_POWER_COST_MULTIPLIER, rounded,
//   then SPELLMOD_COST;
// - cast time: SpellCastTimes' base, then SPELLMOD_CASTING_TIME, times UNIT_MOD_CAST_SPEED unless the spell
//   is a ranged or trade one (Attributes 0x30), rounded;
// - range: SpellRange's maximum, then SPELLMOD_RANGE; cooldown: RecoveryTime and CategoryRecoveryTime, then
//   SPELLMOD_COOLDOWN; duration: SpellDuration's base, then SPELLMOD_DURATION.
u32 modifiedPowerCost(const SpellModTarget& spell, u32 school, u32 powerType, u32 powerCost,
                      u32 powerCostPercent, const Entity* me, const SpellMods* mods);
u32 modifiedCastTime(const SpellModTarget& spell, u32 attributes, u32 castTimeMs, const Entity* me,
                     const SpellMods* mods);
// `details` with its cost, cast time, range, cooldowns and duration as the client shows them (in place).
void applySpellMods(SpellDetails& details, const Entity* me, const SpellMods* mods);

} // namespace vd::game
