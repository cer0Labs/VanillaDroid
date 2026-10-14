#pragma once

#include "core/Types.h"
#include "game/Combat.h"

namespace vd::game {

// Phase 9: which animation a fighting unit plays, from what it holds and what happens to it. Pure functions
// of the client's rules (AnimationData ids: anim/Animator.h); the units of the server (app/Units) and the
// offline group fight (app/CrowdFight) both decide with them.

// What a unit holds, as its animations see it.
enum class WeaponKind : u8 {
    None,         // empty hand: fists
    OneHand,      // axes, maces, swords, fist weapons
    Dagger,       // thrusts
    TwoHand,      // two-handed axes, maces and swords
    TwoHandLoose, // polearms, staves, spears, fishing poles: held "loose", lower
    Bow,          // bows and crossbows
    Gun,
    Thrown,
    Wand,
    Shield,  // off hand: blocks
    Holdable // off hand: an orb or a book, held but not swung
};

// The item class (2 weapon, 4 armor) and subclass of an item (ItemPrototype.h ItemSubclassWeapon; armor
// subclass 6 shield); `inventoryType` (InventoryType: 14 shield, 23 holdable) settles what the class does
// not.
WeaponKind weaponKind(u32 itemClass, u32 subclass, u32 inventoryType = 0);

struct UnitGear {
    WeaponKind mainHand = WeaponKind::None;
    WeaponKind offHand = WeaponKind::None;
    WeaponKind ranged = WeaponKind::None;
    bool sheathed = false; // weapons on the back (UNIT_FIELD_BYTES_2 sheath state 0): fists and empty hands
};

constexpr u16 kNoAnimation = 0xFFFF;

// --- Melee and ranged attacks (SMSG_ATTACKERSTATEUPDATE; Auto Shot / Shoot) --------------------------------
u16 swingAnimation(const UnitGear& gear, bool offHand);
u16 rangedAttackAnimation(WeaponKind ranged); // kNoAnimation for melee kinds
// The stance while in combat and standing still: by the main hand, or the ranged weapon when shooting.
u16 readyAnimation(const UnitGear& gear, bool ranged);

// --- Being hit (the victim of a combat log line) -----------------------------------------------------------
// Wound (crit: CombatCritical), dodge, parry by weapon, shield block; kNoAnimation for what shows nothing (a
// miss, an immunity, a heal).
u16 reactionAnimation(const CombatEvent& e, const UnitGear& victim);

// --- Spells ------------------------------------------------------------------------------------------------
// The kit's animation (kNoAnimation: the kit has none), else the generic ones of the client.
// While casting (a cast time): directed at a unit or not.
u16 precastAnimation(u16 kitAnimation, bool unitTarget);
// When it goes off: the cast kit's, else a directed or omni cast after a cast time; an instant without its
// own animation shows none.
u16 castAnimation(u16 kitAnimation, bool unitTarget, bool hadCastTime);
// While channeling.
u16 channelAnimation(u16 kitAnimation, bool unitTarget);

// --- The base animation from the unit's state --------------------------------------------------------------
struct UnitAnimState {
    bool dead = false;
    bool corpse = false; // dead when it appeared: already lying (Dead), not falling (Death)
    u16 movement = 0;    // anim::animation kStand, kWalk, kRun, kSwim, kJump...
    u8 standState = 0;   // UNIT_STAND_STATE_*: sitting, sleeping, kneeling
    u16 emoteState = 0;  // UNIT_NPC_EMOTESTATE's animation (0: none)
    bool inCombat = false;
    bool ranged = false; // shooting (Auto Shot): the ranged ready stance
    UnitGear gear;
};
struct BaseAnimation {
    u16 animation = 0;
    bool loop = true;
};
// Dead > moving > sitting > NPC emote state > ready (in combat) > standing.
BaseAnimation baseAnimation(const UnitAnimState& state);
// UNIT_STAND_STATE_* -> its animation (Stand when standing).
u16 standAnimation(u8 standState);

// --- Where the weapons are (the sheath state: 0 put away, 1 melee in the hands, 2 ranged) ------------------
// WoW.exe 0x5FDF80, at the end of 0x5FE2F0: each time a unit's animation is chosen the client works out where its
// weapons go from that animation's AnimationData weapon flags (anim::weaponflag), and the active player tells the
// server (CMSG_SETSHEATHED). A spell's animation puts them away even while drawn, an armed swing draws them.
struct SheathInput {
    u8 current = 0;          // the state the unit shows
    u16 animation = 0;       // what it plays now (its action, else its base animation)
    u16 weaponFlags = 0;     // that animation's (AnimationData.dbc field 2)
    bool mounted = false;    // riding (0x5FDF80 reads the mount's model): the weapons stay put away
    bool meleeAttacking = false; // its auto attack is on (SMSG_ATTACKSTART .. ATTACKSTOP)
    bool local = false;      // the active player: its own state is the client's, never the server's
    u8 field = 0;            // the server's (UNIT_FIELD_BYTES_2 byte 0), what the others fall back to
};
u8 sheathAfterAnimation(const SheathInput& in);

} // namespace vd::game
