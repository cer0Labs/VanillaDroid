#include "game/CombatAnimation.h"

#include "anim/Animator.h"

namespace vd::game {
namespace {

namespace animation = anim::animation;

// ItemSubclassWeapon (vMaNGOS ItemPrototype.h).
constexpr u32 kItemClassWeapon = 2, kItemClassArmor = 4;
constexpr u32 kAxe = 0, kAxe2 = 1, kBow = 2, kGun = 3, kMace = 4, kMace2 = 5, kPolearm = 6, kSword = 7,
              kSword2 = 8, kStaff = 10, kExotic = 11, kExotic2 = 12, kFist = 13, kMisc = 14, kDagger = 15,
              kThrown = 16, kSpear = 17, kCrossbow = 18, kWand = 19, kFishingPole = 20;
constexpr u32 kArmorShield = 6;
constexpr u32 kInventoryShield = 14, kInventoryHoldable = 23;

} // namespace

WeaponKind weaponKind(u32 itemClass, u32 subclass, u32 inventoryType) {
    if (itemClass == kItemClassWeapon) {
        switch (subclass) {
        case kAxe:
        case kMace:
        case kSword:
        case kExotic:
        case kFist:
        case kMisc: return WeaponKind::OneHand;
        case kDagger: return WeaponKind::Dagger;
        case kAxe2:
        case kMace2:
        case kSword2:
        case kExotic2: return WeaponKind::TwoHand;
        case kPolearm:
        case kStaff:
        case kSpear:
        case kFishingPole: return WeaponKind::TwoHandLoose;
        case kBow:
        case kCrossbow: return WeaponKind::Bow;
        case kGun: return WeaponKind::Gun;
        case kThrown: return WeaponKind::Thrown;
        case kWand: return WeaponKind::Wand;
        default: return WeaponKind::OneHand;
        }
    }
    if ((itemClass == kItemClassArmor && subclass == kArmorShield) || inventoryType == kInventoryShield) {
        return WeaponKind::Shield;
    }
    if (inventoryType == kInventoryHoldable) {
        return WeaponKind::Holdable;
    }
    return WeaponKind::None;
}

u16 swingAnimation(const UnitGear& gear, bool offHand) {
    const WeaponKind kind = gear.sheathed ? WeaponKind::None : offHand ? gear.offHand : gear.mainHand;
    if (offHand) {
        switch (kind) {
        case WeaponKind::Dagger: return animation::kAttackOffPierce;
        case WeaponKind::OneHand:
        case WeaponKind::TwoHand:
        case WeaponKind::TwoHandLoose: return animation::kAttackOff;
        default: return animation::kAttackUnarmedOff;
        }
    }
    switch (kind) {
    case WeaponKind::OneHand: return animation::kAttack1H;
    case WeaponKind::Dagger: return animation::kAttack1HPierce;
    case WeaponKind::TwoHand: return animation::kAttack2H;
    case WeaponKind::TwoHandLoose: return animation::kAttack2HLoose;
    default: return animation::kAttackUnarmed;
    }
}

u16 rangedAttackAnimation(WeaponKind ranged) {
    switch (ranged) {
    case WeaponKind::Bow: return animation::kAttackBow;
    case WeaponKind::Gun: return animation::kAttackRifle;
    case WeaponKind::Thrown: return animation::kAttackThrown;
    case WeaponKind::Wand: return animation::kSpellCastDirected;
    default: return kNoAnimation;
    }
}

u16 readyAnimation(const UnitGear& gear, bool ranged) {
    if (ranged) {
        switch (gear.ranged) {
        case WeaponKind::Bow: return animation::kReadyBow;
        case WeaponKind::Gun: return animation::kReadyRifle;
        case WeaponKind::Thrown: return animation::kReadyThrown;
        case WeaponKind::Wand: return animation::kReadySpellDirected;
        default: break;
        }
    }
    switch (gear.sheathed ? WeaponKind::None : gear.mainHand) {
    case WeaponKind::OneHand:
    case WeaponKind::Dagger: return animation::kReady1H;
    case WeaponKind::TwoHand: return animation::kReady2H;
    case WeaponKind::TwoHandLoose: return animation::kReady2HLoose;
    default: return animation::kReadyUnarmed;
    }
}

u16 reactionAnimation(const CombatEvent& e, const UnitGear& victim) {
    using K = CombatEvent::Kind;
    switch (e.kind) {
    case K::Damage:
        if (e.amount <= 0 || (e.flags & (CombatEvent::kPeriodic | CombatEvent::kEnvironment)) != 0) {
            return kNoAnimation;
        }
        return (e.flags & CombatEvent::kCrit) ? animation::kCombatCritical : animation::kCombatWound;
    case K::Dodge: return animation::kDodge;
    case K::Parry: {
        const WeaponKind hand = victim.sheathed ? WeaponKind::None : victim.mainHand;
        return hand == WeaponKind::None           ? animation::kParryUnarmed
               : hand == WeaponKind::TwoHand      ? animation::kParry2H
               : hand == WeaponKind::TwoHandLoose ? animation::kParry2HLoose
                                                  : animation::kParry1H;
    }
    case K::Block: return animation::kShieldBlock;
    default: return kNoAnimation;
    }
}

u16 precastAnimation(u16 kitAnimation, bool unitTarget) {
    if (kitAnimation != kNoAnimation) {
        return kitAnimation;
    }
    return unitTarget ? animation::kReadySpellDirected : animation::kReadySpellOmni;
}

u16 castAnimation(u16 kitAnimation, bool unitTarget, bool hadCastTime) {
    if (kitAnimation != kNoAnimation) {
        return kitAnimation;
    }
    if (!hadCastTime) {
        return kNoAnimation;
    }
    return unitTarget ? animation::kSpellCastDirected : animation::kSpellCastOmni;
}

u16 channelAnimation(u16 kitAnimation, bool unitTarget) {
    if (kitAnimation != kNoAnimation) {
        return kitAnimation;
    }
    return unitTarget ? animation::kChannelCastDirected : animation::kChannelCastOmni;
}

u16 standAnimation(u8 standState) {
    switch (standState) {
    case 1: return animation::kSitGround;
    case 2:
    case 4: return animation::kSitChairLow;
    case 5: return animation::kSitChairMedium;
    case 6: return animation::kSitChairHigh;
    case 3: return animation::kSleep;
    case 8: return animation::kKneel;
    default: return animation::kStand;
    }
}

BaseAnimation baseAnimation(const UnitAnimState& s) {
    if (s.dead) {
        // Falls once and stays down; a corpse found lying is already there.
        return {s.corpse ? animation::kDead : animation::kDeath, false};
    }
    if (s.movement != animation::kStand) {
        return {s.movement, true};
    }
    if (s.standState != 0) {
        return {standAnimation(s.standState), true};
    }
    if (s.emoteState != 0) {
        return {s.emoteState, true};
    }
    if (s.inCombat) {
        return {readyAnimation(s.gear, s.ranged), true};
    }
    return {animation::kStand, true};
}

namespace {

// WoW.exe 0x5FCC10: the melee animations of a fight (wounds, swings, parries, blocks, dodges, kicks, bashes).
bool meleeCombatAnimation(u16 id) {
    return id == 10 || (id >= 16 && id <= 24) || id == 30 || id == 36 || (id >= 57 && id <= 59) ||
           (id >= 85 && id <= 88) || id == 95 || id == 117 || id == 118;
}

// WoW.exe 0x5FE180: the ranged ones (AttackBow, AttackRifle, LoadBow, LoadRifle, AttackThrown, HoldBow, HoldRifle,
// HoldThrown, LoadThrown), which keep a ranged weapon in the hands.
bool rangedWeaponAnimation(u16 id) {
    return id == 46 || id == 49 || (id >= 105 && id <= 107) || (id >= 109 && id <= 112);
}

} // namespace

u8 sheathAfterAnimation(const SheathInput& in) {
    constexpr u8 kAway = 0, kMelee = 1, kRanged = 2;
    if ((in.weaponFlags & anim::weaponflag::kSheathe) != 0 || in.mounted) {
        return kAway;
    }
    if (in.current == kRanged) {
        // The ranged weapon stays out through its own animations and anything without the unarmed flag.
        const bool unarmed = (in.weaponFlags & anim::weaponflag::kUnarmed) != 0;
        return unarmed && !rangedWeaponAnimation(in.animation) ? kAway : kRanged;
    }
    if (in.meleeAttacking && meleeCombatAnimation(in.animation)) {
        return kMelee;
    }
    if ((in.weaponFlags & anim::weaponflag::kUnarmed) != 0) {
        return kAway;
    }
    if ((in.weaponFlags & anim::weaponflag::kArmed) != 0 || in.meleeAttacking) {
        return kMelee;
    }
    return in.local ? in.current : in.field;
}

} // namespace vd::game
