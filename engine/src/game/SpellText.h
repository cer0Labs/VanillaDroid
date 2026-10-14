#pragma once

#include "core/Types.h"
#include "game/DbcIndex.h"
#include "game/GameData.h"
#include "game/SpellMods.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vd::game {

// The numbers of one effect of a spell (Spell.dbc, 1.12.1 client layout).
struct SpellEffectData {
    u32 effect = 0; // SpellEffects (0 = unused)
    u32 aura = 0;   // EffectApplyAuraName
    i32 basePoints = 0;
    i32 dieSides = 0;
    u32 baseDice = 0;
    f32 dicePerLevel = 0.0f;
    f32 realPointsPerLevel = 0.0f;
    u32 amplitudeMs = 0; // periodic auras: time between ticks
    u32 radiusIndex = 0; // SpellRadius.dbc
    u32 chainTargets = 0;
    f32 multipleValue = 0.0f;
    i32 miscValue = 0;
    u32 triggerSpell = 0; // the spell a trainer's spell teaches, a proc...
    f32 pointsPerComboPoint = 0.0f;
    f32 damageMultiplier = 0.0f;
    u32 mechanic = 0; // phase 20 (pets): EffectMechanic (SpellDefines.h' Mechanics: 5 fear, 12 stun...), 0 none
};

// What the spell book, trainers and talents show about a spell beyond GameData's SpellInfo: its texts, levels
// and effect numbers (Spell.dbc 1.12.1, field numbers as GameData explains).
struct SpellDetails {
    u32 id = 0;
    std::string_view name;
    std::string_view rank;
    std::string_view description; // raw, with $ tokens (spellDescription() resolves them)
    std::string_view auraText;    // the aura's tooltip, raw
    u32 attributes = 0;
    u32 attributesEx = 0;
    u32 attributesEx2 = 0;
    u32 stances = 0;         // forms it can be cast in (bit form - 1)
    u32 stancesNot = 0;      // forms it cannot be cast in
    u32 casterAuraState = 0; // AuraState the caster needs (UNIT_FIELD_AURASTATE bit state - 1), 0 none
    u32 targetAuraState = 0;
    u32 maxLevel = 0, baseLevel = 0, spellLevel = 0;
    u32 durationMs = 0;
    u32 procChance = 0, procCharges = 0, stackAmount = 0, maxTargets = 0, maxTargetLevel = 0;
    u32 manaPerSecond = 0;
    u32 castTimeMs = 0;
    f32 minRange = 0.0f, maxRange = 0.0f;
    u32 powerType = 0;
    u32 powerCost = 0;
    u32 powerCostPercent = 0;
    u32 recoveryMs = 0, categoryRecoveryMs = 0;
    std::array<SpellEffectData, 3> effects{};
    u32 dispel = 0;     // parity (auras): SpellDispelType.dbc (1 magic, 2 curse, 3 disease, 4 poison), 0 none
    u32 activeIcon = 0; // SpellIcon.dbc of its aura while active (activeIconID), 0 none
    u32 castUi = 0;     // phase 13 (professions): the CraftFrame list it is in (Enchanting, Beast Training), 0 none
    // Phase 13 (spells): what the spell modifiers look at (game/SpellMods) and the school of its power cost.
    u32 school = 0;
    u32 attributesEx3 = 0;
    u32 spellFamily = 0; // SpellFamilyName
    u64 familyFlags = 0; // SpellFamilyFlags
    u32 auraInterruptFlags = 0; // phase 20 (smart): AuraInterruptFlags (0x2 AURA_INTERRUPT_DAMAGE_CANCELS: Polymorph...)
    u32 mechanic = 0;           // phase 20 (pets): Mechanic, the whole spell's (an effect's own: effects[i].mechanic)

    bool passive() const { return (attributes & 0x40) != 0; }       // SPELL_ATTR_PASSIVE
    bool hidden() const { return (attributes & 0x80) != 0; }        // SPELL_ATTR_DO_NOT_DISPLAY
    bool tradeSpell() const { return (attributes & 0x20) != 0; }    // SPELL_ATTR_IS_TRADESKILL: a recipe
    bool channeled() const { return (attributesEx & 0x44) != 0; }   // SPELL_ATTR_EX_CHANNELED_1 / _2
    bool autoRepeat() const { return (attributesEx2 & 0x20) != 0; } // SPELL_ATTR_EX2_AUTO_REPEAT
    // Finishing moves, and Overpower (the server gives warriors a combo point when the target dodges).
    bool needsComboPoints() const {
        return (attributesEx & 0x00500000) != 0;
    } // SPELL_ATTR_EX_REQ_*COMBO_POINTS*
};

