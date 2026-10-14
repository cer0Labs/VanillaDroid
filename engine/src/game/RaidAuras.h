#pragma once

#include "core/Types.h"

#include <array>

namespace vd::game {

// Phase 16 (pullouts): which auras of a raid member the pullout frames list. Blizzard_RaidUI's
// RaidPullout_Update calls RefreshBuffs(button, showBuffs, "raidN"): its four buttons take UnitBuff(unit, i,
// SHOW_CASTABLE_BUFFS) with "Show Buffs" and UnitDebuff(unit, i, SHOW_DISPELLABLE_DEBUFFS) otherwise, i
// = 1..4. The C side of those two calls, read from WoW.exe's disassembly (read only): UnitBuff 0x519500,
// UnitDebuff 0x5198F0. Pure functions on Spell.dbc's numbers.

// The Spell.dbc fields the filters read.
struct RaidAuraSpell {
    u32 attributes = 0;
    u32 attributesEx = 0;
    u32 attributesEx3 = 0;
    std::array<u32, 3> effects{};         // SpellEffects
    std::array<u32, 3> effectAuras{};     // EffectApplyAuraName
    std::array<u32, 3> implicitTargetA{}; // SpellTarget
    std::array<i32, 3> effectMisc{};      // EffectMiscValue
};

// Spell.dbc numbers WoW.exe tests (named as vMaNGOS' SpellDefines.h / SpellAuraDefines.h).
constexpr u32 kSpellAttrDoNotDisplay = 0x80;          // SPELL_ATTR_DO_NOT_DISPLAY
constexpr u32 kSpellAttrExNoAuraIcon = 0x10000000;    // SPELL_ATTR_EX_NO_AURA_ICON
constexpr u32 kSpellAttrEx3HideInRaidFilter = 0x4000; // SPELL_ATTR_EX3_HIDE_IN_RAID_FILTER
constexpr u32 kSpellAttrPassive = 0x40;               // SPELL_ATTR_PASSIVE
constexpr u32 kSpellEffectDispel = 38;                // SPELL_EFFECT_DISPEL (misc value: the dispel type)
constexpr u32 kTargetUnitCaster = 1;                  // TARGET_UNIT_CASTER
constexpr u32 kTargetUnitEnemy = 6;                   // TARGET_UNIT_ENEMY

// Whether UnitBuff / UnitDebuff list the aura at all (0x519860; the target and party frames' auras go through
// it too): not SPELL_ATTR_DO_NOT_DISPLAY, not SPELL_ATTR_EX_NO_AURA_ICON, and no tracking aura
// (SPELL_AURA_TRACK_CREATURES 44, _RESOURCES 45, _STEALTHED 151). (Its RequiredAuraVision test, the spirit
// world's auras, is left out.)
bool raidAuraListed(const RaidAuraSpell& spell);

// SHOW_CASTABLE_BUFFS: a buff the player could cast on the member (0x4B3870): a spell it knows (`known`), not
// passive, without SPELL_ATTR_EX3_HIDE_IN_RAID_FILTER, its first effect not on the caster
// (TARGET_UNIT_CASTER), and none of its effects an area aura (SPELL_EFFECT_PERSISTENT_AREA_AURA 27,
// APPLY_AREA_AURA_PARTY 35, _PET 119, _FRIEND 128, _ENEMY 129). Another rank than the ones the player knows
// does not count.
bool raidCastableBuff(const RaidAuraSpell& spell, bool known);

// SHOW_DISPELLABLE_DEBUFFS: the dispel types (bit = SpellDispelType id: 1 magic, 2 curse, 3 disease, 4
// poison) a spell teaches the player to remove: the misc value of each SPELL_EFFECT_DISPEL effect not aimed
// at an enemy (TARGET_UNIT_ENEMY: Purge, the hunters' Tranquilizing Shot). WoW.exe adds them as each spell is
// learned (0x4B25B0, list 0xB71118) and UnitDebuff's filter keeps the debuffs whose Dispel is in the list
// (0x4B3920); a debuff without a type never passes it.
u32 raidDispelTypes(const RaidAuraSpell& spell);
bool raidDispellable(u32 dispelTypes, u32 debuffDispel);

} // namespace vd::game
