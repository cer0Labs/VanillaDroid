#pragma once

#include "core/Types.h"
#include "game/Combat.h"

#include <span>

namespace vd::game {

// Phase 15 (pets): the damage numbers over the player's and the pet's portraits (ROADMAP F15.11), as the 1.12
// client makes them: WoW.exe fires UNIT_COMBAT (event 182, "%s%s%s%d%d": unit, event, flags, amount, type)
// from the packets below only, through 0x494600, and CombatFeedback.lua turns each into the frame's hit
// indicator (PlayerHitIndicator, PetHitIndicator: TargetFrame has none in 1.12).
// - SMSG_ATTACKERSTATEUPDATE (at the swing's impact, 0x6243E0 -> 0x4946D0): the victim state's name
//   (MISS, WOUND, DODGE, PARRY, INTERRUPT, BLOCK, EVADE, IMMUNE, DEFLECT), the total damage, the school,
//   the HitInfo bits;
// - SMSG_SPELLNONMELEEDAMAGELOG (0x5E85E0), SMSG_PERIODICAURALOG's damage (0x626F87): WOUND with crit /
//   absorb / block / resist bits (periodic: absorb / resist only), not for spells hidden from the log
//   (attributes 0x100);
// - SMSG_SPELLDAMAGESHIELD (0x5E84E0): WOUND on whoever struck the shield's bearer;
// - SMSG_ENVIRONMENTALDAMAGELOG (0x624F30): WOUND with the environment's school, absorb / resist bits;
// - SMSG_SPELLHEALLOG (0x5E89C0, crit) and periodic heals (0x627017, no crit): HEAL;
// - SMSG_SPELLLOGMISS (0x5E7E00) and SMSG_SPELL_GO's misses (0x6E7A70, also at a missile's impact
//   0x61D870): the miss's name (MISS, RESIST, DODGE, PARRY, BLOCK, EVADE, IMMUNE, DEFLECT, ABSORB,
//   REFLECT), a reflect also on the caster; hidden spells none.
// Never for energize (SPELLENERGIZELOG, periodic mana), drains, SPELLORDAMAGE_IMMUNE or PROCRESIST:
// CombatFeedback's ENERGIZE branch is never reached in 1.12.

enum class UnitCombatEvent : u8 {
    None,
    Miss,
    Wound,
    Dodge,
    Parry,
    Interrupt,
    Block,
    Evade,
    Immune,
    Deflect,
    Resist,
    Absorb,
    Reflect,
    Heal
};
// UNIT_COMBAT's flags string (0x494600: with an amount CRITICAL / GLANCING / CRUSHING, without one ABSORB /
// BLOCK / RESIST, from the HitInfo bits 0x80 / 0x4000 / 0x8000 and 0x20 / 0x800 / 0x40).
enum class UnitCombatFlag : u8 { None, Critical, Glancing, Crushing, Absorb, Block, Resist };

struct UnitCombat {
    u64 unit = 0; // who it is about (the frames of that unit react: "player", "pet")
    UnitCombatEvent event = UnitCombatEvent::None;
    UnitCombatFlag flag = UnitCombatFlag::None;
    i32 amount = 0;
    u32 type = 0; // the school (0 physical)
};

// Whether a spell is hidden from the combat log (Spell.dbc attributes 0x100): its damage and misses fire
// nothing.
struct SpellLogFilter {
    bool (*hidden)(const void* context, u32 spellId) = nullptr;
    const void* context = nullptr;
    bool operator()(u32 spellId) const { return spellId != 0 && hidden && hidden(context, spellId); }
};

// The UNIT_COMBATs WoW.exe fires for a combat event (at most 2: a reflect fires on the caster too). Returns
// how many were written to `out`.
u32 unitCombats(const CombatEvent& e, const SpellLogFilter& hiddenSpell, std::span<UnitCombat> out);

// What a hit indicator shows (CombatFeedback_OnCombatEvent's text, color and height).
enum class FeedbackColor : u8 { White, Yellow, Green };
struct FeedbackView {
    bool shown = false;
    i32 amount = 0; // the number, when `word` is None
    UnitCombatEvent word =
        UnitCombatEvent::None; // CombatFeedbackText[event] (MISS, ABSORB...): its name instead
    FeedbackColor color = FeedbackColor::White;
    f32 height = 0.0f; // the text's height in interface units (CombatFeedback_Initialize's 30, scaled)
    f32 alpha = 0.0f;
};

// One frame's hit indicator (CombatFeedback.lua): the last UNIT_COMBAT shows, fading in for 0.2 s, held 0.7
// s, fading out 0.3 s (COMBATFEEDBACK_FADEINTIME / HOLDTIME / FADEOUTTIME). No allocation.
class CombatFeedback {
public:
    static constexpr u32 kFadeInMs = 200;
    static constexpr u32 kHoldMs = 700;
    static constexpr u32 kFadeOutMs = 300;
    static constexpr f32 kFontHeight =
        30.0f; // CombatFeedback_Initialize(PlayerHitIndicator / PetHitIndicator, 30)

    // CombatFeedback_OnCombatEvent.
    void onCombatEvent(const UnitCombat& c, u32 nowMs);
    // CombatFeedback_OnUpdate at `nowMs`: hidden once its time is over.
    FeedbackView view(u32 nowMs) const;
    void clear() { m_shown = false; }

private:
    bool m_shown = false;
    u32 m_startMs = 0;
    FeedbackView m_view;
};

} // namespace vd::game
