#include "game/ReactiveSpells.h"

#include "game/ActionBar.h"
#include "game/SpellText.h"

namespace vd::game {
namespace {

constexpr u32 kPowerRage = 1;
// AURA_STATE_JUDGEMENT: a seal the paladin put up himself, not something the fight gives (Judgement is a
// button of the rotation, not a reaction).
constexpr u32 kAuraStateJudgement = 5;
constexpr u32 kGlobalSlackMs = 100; // a cooldown no longer than the global one (and this) is the global one

} // namespace

bool isReactiveSpell(const SpellDetails& spell) {
    if (spell.passive()) {
        return false;
    }
    const bool caster = spell.casterAuraState != 0 && spell.casterAuraState != kAuraStateJudgement;
    return caster || spell.targetAuraState != 0 ||
           (spell.needsComboPoints() && spell.powerType == kPowerRage);
}

bool reactiveReady(const ActionUsable& usable, u32 cooldownLeftMs, u32 globalLeftMs) {
    return (usable.usable || usable.noMana) && cooldownLeftMs <= globalLeftMs + kGlobalSlackMs;
}

void insertReactive(std::span<ReactiveEntry> list, u32& count, const ReactiveEntry& entry) {
    if (count >= list.size()) {
        return;
    }
    u32 at = count++;
    for (; at > 0 && list[at - 1].cooldownMs < entry.cooldownMs; --at) {
        list[at] = list[at - 1];
    }
    list[at] = entry;
}

} // namespace vd::game
