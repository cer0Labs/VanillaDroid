// Phase 9: the spell visual tables (game/SpellVisualData) on real rows of the 1.12.1 client, and the
// animation choices of game/CombatAnimation.
#include "anim/Animator.h"
#include "fx/EffectSet.h"
#include "game/CombatAnimation.h"
#include "game/SpellVisualData.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <set>

using namespace vd;
using namespace vd::game;
namespace animation = vd::anim::animation;

namespace {

bool openPack(pack::PackReader& pack, const char* name) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && pack.open(std::filesystem::path(dir) / name).ok();
}

u64 model(const char* path) {
    return fx::effectModelHash(path);
}

} // namespace

TEST_CASE("Spell visual tables: real rows of the 1.12.1 client (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openPack(db, "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    SpellVisualData data;
    REQUIRE(data.init(db));

    // Frostbolt (116): SpellVisual 13 and its kits, a missile at 28 yd/s.
    const SpellVisualInfo* frostbolt = data.spellVisual(116);
    REQUIRE(frostbolt);
    CHECK(frostbolt->id == 13);
    CHECK(data.missileSpeed(116) == doctest::Approx(28.0f));
    CHECK(frostbolt->precastKit == 194);
    CHECK(frostbolt->castKit == 202);
    CHECK(frostbolt->impactKit == 4991);
    CHECK(frostbolt->stateKit == 3531);
    CHECK(frostbolt->channelKit == 0);
    CHECK(frostbolt->missileModel == model("Spells\\Frostbolt.mdx"));
    CHECK(frostbolt->missilePath == 0);
    CHECK(frostbolt->missileAttachment == 1);
    CHECK(frostbolt->areaModel == 0);
    CHECK(data.modelPath(frostbolt->missileModel) == "Spells\\Frostbolt.mdx");

    // Its precast: both hands glow, the caster readies a directed spell.
    const VisualKit* precast = data.kit(194);
    REQUIRE(precast);
    CHECK(precast->animation == animation::kReadySpellDirected);
    CHECK(precast->models[static_cast<u32>(KitSlot::LeftHand)] == model("Spells\\Ice_Precast_Low_Hand.mdx"));
    CHECK(precast->models[static_cast<u32>(KitSlot::RightHand)] == model("Spells\\Ice_Precast_Low_Hand.mdx"));
    CHECK(precast->modelCount == 2);
    CHECK(data.kit(202)->animation == animation::kSpellCastDirected);
    // The impact on the chest, with a wound; the slowing state over the head.
    const VisualKit* impact = data.kit(4991);
    REQUIRE(impact);
    CHECK(impact->animation == animation::kCombatWound);
    CHECK(impact->models[static_cast<u32>(KitSlot::Chest)] == model("Spells\\Ice_ImpactDD_Med_Chest.mdx"));
    CHECK(data.kit(3531)->models[static_cast<u32>(KitSlot::Head)] ==
          model("Spells\\Ice_Precast_Uber_Head.mdx"));

    // Blizzard (10): a channel (omni) and its ground area.
    const SpellVisualInfo* blizzard = data.spellVisual(10);
    REQUIRE(blizzard);
    CHECK(blizzard->channelKit == 717);
    CHECK(data.kit(717)->animation == animation::kChannelCastOmni);
    CHECK(blizzard->areaModel == model("Spells\\Blizzard_Impact_Base.mdx"));
    CHECK(blizzard->areaKit == 609);
    CHECK(data.missileSpeed(10) == 0.0f);
    // Frost Nova (122): the cast kit's model at the feet; its state kit has a world model too.
    const VisualKit* nova = data.kit(data.spellVisual(122)->castKit);
    REQUIRE(nova);
    CHECK(nova->models[static_cast<u32>(KitSlot::Base)] == model("Spells\\Frost_Nova_area.mdx"));
    CHECK(data.kit(data.spellVisual(122)->stateKit)->models[static_cast<u32>(KitSlot::World)] ==
          model("Spells\\Frost_Nova_state.mdx"));
    // Heroic Strike (78): an ability swing (Special1H), no cast time.
    CHECK(data.kit(data.spellVisual(78)->castKit)->animation == 57);
    // Shots with a ranged weapon: Auto Shot (bows, guns, crossbows), the wand's Shoot, Throw; melee abilities
    // and spells are not.
    CHECK(data.rangedWeaponMask(75) == 0x4000c);
    CHECK(data.rangedWeaponMask(5019) == 0x80000);
    CHECK(data.rangedWeaponMask(2764) == 0x10000);
    CHECK(data.rangedWeaponMask(7918) == 0x8);
    CHECK(data.rangedWeaponMask(1752) == 0); // Sinister Strike: a melee weapon
    CHECK(data.rangedWeaponMask(116) == 0);
    // Spells without a visual (Auto Shot) or unknown.
    CHECK(data.spellVisual(75) == nullptr);
    CHECK(data.spellVisual(0) == nullptr);
    CHECK(data.spellVisual(99999999) == nullptr);
    CHECK(data.kit(0) == nullptr);

    // Enchantments' glows along a weapon (its points 0-4): Flametongue 1 (5) burns at point 3, Deadly Poison
    // (7) drips from point 4; a stat enchantment shows nothing.
    std::array<u64, SpellVisualData::kItemVisualSlots> glow{};
    REQUIRE(data.enchantModels(5, glow));
    CHECK(glow[3] == model("Spells/Enchantments/Shaman_Fire.mdx"));
    CHECK(glow[0] == 0);
    CHECK(glow[4] == 0);
    REQUIRE(data.enchantModels(7, glow));
    CHECK(glow[4] == model("Spells/Enchantments/PoisonDrip.mdx"));
    CHECK_FALSE(data.enchantModels(0, glow));
    CHECK_FALSE(data.enchantModels(15, glow)); // Reinforced (+8 Armor)

    // Emotes: a one-shot bow, a dance state; /applaud is the applaud emote.
    CHECK(data.emoteAnimation(2) == 66);
    CHECK(data.emoteOneShot(2));
    CHECK(data.emoteAnimation(10) == 69); // EmoteDance
    CHECK_FALSE(data.emoteOneShot(10));
    CHECK(data.textEmote(5) == 21);
    CHECK(data.emoteAnimation(21) == 80);
    CHECK(data.emoteAnimation(0) == kNoKitAnimation);
}

