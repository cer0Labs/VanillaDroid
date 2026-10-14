#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <span>

namespace vd::game {

struct SpellDetails;

// Phase 20 (smart, docs/MOBILE_CONTROLS.md §6.2): the touch cluster's mount / travel button. A mount is a
// spell that applies SPELL_AURA_MOUNTED (an item's on-use spell, or a class mount the player knows: Summon
// Felsteed, Warhorse...) and its speed the points of its SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED; the class
// forms that travel (Travel Form, Cat Form indoors, Aquatic Form swimming, Ghost Wolf, Aspect of the Cheetah)
// stand in without one. Where each can be used comes from its spell (SPELL_ATTR_ONLY_OUTDOORS,
// SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL), as the server checks it. Plain functions; no allocation.

constexpr u32 kAuraMounted = 78;                    // SPELL_AURA_MOUNTED
constexpr u32 kAuraModMountedSpeed = 32;            // SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED
constexpr u32 kSpellAttrOnlyOutdoors = 0x00008000;  // SPELL_ATTR_ONLY_OUTDOORS
constexpr u32 kSpellAttrNotShapeshift = 0x00010000; // SPELL_ATTR_NOT_SHAPESHIFT
constexpr u32 kSpellAttrNotInCombat = 0x10000000;   // SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL
// The class spells that travel without a mount (one rank each in 1.12).
constexpr u32 kSpellTravelForm = 783;
constexpr u32 kSpellAquaticForm = 1066;
constexpr u32 kSpellCatForm = 768;
constexpr u32 kSpellGhostWolf = 2645;
constexpr u32 kSpellAspectOfTheCheetah = 5118;

// Whether `spell` mounts; its speed (percent over running, 0 when it says none).
bool isMountSpell(const SpellDetails& spell, u32* speed = nullptr);

// A way to travel the button can offer.
struct TravelOption {
    TouchAction action; // an item (its entry) or a spell
    u32 speed = 0;      // a mount's percent (forms: 0)
    u32 form = 0;       // the shapeshift form it takes (SpellShapeshiftForm id), 0 a mount or an aspect
    bool mount = false; // SPELL_AURA_MOUNTED
    bool outdoorsOnly = false;    // SPELL_ATTR_ONLY_OUTDOORS
    bool notInCombat = false;     // SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL
    bool notShapeshifted = false; // SPELL_ATTR_NOT_SHAPESHIFT (the mounts)
    bool swimming = false;        // only in water (Aquatic Form)
    bool usable = true;           // the player's level and riding skill allow it (items)
    bool active = false;          // its aura is on the player now (Aspect of the Cheetah: a tap cancels it)
};

// The option of a spell (an item's on-use spell, a class mount or form) for `action`: mount and speed, form,
// where it can be used. `usable` and `active` are the caller's.
TravelOption travelOptionOf(const SpellDetails& spell, const TouchAction& action);

// The mount the button uses: the player's favourite (`favorite`, TouchLayout::mount) when it is there and
// usable, else the fastest usable one (a class mount before an item at the same speed: nothing to carry). -1
// none.
i32 chooseMount(std::span<const TravelOption> options, const TouchAction& favorite);

// The player's state for the button.
struct TravelState {
    bool mounted = false; // UNIT_FIELD_MOUNTDISPLAYID
    bool swimming = false;
    bool indoors = false;
    bool inCombat = false;
    u32 form = 0;      // UNIT_FIELD_BYTES_1 byte 2
    u32 auraSpell = 0; // mounted: the mount's aura (its spell: what a tap cancels)
};
// What the button shows and does now.
struct TravelChoice {
    TouchAction action;  // empty: hidden (nothing to travel with)
    bool usable = true;  // false: shown faint (indoors, in combat, swimming: the server would refuse it)
    bool active = false; // mounted, or in that form: a tap leaves it (cancels its aura)
};
// §6.2: mounted -> dismount; swimming -> Aquatic Form; a usable mount (outdoors, out of combat, not swimming)
// -> the chosen one; else the forms (Travel Form outdoors, Cat Form indoors, Ghost Wolf, Aspect of the
// Cheetah) that can be used here; else the mount or the first form, faint. `options` holds the mounts and the
// forms the player has.
TravelChoice chooseTravel(const TravelState& state, std::span<const TravelOption> options,
                          const TouchAction& favorite);

} // namespace vd::game
