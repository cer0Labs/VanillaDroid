#pragma once

#include "core/Types.h"

#include <span>

namespace vd::game {

struct ActionUsable;
struct SpellDetails;

// Phase 20 (smart, docs/MOBILE_CONTROLS.md §6.3): the spells the touch cluster's reactive button offers:
// those that become usable through a state of the fight, read from Spell.dbc rather than listed by hand: an
// aura state of the caster or the target (CasterAuraState / TargetAuraState: Revenge, Execute, Riposte,
// Mongoose Bite, Counterattack, Hammer of Wrath; not Judgement, whose state is the paladin's own seal), and
// the warrior's Overpower (the combo point the server gives a warrior when its target dodges: a rage spell
// that needs combo points). Plain functions; no allocation.

bool isReactiveSpell(const SpellDetails& spell);

// Whether a reactive spell shows now: usable by its state (IsUsableAction; short of power still counts: it
// shows blue) and not cooling down beyond the global cooldown (`cooldownLeftMs` its cooldown's time left, the
// global one's included; `globalLeftMs` the global cooldown's).
bool reactiveReady(const ActionUsable& usable, u32 cooldownLeftMs, u32 globalLeftMs);

// A reactive spell usable now and its cooldown (the longest of its own and its category's).
struct ReactiveEntry {
    u32 spellId = 0;
    u32 cooldownMs = 0;
};
// Adds `entry` to the first `count` of `list` in the order the button shows them (the longest cooldown first:
// Overpower before Execute; the earlier one first between equals). Nothing when `list` is full.
void insertReactive(std::span<ReactiveEntry> list, u32& count, const ReactiveEntry& entry);

} // namespace vd::game