// How a description reads for this player.
struct SpellTextContext {
    u32 casterLevel = 0; // 0: the spell's base numbers (no per-level growth)
    u8 gender = 0;       // 0 male, 1 female ($g)
    // Phase 13 (spells): the player's spell modifiers (null: none). WoW.exe's description parser applies them to
    // $s / $m / $o (SPELLMOD_ALL_EFFECTS, then _DAMAGE or _DOT for damage and an aura's own op), $d (_DURATION),
    // $a (_RADIUS), $r (_RANGE), $x (_JUMP_TARGETS), $t (_ACTIVATION_TIME), $h (_CHANCE_OF_SUCCESS), $n (_CHARGES)
    // and $e (_MULTIPLE_VALUE).
    const SpellMods* mods = nullptr;
};

// Spell texts with their numbers (phase 8): an index of Spell.dbc and the side tables its tooltips read
// (SpellDuration, SpellRadius, SpellRange, SpellCastTimes, SpellIcon). The DBCs stay in the mapped db pack.
class SpellText {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_spells.empty(); }

    bool details(u32 spellId, SpellDetails& out) const;
    bool has(u32 spellId) const { return record(spellId) != kMissing; }
    std::string_view name(u32 spellId) const;
    std::string_view rank(u32 spellId) const;
    // assetPathHash of a SpellIcon.dbc entry's texture ("Interface\\Icons\\X.blp"), 0 if none.
    u64 iconHash(u32 spellIconId) const;
    // Yards of a SpellRadius.dbc entry.
    f32 radius(u32 radiusIndex) const;
    // Parity (auras): the spell's dispel type (Spell.dbc Dispel: a SpellDispelType.dbc id), 0 for none or an unknown
    // spell; no texts read (every frame, for the debuff borders). And a type's name ("Magic", "Curse", "Disease",
    // "Poison"...), empty for none.
    u32 dispelType(u32 spellId) const;
    std::string_view dispelName(u32 dispelType) const;

    // The text with its $ tokens resolved for `spellId` ($s1, $d, $o1, $t1, $m1, $a1, $h, $n, $x1, $u, $i,
    // $q1, $e1, $b1, $f1, $v, $r, $z, $12345s1 of another spell, $*N; and $/N; scaling, $lsingular:plural;,
    // $gmale:female;). Appends to `out`. Ranges and durations use the client's texts (core/Text).
    void resolve(u32 spellId, std::string_view text, const SpellTextContext& context, std::string& out) const;

private:
    static constexpr u32 kMissing = 0xFFFFFFFFu;
    u32 record(u32 spellId) const;
    // A number for a token letter of `spellId` (effect `index` 0..2): false when the letter is unknown.
    bool tokenValue(u32 spellId, char letter, u32 index, const SpellTextContext& context, f64& value,
                    f64& max) const;
    u32 durationMs(u32 record) const;

    DbcTable m_spells;
    DbcTable m_durations;
    DbcTable m_radii;
    DbcTable m_ranges;
    DbcTable m_castTimes;
    DbcTable m_icons;
    DbcTable m_dispelTypes;
};

// The spell's description resolved for the player (the tooltip's last paragraph): replaces `out`. False when
// the spell is unknown to the client (out left empty). Item tooltips can call it for their "Use:" / "Equip:"
// spells.
bool spellDescription(const SpellText& text, u32 spellId, std::string& out,
                      const SpellTextContext& context = {});
// The aura's tooltip text (what a buff or debuff does), resolved the same way.
bool auraDescription(const SpellText& text, u32 spellId, std::string& out,
                     const SpellTextContext& context = {});

// Tooltip lines of a spell with the client's texts (core/Text), into `out` (NUL-terminated, cut to `size`).
// Each returns the characters written; 0 when the line does not apply (no cost, instant without cooldown...).
// MANA_COST, RAGE_COST, ENERGY_COST, FOCUS_COST (+ _PER_TIME): "35 Mana", "10 Rage"; "8% of base Mana".
usize formatSpellCost(char* out, usize size, const SpellDetails& spell);
// SPELL_RANGE: "30 yd range", "8-35 yd range"; "Melee Range"; 0 for spells on the caster.
usize formatSpellRange(char* out, usize size, const SpellDetails& spell);
// SPELL_CAST_TIME_*: "Instant cast" (mana) / "Instant", "1.5 sec cast", "Channeled"; passives: 0.
usize formatSpellCastTime(char* out, usize size, const SpellDetails& spell);
// SPELL_RECAST_TIME_*: "8 sec cooldown", "30 min cooldown".
usize formatSpellCooldown(char* out, usize size, const SpellDetails& spell);
// A duration as the client's descriptions write it ($d, INT_SPELL_DURATION_*): "30 sec", "10 min", "2 hrs".
usize formatSpellDuration(char* out, usize size, u32 ms);

} // namespace vd::game