TEST_CASE("Spell visual models: the kits' and missiles' models are baked into actors.vdpk (VD_PACKS_DIR)") {
    pack::PackReader db, actors;
    if (!openPack(db, "db.vdpk") || !openPack(actors, "actors.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    SpellVisualData data;
    REQUIRE(data.init(db));
    std::set<u64> models;
    // Enchantment glows too.
    for (u32 enchant : {5u, 2u, 283u, 29u, 7u}) {
        std::array<u64, SpellVisualData::kItemVisualSlots> glow{};
        REQUIRE(data.enchantModels(enchant, glow));
        for (u64 m : glow) {
            if (m != 0) {
                models.insert(m);
            }
        }
    }
    for (u32 spell :
         {116u, 133u, 10u, 2120u, 686u, 403u, 2054u, 139u, 17u, 122u, 118u, 689u, 5740u, 324u, 168u}) {
        const SpellVisualInfo* v = data.spellVisual(spell);
        REQUIRE(v);
        for (u32 kitId : {v->precastKit, v->castKit, v->impactKit, v->stateKit, v->channelKit}) {
            if (const VisualKit* kit = data.kit(kitId)) {
                for (u64 m : kit->models) {
                    if (m != 0) {
                        models.insert(m);
                    }
                }
            }
        }
        for (u64 m : {v->missileModel, v->areaModel}) {
            if (m != 0) {
                models.insert(m);
            }
        }
    }
    u32 missing = 0;
    for (u64 m : models) {
        if (!actors.find(m)) {
            ++missing;
            MESSAGE("not baked: " << data.modelPath(m));
        }
    }
    MESSAGE(models.size() << " effect models of common spells, " << missing << " missing");
    CHECK(models.size() > 20);
    CHECK(missing == 0);
}

TEST_CASE("Combat animations: swings, stances and reactions by weapon") {
    UnitGear gear;
    // Weapon kinds from the item class and subclass (ItemSubclassWeapon), shields and holdables from armor.
    CHECK(weaponKind(2, 7) == WeaponKind::OneHand); // sword
    CHECK(weaponKind(2, 15) == WeaponKind::Dagger);
    CHECK(weaponKind(2, 8) == WeaponKind::TwoHand);       // two-handed sword
    CHECK(weaponKind(2, 10) == WeaponKind::TwoHandLoose); // staff
    CHECK(weaponKind(2, 6) == WeaponKind::TwoHandLoose);  // polearm
    CHECK(weaponKind(2, 2) == WeaponKind::Bow);
    CHECK(weaponKind(2, 18) == WeaponKind::Bow); // crossbow
    CHECK(weaponKind(2, 3) == WeaponKind::Gun);
    CHECK(weaponKind(2, 16) == WeaponKind::Thrown);
    CHECK(weaponKind(2, 19) == WeaponKind::Wand);
    CHECK(weaponKind(4, 6) == WeaponKind::Shield);
    CHECK(weaponKind(0, 0, 14) == WeaponKind::Shield);
    CHECK(weaponKind(4, 0, 23) == WeaponKind::Holdable);
    CHECK(weaponKind(0, 0) == WeaponKind::None);

    // Swings: fists, a sword, a dagger's thrust, two hands, a staff; the off hand.
    CHECK(swingAnimation(gear, false) == animation::kAttackUnarmed);
    CHECK(swingAnimation(gear, true) == animation::kAttackUnarmedOff);
    gear.mainHand = WeaponKind::OneHand;
    gear.offHand = WeaponKind::Dagger;
    CHECK(swingAnimation(gear, false) == animation::kAttack1H);
    CHECK(swingAnimation(gear, true) == animation::kAttackOffPierce);
    gear.mainHand = WeaponKind::Dagger;
    CHECK(swingAnimation(gear, false) == animation::kAttack1HPierce);
    gear.mainHand = WeaponKind::TwoHand;
    CHECK(swingAnimation(gear, false) == animation::kAttack2H);
    CHECK(readyAnimation(gear, false) == animation::kReady2H);
    gear.mainHand = WeaponKind::TwoHandLoose;
    CHECK(swingAnimation(gear, false) == animation::kAttack2HLoose);
    CHECK(readyAnimation(gear, false) == animation::kReady2HLoose);
    // Sheathed: the fists.
    gear.sheathed = true;
    CHECK(swingAnimation(gear, false) == animation::kAttackUnarmed);
    CHECK(readyAnimation(gear, false) == animation::kReadyUnarmed);
    gear.sheathed = false;
    // Ranged: bow, gun, thrown, wand; the ready stance while shooting.
    gear.ranged = WeaponKind::Bow;
    CHECK(rangedAttackAnimation(gear.ranged) == animation::kAttackBow);
    CHECK(readyAnimation(gear, true) == animation::kReadyBow);
    CHECK(rangedAttackAnimation(WeaponKind::Gun) == animation::kAttackRifle);
    CHECK(rangedAttackAnimation(WeaponKind::Thrown) == animation::kAttackThrown);
    CHECK(rangedAttackAnimation(WeaponKind::OneHand) == kNoAnimation);

    // Reactions of the victim.
    CombatEvent e;
    e.kind = CombatEvent::Kind::Damage;
    e.amount = 120;
    UnitGear victim;
    victim.mainHand = WeaponKind::OneHand;
    CHECK(reactionAnimation(e, victim) == animation::kCombatWound);
    e.flags = CombatEvent::kCrit;
    CHECK(reactionAnimation(e, victim) == animation::kCombatCritical);
    e.flags = CombatEvent::kPeriodic;
    CHECK(reactionAnimation(e, victim) == kNoAnimation); // a damage over time tick: no flinch
    e.flags = 0;
    e.kind = CombatEvent::Kind::Dodge;
    CHECK(reactionAnimation(e, victim) == animation::kDodge);
    e.kind = CombatEvent::Kind::Parry;
    CHECK(reactionAnimation(e, victim) == animation::kParry1H);
    victim.mainHand = WeaponKind::TwoHand;
    CHECK(reactionAnimation(e, victim) == animation::kParry2H);
    victim.mainHand = WeaponKind::None;
    CHECK(reactionAnimation(e, victim) == animation::kParryUnarmed);
    e.kind = CombatEvent::Kind::Block;
    CHECK(reactionAnimation(e, victim) == animation::kShieldBlock);
    e.kind = CombatEvent::Kind::Miss;
    CHECK(reactionAnimation(e, victim) == kNoAnimation);
    e.kind = CombatEvent::Kind::Heal;
    CHECK(reactionAnimation(e, victim) == kNoAnimation);
}

TEST_CASE("Combat animations: spells and the base state") {
    // Precast: the kit's, else directed at a unit or omni.
    CHECK(precastAnimation(kNoAnimation, true) == animation::kReadySpellDirected);
    CHECK(precastAnimation(kNoAnimation, false) == animation::kReadySpellOmni);
    CHECK(precastAnimation(83, true) == 83);
    // Cast: an instant without its own animation shows none.
    CHECK(castAnimation(kNoAnimation, true, true) == animation::kSpellCastDirected);
    CHECK(castAnimation(kNoAnimation, false, true) == animation::kSpellCastOmni);
    CHECK(castAnimation(kNoAnimation, true, false) == kNoAnimation);
    CHECK(castAnimation(57, true, false) == 57);
    CHECK(channelAnimation(kNoAnimation, true) == animation::kChannelCastDirected);
    CHECK(channelAnimation(kNoAnimation, false) == animation::kChannelCastOmni);

    UnitAnimState s;
    CHECK(baseAnimation(s).animation == animation::kStand);
    s.inCombat = true;
    s.gear.mainHand = WeaponKind::OneHand;
    CHECK(baseAnimation(s).animation == animation::kReady1H);
    s.ranged = true;
    s.gear.ranged = WeaponKind::Gun;
    CHECK(baseAnimation(s).animation == animation::kReadyRifle);
    s.standState = 1; // sitting
    CHECK(baseAnimation(s).animation == animation::kSitGround);
    s.movement = animation::kRun;
    CHECK(baseAnimation(s).animation == animation::kRun);
    CHECK(baseAnimation(s).loop);
    s.dead = true;
    CHECK(baseAnimation(s).animation == animation::kDeath);
    CHECK_FALSE(baseAnimation(s).loop);
    s.corpse = true;
    CHECK(baseAnimation(s).animation == animation::kDead);
    // NPC emote states loop when nothing else applies.
    UnitAnimState npc;
    npc.emoteState = 69;
    CHECK(baseAnimation(npc).animation == 69);
    CHECK(standAnimation(3) == animation::kSleep);
    CHECK(standAnimation(0) == animation::kStand);
}

TEST_CASE("Sheath state: WoW.exe 0x5FDF80 from the animation's weapon flags") {
    namespace wf = vd::anim::weaponflag;
    // A druid with her staff drawn casts Wrath: the spell's animation puts it away (the 1.12 tells the server).
    SheathInput cast;
    cast.current = 1;
    cast.animation = animation::kSpellCastDirected;
    cast.weaponFlags = wf::kSheathe;
    cast.local = true;
    CHECK(sheathAfterAnimation(cast) == 0);
    cast.meleeAttacking = true; // even while auto attacking
    CHECK(sheathAfterAnimation(cast) == 0);
    // Back to standing: put away stays put away; with the auto attack on the weapon comes out again.
    SheathInput stand;
    stand.current = 0;
    stand.animation = animation::kStand;
    stand.local = true;
    stand.field = 1;
    CHECK(sheathAfterAnimation(stand) == 0); // the player's own state, not the server's
    stand.meleeAttacking = true;
    CHECK(sheathAfterAnimation(stand) == 1);
    // Another unit standing follows the server.
    SheathInput other = stand;
    other.local = false;
    other.meleeAttacking = false;
    CHECK(sheathAfterAnimation(other) == 1);
    other.field = 0;
    CHECK(sheathAfterAnimation(other) == 0);
    // Unarmed stance and emotes put the weapons away; armed swings and stances draw them; a fist swing while
    // attacking draws them.
    SheathInput ready;
    ready.current = 1;
    ready.animation = animation::kReadyUnarmed;
    ready.weaponFlags = wf::kUnarmed;
    CHECK(sheathAfterAnimation(ready) == 0);
    ready.animation = animation::kAttackUnarmed;
    ready.meleeAttacking = true;
    CHECK(sheathAfterAnimation(ready) == 1);
    SheathInput swing;
    swing.animation = animation::kAttack2HLoose;
    swing.weaponFlags = wf::kArmed;
    CHECK(sheathAfterAnimation(swing) == 1);
    // Mounted: away whatever it plays.
    swing.mounted = true;
    CHECK(sheathAfterAnimation(swing) == 0);
    // A drawn bow stays out through its own animations and a cast-free stance, not through an emote.
    SheathInput bow;
    bow.current = 2;
    bow.animation = animation::kAttackBow;
    bow.weaponFlags = wf::kUnarmed;
    CHECK(sheathAfterAnimation(bow) == 2);
    bow.animation = animation::kStand;
    bow.weaponFlags = 0;
    CHECK(sheathAfterAnimation(bow) == 2);
    bow.animation = 69; // EmoteDance
    bow.weaponFlags = wf::kUnarmed;
    CHECK(sheathAfterAnimation(bow) == 0);
    bow.animation = animation::kSpellCastDirected;
    bow.weaponFlags = wf::kSheathe;
    CHECK(sheathAfterAnimation(bow) == 0);
}
