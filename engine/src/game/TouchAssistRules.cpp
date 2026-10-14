#include "game/TouchAssistRules.h"

#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/SpellText.h"
#include "game/TouchContext.h"

#include <algorithm>
#include <cmath>

namespace vd::game {

namespace {

constexpr u32 kAttrOnNextSwing = 0x00000004 | 0x00000400; // SPELL_ATTR_ON_NEXT_SWING_1 / _2
constexpr u32 kAttrExInitiatesCombat = 0x00000200;        // SPELL_ATTR_EX_INITIATES_COMBAT
constexpr u32 kAttrEx2CombatPostCast = 0x00100000;        // SPELL_ATTR_EX2_INITIATE_COMBAT_POST_CAST
constexpr u32 kFamilyPaladin = 10;                        // SPELLFAMILY_PALADIN
// SpellEntry::IsSealSpell: CF_PALADIN_SEAL_OF_THE_CRUSADER, CF_PALADIN_SEAL_OF_COMMAND, CF_PALADIN_SEALS.
constexpr u64 kSealFlags = 0x00000200ull | 0x02000000ull | 0x08000000ull;
// SpellCastResult: the cooldown ends any moment.
constexpr u32 kNotReady = 60;        // SPELL_FAILED_NOT_READY
constexpr u32 kSpellInProgress = 97; // SPELL_FAILED_SPELL_IN_PROGRESS

bool reached(u32 nowMs, u32 untilMs) {
    return static_cast<i32>(nowMs - untilMs) >= 0;
}

bool warriorStance(u32 form) {
    return form == kFormBattleStance || form == kFormDefensiveStance || form == kFormBerserkerStance;
}

bool druidAnimalForm(u32 form) {
    return form == kFormCat || form == kFormTravel || form == kFormAquatic || form == kFormBear ||
           form == kFormDireBear || form == kFormMoonkin;
}

} // namespace

AttackStart attackStartOf(const SpellDetails& spell, bool assist) {
    // The stealth openers have both flags: after the cast (the attack would break the stealth first).
    if ((spell.attributesEx2 & kAttrEx2CombatPostCast) != 0) {
        return AttackStart::AfterCast;
    }
    if ((spell.attributesEx & kAttrExInitiatesCombat) != 0) {
        return AttackStart::AtCast;
    }
    if (!assist) {
        return AttackStart::None;
    }
    if ((spell.attributes & kAttrOnNextSwing) != 0) {
        return AttackStart::AtCast; // Heroic Strike, Raptor Strike: they wait for a swing that never comes
    }
    if (spell.spellFamily == kFamilyPaladin && (spell.familyFlags & kSealFlags) != 0) {
        return AttackStart::AtCast; // a seal works through the swings
    }
    return AttackStart::None;
}

// --- The repeat ---

bool TouchRepeatGate::ready(u32 nowMs) const {
    if (m_blocked && !reached(nowMs, m_blockedUntilMs)) {
        return false;
    }
    if (m_spell != 0 && !reached(nowMs, m_sentMs + kTouchInFlightMs)) {
        return false; // still on its way
    }
    return !m_sentAny || reached(nowMs, m_sentMs + kTouchRepeatEveryMs);
}

void TouchRepeatGate::sent(u32 spellId, u32 nowMs) {
    m_spell = spellId;
    m_sentMs = nowMs;
    m_sentAny = true;
}

void TouchRepeatGate::answered(u32 spellId) {
    if (spellId == m_spell) {
        m_spell = 0;
    }
}

void TouchRepeatGate::refused(u32 spellId, u32 reason, u32 nowMs) {
    if (spellId != m_spell) {
        return; // another button's (or the classic bars')
    }
    m_spell = 0;
    m_blocked = true;
    m_blockedUntilMs = nowMs + (reason == kNotReady || reason == kSpellInProgress ? kTouchNotReadyMs : kTouchRefusedMs);
}

// --- The queue ---

bool TouchSpellQueue::offer(u32 spellId, u32 blockedMs, u32 nowMs) {
    if (spellId == 0 || blockedMs == 0 || blockedMs > kTouchQueueWindowMs) {
        return false;
    }
    m_spell = spellId; // a later press replaces an earlier one
    m_untilMs = nowMs + blockedMs + kTouchQueueGraceMs;
    return true;
}

u32 TouchSpellQueue::due(u32 blockedMs, u32 nowMs) {
    if (m_spell == 0) {
        return 0;
    }
    if (blockedMs == 0) {
        const u32 spell = m_spell;
        m_spell = 0;
        return spell;
    }
    if (reached(nowMs, m_untilMs)) {
        m_spell = 0; // its moment passed (a cast pushed back, a lag)
    }
    return 0;
}

// --- The forms ---

TouchFormChange touchFormChange(const SpellDetails& spell, u32 current, std::span<const ShapeshiftForm> forms,
                                const AbilityData& data, bool stances, bool druidForms) {
    TouchFormChange change;
    if (data.castableInForm(spell, current)) {
        return change;
    }
    if (stances && (current == 0 || warriorStance(current))) {
        // The first stance of the stance bar's order (by level: Battle, Defensive, Berserker) that takes it.
        for (const ShapeshiftForm& f : forms) {
            if (warriorStance(f.form) && f.form != current && data.castableInForm(spell, f.form)) {
                change.form = f.form;
                change.spellId = f.spellId;
                return change;
            }
        }
        return change;
    }
    if (druidForms && druidAnimalForm(current) && data.castableInForm(spell, 0)) {
        for (const ShapeshiftForm& f : forms) {
            // Dire Bear Form keeps Bear Form's button: the form in use is either.
            if (f.form == current || (current == kFormDireBear && f.form == kFormBear)) {
                change.leaveSpell = f.spellId;
                change.form = 0;
                return change;
            }
        }
    }
    return change;
}

void TouchFormSwitch::start(u32 spellId, u64 target, u32 form, u32 nowMs) {
    m_spell = spellId;
    m_target = target;
    m_form = form;
    m_untilMs = nowMs + kTouchFormWaitMs;
}

TouchFormSwitch::Due TouchFormSwitch::poll(u32 form, u32 nowMs) {
    Due due;
    if (m_spell == 0) {
        return due;
    }
    if (form == m_form) {
        due = {m_spell, m_target};
        m_spell = 0;
    } else if (reached(nowMs, m_untilMs)) {
        m_spell = 0; // the form never came (refused, on its cooldown): forgotten
    }
    return due;
}

// --- The site ---

TouchSite touchSiteOf(u32 instanceType) {
    switch (instanceType) {
    case 0: return TouchSite::World;
    case 1: return TouchSite::Dungeon;
    case 2: return TouchSite::Raid;
    case 3: return TouchSite::Battleground;
    default: return TouchSite::None;
    }
}

i32 touchTemplateForSite(const TouchLayout& layout, TouchSite site) {
    if (site == TouchSite::None) {
        return -1;
    }
    for (u32 i = 0; i < layout.count && i < kMaxTouchTemplates; ++i) {
        if (layout.templates[i].site == static_cast<u8>(site)) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 TouchSiteSwitch::update(TouchSite site, const TouchLayout& layout) {
    if (site == m_site || site == TouchSite::None) { // the same place, or one not known (a map without its row)
        return -1;
    }
    m_site = site;
    const i32 index = touchTemplateForSite(layout, site);
    return index >= 0 && static_cast<u32>(index) != layout.active ? index : -1;
}

// --- The aim ---

glm::vec2 touchAimOffset(f32 nx, f32 ny, glm::vec2 forward, glm::vec2 right, f32 range) {
    const f32 length = std::sqrt(nx * nx + ny * ny);
    if (length > 1.0f) {
        nx /= length;
        ny /= length;
    }
    // Up on the screen is away from the camera.
    return (right * nx - forward * ny) * range;
}

} // namespace vd::game
