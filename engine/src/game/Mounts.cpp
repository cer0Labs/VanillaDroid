#include "game/Mounts.h"

#include "game/SpellText.h"

namespace vd::game {
namespace {

constexpr u32 kEffectApplyAura = 6;    // SPELL_EFFECT_APPLY_AURA
constexpr u32 kAuraModShapeshift = 36; // SPELL_AURA_MOD_SHAPESHIFT (misc: the form)
constexpr u32 kFormAquatic = 4;        // FORM_AQUA

// Whether `o` can be used where the player is now.
bool usableNow(const TravelOption& o, const TravelState& s) {
    if (!o.usable || (o.outdoorsOnly && s.indoors) || (o.notInCombat && s.inCombat)) {
        return false;
    }
    // Aquatic Form only in water; no mount in it (Spell::CheckCast: SPELL_FAILED_ONLY_ABOVEWATER).
    if (o.swimming != s.swimming && (o.swimming || o.mount)) {
        return false;
    }
    // Unit::IsInDisallowedMountForm: no form, the warrior's stances, Shadowform and Stealth let the player mount; any
    // other form does not (SPELL_FAILED_NOT_SHAPESHIFT).
    const bool formAllowed = s.form == 0 || s.form == 17 || s.form == 18 || s.form == 19 || s.form == 28 || s.form == 30;
    return !(o.notShapeshifted && !formAllowed);
}

} // namespace

bool isMountSpell(const SpellDetails& spell, u32* speed) {
    bool mount = false;
    u32 fastest = 0;
    for (const SpellEffectData& e : spell.effects) {
        if (e.effect != kEffectApplyAura) {
            continue;
        }
        if (e.aura == kAuraMounted) {
            mount = true;
        } else if (e.aura == kAuraModMountedSpeed) {
            const i32 points = e.basePoints + (e.dieSides > 0 ? e.dieSides : 0);
            fastest = points > 0 ? static_cast<u32>(points) : 0u;
        }
    }
    if (speed) {
        *speed = mount ? fastest : 0;
    }
    return mount;
}

TravelOption travelOptionOf(const SpellDetails& spell, const TouchAction& action) {
    TravelOption o;
    o.action = action;
    o.mount = isMountSpell(spell, &o.speed);
    for (const SpellEffectData& e : spell.effects) {
        if (e.effect == kEffectApplyAura && e.aura == kAuraModShapeshift && e.miscValue > 0) {
            o.form = static_cast<u32>(e.miscValue);
        }
    }
    o.outdoorsOnly = (spell.attributes & kSpellAttrOnlyOutdoors) != 0;
    o.notInCombat = (spell.attributes & kSpellAttrNotInCombat) != 0;
    o.notShapeshifted = (spell.attributes & kSpellAttrNotShapeshift) != 0;
    o.swimming = o.form == kFormAquatic;
    return o;
}

i32 chooseMount(std::span<const TravelOption> options, const TouchAction& favorite) {
    i32 best = -1;
    for (u32 i = 0; i < options.size(); ++i) {
        const TravelOption& o = options[i];
        if (!o.mount || !o.usable) {
            continue;
        }
        if (!favorite.empty() && o.action.kind == favorite.kind && o.action.id == favorite.id) {
            return static_cast<i32>(i);
        }
        if (best < 0) {
            best = static_cast<i32>(i);
            continue;
        }
        const TravelOption& b = options[static_cast<u32>(best)];
        const bool spell = o.action.kind == TouchActionKind::Spell,
                   bestSpell = b.action.kind == TouchActionKind::Spell;
        if (o.speed > b.speed || (o.speed == b.speed && spell && !bestSpell)) {
            best = static_cast<i32>(i);
        }
    }
    return best;
}

TravelChoice chooseTravel(const TravelState& state, std::span<const TravelOption> options,
                          const TouchAction& favorite) {
    TravelChoice choice;
    if (state.mounted) { // the button gets off (the mount's aura cancelled, as BuffFrame does)
        choice.action = TouchAction::spell(state.auraSpell, kTouchFixedRank);
        choice.active = true;
        return choice;
    }
    const auto pick = [&](const TravelOption& o, bool usable) {
        choice.action = o.action;
        choice.usable = usable;
        choice.active = o.active || (o.form != 0 && o.form == state.form);
        return choice;
    };
    // Swimming: Aquatic Form.
    for (const TravelOption& o : options) {
        if (o.swimming && usableNow(o, state)) {
            return pick(o, true);
        }
    }
    const i32 mount = chooseMount(options, favorite);
    if (mount >= 0 && usableNow(options[static_cast<u32>(mount)], state)) {
        return pick(options[static_cast<u32>(mount)], true);
    }
    // The forms in the caller's order (Travel Form before Cat Form: indoors only the cat is left).
    for (const TravelOption& o : options) {
        if (!o.mount && !o.swimming && usableNow(o, state)) {
            return pick(o, true);
        }
    }
    // Nothing can be used here: the mount (or the first form) faint, so that the button does not come and go.
    if (mount >= 0) {
        return pick(options[static_cast<u32>(mount)], false);
    }
    for (const TravelOption& o : options) {
        if (!o.mount && !o.swimming) {
            return pick(o, false);
        }
    }
    for (const TravelOption& o : options) {
        if (o.mount && !o.usable) {
            return pick(o, false); // a mount the player cannot ride yet (its level or riding skill)
        }
    }
    return choice;
}

} // namespace vd::game
