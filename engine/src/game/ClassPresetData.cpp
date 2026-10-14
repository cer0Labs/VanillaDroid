#include "game/ClassPresetData.h"

#include <algorithm>
#include <iterator>

namespace vd::game::presets {
namespace {

// docs/MOBILE_CONTROLS.md §7, the tables of the class presets. A slot lists its candidates, the first one the player
// knows (and that no slot filled before it took: Main, A, C, D, E, B, F, G, H) wins:
//   S(id)               a spell, by its first rank (the slot casts the highest rank known)
//   Top(tree, id)       a spell that needs the talent tree with the most points to be `tree`; TopOrNone: or none
//   G(kind)             the template's group of that kind (§6.6), its members those the player has
//   Use(consumable)     an automatic consumable (§6.1)
//   Same(layer)         what that layer shows there (the Prowl layer: Cat Form's)
//   kHide               nothing (a form whose base actions cannot be cast in it)
// No candidate (kInherit) or none known: the base shows there (in the base itself: nothing). Spells that need an
// equipped item (Backstab a dagger, Shield Bash a shield, Shoot a wand) count only with it worn.

constexpr Cand S(u32 spell) {
    return {CandKind::Spell, 0, 0, spell};
}
constexpr Cand Top(u16 tree, u32 spell) {
    return {CandKind::Spell, kIfTopTree, tree, spell};
}
constexpr Cand TopOrNone(u16 tree, u32 spell) {
    return {CandKind::Spell, kIfTopTree | kOrUntalented, tree, spell};
}
constexpr Cand G(GroupKind kind) {
    return {CandKind::Group, 0, 0, static_cast<u32>(kind)};
}
constexpr Cand Use(TouchConsumable consumable) {
    return {CandKind::Consumable, 0, 0, static_cast<u32>(consumable)};
}
constexpr Cand Same(TouchContext layer) {
    return {CandKind::SameAs, 0, 0, static_cast<u32>(layer)};
}
constexpr Cand kHide{CandKind::Hide, 0, 0, 0};
constexpr Slot slot(Cand a, Cand b = {}, Cand c = {}, Cand d = {}, Cand e = {}) {
    return {{a, b, c, d, e}};
}
constexpr Slot kInherit{};
constexpr Member M(u32 a, u32 b = 0, u32 c = 0, u32 d = 0, u32 e = 0) {
    return {{a, b, c, d, e}};
}
constexpr u16 contextBit(TouchContext layer) {
    return static_cast<u16>(1u << static_cast<u32>(layer));
}

constexpr u8 kWarrior = 1, kPaladin = 2, kHunter = 3, kRogue = 4, kPriest = 5, kShaman = 7, kMage = 8,
             kWarlock = 9, kDruid = 11;
// TalentTab.dbc ids.
constexpr u16 kTreeFire = 41, kTreeFrost = 61, kTreeArcane = 81, kTreeArms = 161,
              kTreeWarriorProtection = 163, kTreeFury = 164, kTreeCombat = 181, kTreeAssassination = 182,
              kTreeSubtlety = 183, kTreeDiscipline = 201, kTreePriestHoly = 202, kTreeShadow = 203,
              kTreeElemental = 261, kTreeShamanRestoration = 262, kTreeEnhancement = 263,
              kTreeFeralCombat = 281, kTreeDruidRestoration = 282, kTreeBalance = 283, kTreeDestruction = 301,
              kTreeAffliction = 302, kTreeDemonology = 303, kTreeBeastMastery = 361, kTreeSurvival = 362,
              kTreeMarksmanship = 363, kTreeRetribution = 381, kTreePaladinHoly = 382,
              kTreePaladinProtection = 383;

// §7.11: the races' own spells, a member of a class's groups (one of each race: the first the player knows).
// Perception (human), Shadowmeld (night elf), Blood Fury (orc), Berserking (troll), War Stomp (tauren).
constexpr Member kRacialOffensive = M(20600, 20580, 20572, 20554, 20549);
// Stoneform (dwarf), Escape Artist (gnome), Will of the Forsaken (undead): they free from poisons, roots, fear.
constexpr Member kRacialDefensive = M(20594, 20589, 7744);
// §7.8: the priest's race spells, two each race: Desperate Prayer (human, dwarf), Starshards (night elf), Touch of
// Weakness (undead), Hex of Weakness (troll); Feedback (human), Fear Ward (dwarf), Elune's Grace (night elf),
// Devouring Plague (undead), Shadowguard (troll).
constexpr Member kPriestRace1 = M(13908, 10797, 2652, 9035);
constexpr Member kPriestRace2 = M(13896, 6346, 2651, 2944, 18137);

// The race masks of those (SkillLineAbility.dbc: bit race - 1).
struct RaceSpell {
    u32 spell;
    u32 races;
};
constexpr RaceSpell kRaceSpells[] = {
    {20600, 0x1},  {20580, 0x8}, {20572, 0x2}, {20554, 0x80}, {20549, 0x20}, {20594, 0x4},
    {20589, 0x40}, {7744, 0x10}, {13908, 0x5}, {10797, 0x8},  {2652, 0x10},  {9035, 0x80},
    {13896, 0x1},  {6346, 0x4},  {2651, 0x8},  {2944, 0x10},  {18137, 0x80},
};

// The groups (§6.6). A member is a spell, or the first known of a few (Ice Armor over Frost Armor).
constexpr GroupDef kGroups[] = {
    // Thunder Clap, Cleave, Sweeping Strikes.
    {GroupKind::WarriorBattleArea, {M(6343), M(845), M(12292)}},
    // Battle Shout, Demoralizing Shout, Intimidating Shout.
    {GroupKind::WarriorShouts, {M(6673), M(1160), M(5246)}},
    // Retaliation, defensive racial.
    {GroupKind::WarriorBattleDefense, {M(20230), kRacialDefensive}},
    // Bloodrage, Death Wish, Recklessness, racial.
    {GroupKind::WarriorCooldowns, {M(2687), M(12328), M(1719), kRacialOffensive}},
    // Taunt, Mocking Blow, Challenging Shout.
    {GroupKind::WarriorTaunts, {M(355), M(694), M(1161)}},
    // Shield Wall, Last Stand, defensive racial.
    {GroupKind::WarriorShieldWall, {M(871), M(12975), kRacialDefensive}},
    // Berserker Rage, defensive racial.
    {GroupKind::WarriorBerserkerDefense, {M(18499), kRacialDefensive}},
    // Demoralizing Shout, Battle Shout, Intimidating Shout.
    {GroupKind::WarriorTankShouts, {M(1160), M(6673), M(5246)}},
    // Kidney Shot, Rupture, Expose Armor.
    {GroupKind::RogueFinishers, {M(408), M(1943), M(8647)}},
    // Kick, Gouge, Blind.
    {GroupKind::RogueInterrupts, {M(1766), M(1776), M(2094)}},
    // Blade Flurry, Adrenaline Rush, Cold Blood, Ghostly Strike, Preparation.
    {GroupKind::RogueCooldowns, {M(13877), M(13750), M(14177), M(14278), M(14185)}},
    // Evasion, Feint, Sprint, defensive racial.
    {GroupKind::RogueDefense, {M(5277), M(1966), M(2983), kRacialDefensive}},
    // Vanish, racial.
    {GroupKind::RogueVanish, {M(1856), kRacialOffensive}},
    // Stealth, Distract.
    {GroupKind::RogueStealthExit, {M(1784), M(1725)}},
    // Entangling Roots, Hibernate.
    {GroupKind::DruidControl, {M(339), M(2637)}},
    // Mark of the Wild, Thorns, Omen of Clarity.
    {GroupKind::DruidBuffs, {M(1126), M(467), M(16864)}},
    // Barkskin, Rejuvenation, Regrowth, Healing Touch, defensive racial.
    {GroupKind::DruidDefense, {M(22812), M(774), M(8936), M(5185), kRacialDefensive}},
    // Innervate, Rebirth, Nature's Swiftness, racial.
    {GroupKind::DruidCooldowns, {M(29166), M(20484), M(17116), kRacialOffensive}},
    // Growl, Feral Charge, Challenging Roar.
    {GroupKind::DruidBearTaunts, {M(6795), M(16979), M(5209)}},
    // Remove Curse, Abolish Poison or Cure Poison.
    {GroupKind::DruidCures, {M(2782), M(2893, 8946)}},
    // Innervate, Rebirth, racial.
    {GroupKind::DruidHealCooldowns, {M(29166), M(20484), kRacialOffensive}},
    // Frenzied Regeneration, defensive racial.
    {GroupKind::DruidBearDefense, {M(22842), kRacialDefensive}},
    // Barkskin, defensive racial.
    {GroupKind::DruidHealDefense, {M(22812), kRacialDefensive}},
    // Serpent Sting, Viper Sting, Scorpid Sting.
    {GroupKind::HunterStings, {M(1978), M(3034), M(3043)}},
    // Multi-Shot, Volley.
    {GroupKind::HunterArea, {M(2643), M(1510)}},
    // Concussive Shot, Scatter Shot, Wyvern Sting.
    {GroupKind::HunterControl, {M(5116), M(19503), M(19386)}},
    // Aspect of the Hawk, Aspect of the Monkey, Aspect of the Cheetah, Aspect of the Pack, Aspect of the Beast, Aspect
    // of the Wild.
    {GroupKind::HunterAspects, {M(13165), M(13163), M(5118), M(13159), M(13161), M(20043)}},
    // Feign Death, Freezing Trap, Frost Trap, Immolation Trap, Explosive Trap, defensive racial.
    {GroupKind::HunterTraps, {M(5384), M(1499), M(13809), M(13795), M(13813), kRacialDefensive}},
    // Rapid Fire, Bestial Wrath, Intimidation, racial.
    {GroupKind::HunterCooldowns, {M(3045), M(19574), M(19577), kRacialOffensive}},
    // Deterrence, defensive racial.
    {GroupKind::HunterMeleeDefense, {M(19263), kRacialDefensive}},
    // Arcane Explosion, Blizzard, Flamestrike, Cone of Cold.
    {GroupKind::MageArea, {M(1449), M(10), M(2120), M(120)}},
    // Polymorph, Polymorph: Pig, Polymorph: Turtle, Polymorph: Cow.
    {GroupKind::MagePolymorphs, {M(118), M(28272), M(28271), M(28270)}},
    // Ice Armor or Frost Armor, Mage Armor, Arcane Intellect, Dampen Magic, Amplify Magic, Remove Lesser Curse.
    {GroupKind::MageBuffs, {M(7302, 168), M(6117), M(1459), M(604), M(1008), M(475)}},
    // Frost Nova, Blink, Ice Block, Ice Barrier, Mana Shield, Fire Ward, Frost Ward, defensive racial.
    {GroupKind::MageDefense,
     {M(122), M(1953), M(11958), M(11426), M(1463), M(543), M(6143), kRacialDefensive}},
    // Evocation, Presence of Mind, Arcane Power, Combustion, Cold Snap, racial.
    {GroupKind::MageCooldowns, {M(12051), M(12043), M(12042), M(11129), M(12472), kRacialOffensive}},
    // Immolate, Searing Pain, Conflagrate, Shadowburn.
    {GroupKind::WarlockFire, {M(348), M(5676), M(17962), M(17877)}},
    // Rain of Fire, Hellfire.
    {GroupKind::WarlockArea, {M(5740), M(1949)}},
    // Fear, Howl of Terror, Death Coil, Banish.
    {GroupKind::WarlockControl, {M(5782), M(5484), M(6789), M(710)}},
    // Curse of Agony, Curse of Weakness, Curse of Recklessness, Curse of Tongues, Curse of the Elements, Curse of
    // Shadow, Curse of Exhaustion, Curse of Doom.
    {GroupKind::WarlockCurses, {M(980), M(702), M(704), M(1714), M(1490), M(17862), M(18223), M(603)}},
    // Demon Armor or Demon Skin, Unending Breath, Detect Greater Invisibility or Detect Invisibility or Detect Lesser
    // Invisibility, Shadow Ward.
    {GroupKind::WarlockBuffs, {M(706, 687), M(5697), M(11743, 2970, 132), M(6229)}},
    // Life Tap, Health Funnel, Dark Pact, Drain Life, Drain Mana, defensive racial.
    {GroupKind::WarlockLife, {M(1454), M(755), M(18220), M(689), M(5138), kRacialDefensive}},
    // Drain Soul, Shoot, racial.
    {GroupKind::WarlockDrain, {M(1120), M(5019), kRacialOffensive}},
    // Dispel Magic, Cure Disease, Abolish Disease.
    {GroupKind::PriestDispels, {M(527), M(528), M(552)}},
    // Power Word: Fortitude, Divine Spirit, Shadow Protection, Inner Fire, Prayer of Fortitude.
    {GroupKind::PriestBuffs, {M(1243), M(14752), M(976), M(588), M(21562)}},
    // Fade, Psychic Scream, defensive racial.
    {GroupKind::PriestHealDefense, {M(586), M(8122), kRacialDefensive}},
    // Inner Focus, Power Infusion, race spell, race spell, racial.
    {GroupKind::PriestHealCooldowns, {M(14751), M(10060), kPriestRace1, kPriestRace2, kRacialOffensive}},
    // Fade, Power Word: Shield, defensive racial.
    {GroupKind::PriestDamageDefense, {M(586), M(17), kRacialDefensive}},
    // Shoot, race spell, race spell, racial.
    {GroupKind::PriestWand, {M(5019), kPriestRace1, kPriestRace2, kRacialOffensive}},
    // Seal of Righteousness, Seal of Command, Seal of the Crusader, Seal of Justice, Seal of Light, Seal of Wisdom.
    {GroupKind::PaladinSeals, {M(20154), M(20375), M(21082), M(20164), M(20165), M(20166)}},
    // Flash of Light, Holy Light.
    {GroupKind::PaladinHeals, {M(19750), M(635)}},
    // Consecration, Exorcism, Holy Wrath.
    {GroupKind::PaladinHoly, {M(26573), M(879), M(2812)}},
    // Blessing of Might, Blessing of Wisdom, Blessing of Kings, Blessing of Salvation, Blessing of Light, Blessing of
    // Sanctuary, Blessing of Freedom, Blessing of Sacrifice.
    {GroupKind::PaladinBlessings,
     {M(19740), M(19742), M(20217), M(1038), M(19977), M(20911), M(1044), M(6940)}},
    // Devotion Aura, Retribution Aura, Concentration Aura, Shadow Resistance Aura, Frost Resistance Aura, Fire
    // Resistance Aura, Sanctity Aura.
    {GroupKind::PaladinAuras, {M(465), M(7294), M(19746), M(19876), M(19888), M(19891), M(20218)}},
    // Divine Shield or Divine Protection, Blessing of Protection, defensive racial.
    {GroupKind::PaladinDefense, {M(642, 498), M(1022), kRacialDefensive}},
    // Lay on Hands, Divine Favor, racial.
    {GroupKind::PaladinCooldowns, {M(633), M(20216), kRacialOffensive}},
    // Cleanse, Purify.
    {GroupKind::PaladinCleanse, {M(4987), M(1152)}},
    // Divine Favor, Lay on Hands, racial.
    {GroupKind::PaladinHealCooldowns, {M(20216), M(633), kRacialOffensive}},
    // Stoneskin Totem, Earthbind Totem, Stoneclaw Totem, Strength of Earth Totem, Tremor Totem.
    {GroupKind::ShamanEarthTotems, {M(8071), M(2484), M(5730), M(8075), M(8143)}},
    // Searing Totem, Fire Nova Totem, Magma Totem, Frost Resistance Totem, Flametongue Totem.
    {GroupKind::ShamanFireTotems, {M(3599), M(1535), M(8190), M(8181), M(8227)}},
    // Healing Stream Totem, Mana Spring Totem, Poison Cleansing Totem, Disease Cleansing Totem, Fire Resistance Totem.
    {GroupKind::ShamanWaterTotems, {M(5394), M(5675), M(8166), M(8170), M(8184)}},
    // Grounding Totem, Nature Resistance Totem, Windfury Totem, Sentry Totem, Windwall Totem, Grace of Air Totem,
    // Tranquil Air Totem.
    {GroupKind::ShamanAirTotems, {M(8177), M(10595), M(8512), M(6495), M(15107), M(8835), M(25908)}},
    // Earth Shock, Frost Shock, Purge.
    {GroupKind::ShamanShocks, {M(8042), M(8056), M(370)}},
    // Rockbiter Weapon, Flametongue Weapon, Frostbrand Weapon, Windfury Weapon.
    {GroupKind::ShamanWeapons, {M(8017), M(8024), M(8033), M(8232)}},
    // Lesser Healing Wave, Healing Wave, defensive racial.
    {GroupKind::ShamanHeals, {M(8004), M(331), kRacialDefensive}},
    // Elemental Mastery, racial.
    {GroupKind::ShamanCooldowns, {M(16166), kRacialOffensive}},
    // Frost Shock, Purge.
    {GroupKind::ShamanFrostShocks, {M(8056), M(370)}},
    // Cure Poison, Cure Disease.
    {GroupKind::ShamanCures, {M(526), M(2870)}},
    // Earth Shock, Purge.
    {GroupKind::ShamanHealShocks, {M(8042), M(370)}},
    // Mana Tide Totem, racial.
    {GroupKind::ShamanManaTide, {M(16190), kRacialOffensive}},
    // racial.
    {GroupKind::Racial, {kRacialOffensive}},
};

struct GroupOwner {
    GroupKind kind;
    u8 cls;
};
constexpr GroupOwner kGroupOwners[] = {
    {GroupKind::WarriorBattleArea, kWarrior},
    {GroupKind::WarriorShouts, kWarrior},
    {GroupKind::WarriorBattleDefense, kWarrior},
    {GroupKind::WarriorCooldowns, kWarrior},
    {GroupKind::WarriorTaunts, kWarrior},
    {GroupKind::WarriorShieldWall, kWarrior},
    {GroupKind::WarriorBerserkerDefense, kWarrior},
    {GroupKind::WarriorTankShouts, kWarrior},
    {GroupKind::RogueFinishers, kRogue},
    {GroupKind::RogueInterrupts, kRogue},
    {GroupKind::RogueCooldowns, kRogue},
    {GroupKind::RogueDefense, kRogue},
    {GroupKind::RogueVanish, kRogue},
    {GroupKind::RogueStealthExit, kRogue},
    {GroupKind::DruidControl, kDruid},
    {GroupKind::DruidBuffs, kDruid},
    {GroupKind::DruidDefense, kDruid},
    {GroupKind::DruidCooldowns, kDruid},
    {GroupKind::DruidBearTaunts, kDruid},
    {GroupKind::DruidCures, kDruid},
    {GroupKind::DruidHealCooldowns, kDruid},
    {GroupKind::DruidBearDefense, kDruid},
    {GroupKind::DruidHealDefense, kDruid},
    {GroupKind::HunterStings, kHunter},
    {GroupKind::HunterArea, kHunter},
    {GroupKind::HunterControl, kHunter},
    {GroupKind::HunterAspects, kHunter},
    {GroupKind::HunterTraps, kHunter},
    {GroupKind::HunterCooldowns, kHunter},
    {GroupKind::HunterMeleeDefense, kHunter},
    {GroupKind::MageArea, kMage},
    {GroupKind::MagePolymorphs, kMage},
    {GroupKind::MageBuffs, kMage},
    {GroupKind::MageDefense, kMage},
    {GroupKind::MageCooldowns, kMage},
    {GroupKind::WarlockFire, kWarlock},
    {GroupKind::WarlockArea, kWarlock},
    {GroupKind::WarlockControl, kWarlock},
    {GroupKind::WarlockCurses, kWarlock},
    {GroupKind::WarlockBuffs, kWarlock},
    {GroupKind::WarlockLife, kWarlock},
    {GroupKind::WarlockDrain, kWarlock},
    {GroupKind::PriestDispels, kPriest},
    {GroupKind::PriestBuffs, kPriest},
    {GroupKind::PriestHealDefense, kPriest},
    {GroupKind::PriestHealCooldowns, kPriest},
    {GroupKind::PriestDamageDefense, kPriest},
    {GroupKind::PriestWand, kPriest},
    {GroupKind::PaladinSeals, kPaladin},
    {GroupKind::PaladinHeals, kPaladin},
    {GroupKind::PaladinHoly, kPaladin},
    {GroupKind::PaladinBlessings, kPaladin},
    {GroupKind::PaladinAuras, kPaladin},
    {GroupKind::PaladinDefense, kPaladin},
    {GroupKind::PaladinCooldowns, kPaladin},
    {GroupKind::PaladinCleanse, kPaladin},
    {GroupKind::PaladinHealCooldowns, kPaladin},
    {GroupKind::ShamanEarthTotems, kShaman},
    {GroupKind::ShamanFireTotems, kShaman},
    {GroupKind::ShamanWaterTotems, kShaman},
    {GroupKind::ShamanAirTotems, kShaman},
    {GroupKind::ShamanShocks, kShaman},
    {GroupKind::ShamanWeapons, kShaman},
    {GroupKind::ShamanHeals, kShaman},
    {GroupKind::ShamanCooldowns, kShaman},
    {GroupKind::ShamanFrostShocks, kShaman},
    {GroupKind::ShamanCures, kShaman},
    {GroupKind::ShamanHealShocks, kShaman},
    {GroupKind::ShamanManaTide, kShaman},
    {GroupKind::Racial, 0},
};

// The tables: P, A, B, C, D, E, F, G, H by TouchPlace, then the three consumables.

// The warrior (§7.2): Battle Stance; the base copies it (out of every stance).
constexpr Table kWarriorBattle{
    slot(S(78)),                              // P Heroic Strike
    slot(S(12294), S(23881), S(772)),         // A Mortal Strike > Bloodthirst > Rend
    slot(S(100)),                             // B Charge
    slot(S(772), S(7386)),                    // C Rend > Sunder Armor
    slot(G(GroupKind::WarriorBattleArea)),    // D <WarriorBattleArea>
    slot(S(1715)),                            // E Hamstring
    slot(G(GroupKind::WarriorShouts)),        // F <WarriorShouts>
    slot(G(GroupKind::WarriorBattleDefense)), // G <WarriorBattleDefense>
    slot(G(GroupKind::WarriorCooldowns)),     // H <WarriorCooldowns>
    slot(Use(TouchConsumable::Health)),       // c1 health
    slot(Use(TouchConsumable::Rage)),         // c2 rage
    slot(Use(TouchConsumable::Bandage)),      // c3 bandage
};

// Defensive Stance. Mocking Blow is Battle Stance's (grey here until the stance assist, §8).
constexpr Table kWarriorDefensive{
    slot(S(78)),                           // P Heroic Strike
    slot(S(23922), S(7386)),               // A Shield Slam > Sunder Armor
    slot(G(GroupKind::WarriorTaunts)),     // B <WarriorTaunts>
    slot(S(6572)),                         // C Revenge
    slot(S(2565)),                         // D Shield Block
    slot(S(72)),                           // E Shield Bash
    kInherit,                              // F
    slot(G(GroupKind::WarriorShieldWall)), // G <WarriorShieldWall>
    kInherit,                              // H
    kInherit,                              // c1
    kInherit,                              // c2
    kInherit,                              // c3
};

// Berserker Stance.
constexpr Table kWarriorBerserker{
    slot(S(78)),                                 // P Heroic Strike
    slot(S(23881), S(12294), S(1680)),           // A Bloodthirst > Mortal Strike > Whirlwind
    slot(S(20252)),                              // B Intercept
    slot(S(1680), S(1464)),                      // C Whirlwind > Slam
    slot(S(845)),                                // D Cleave
    slot(S(6552)),                               // E Pummel
    kInherit,                                    // F
    slot(G(GroupKind::WarriorBerserkerDefense)), // G <WarriorBerserkerDefense>
    kInherit,                                    // H
    kInherit,                                    // c1
    kInherit,                                    // c2
    kInherit,                                    // c3
};

// The tank's base: Defensive Stance with Demoralizing Shout leading the shouts.
constexpr Table kWarriorTank{
    slot(S(78)),                           // P Heroic Strike
    slot(S(23922), S(7386)),               // A Shield Slam > Sunder Armor
    slot(G(GroupKind::WarriorTaunts)),     // B <WarriorTaunts>
    slot(S(6572)),                         // C Revenge
    slot(S(2565)),                         // D Shield Block
    slot(S(72)),                           // E Shield Bash
    slot(G(GroupKind::WarriorTankShouts)), // F <WarriorTankShouts>
    slot(G(GroupKind::WarriorShieldWall)), // G <WarriorShieldWall>
    slot(G(GroupKind::WarriorCooldowns)),  // H <WarriorCooldowns>
    slot(Use(TouchConsumable::Health)),    // c1 health
    slot(Use(TouchConsumable::Rage)),      // c2 rage
    slot(Use(TouchConsumable::Bandage)),   // c3 bandage
};

// The rogue (§7.3).
constexpr Table kRogueBase{
    slot(S(16511), S(1752)),                     // P Hemorrhage > Sinister Strike
    slot(S(2098)),                               // A Eviscerate
    slot(S(1784)),                               // B Stealth
    slot(S(5171)),                               // C Slice and Dice
    slot(G(GroupKind::RogueFinishers)),          // D <RogueFinishers>
    slot(G(GroupKind::RogueInterrupts)),         // E <RogueInterrupts>
    slot(G(GroupKind::RogueCooldowns), S(2983)), // F <RogueCooldowns> > Sprint
    slot(G(GroupKind::RogueDefense)),            // G <RogueDefense>
    slot(G(GroupKind::RogueVanish)),             // H <RogueVanish>
    slot(Use(TouchConsumable::Health)),          // c1 health
    slot(Use(TouchConsumable::Energy)),          // c2 energy
    slot(Use(TouchConsumable::Bandage)),         // c3 bandage
};

// Stealth: the openers.
constexpr Table kRogueStealth{
    slot(S(8676), S(1833)),               // P Ambush > Cheap Shot
    slot(S(1833), S(703)),                // A Cheap Shot > Garrote
    slot(G(GroupKind::RogueStealthExit)), // B <RogueStealthExit>
    slot(S(703)),                         // C Garrote
    slot(S(6770)),                        // D Sap
    slot(S(921)),                         // E Pick Pocket
    kInherit,                             // F
    kInherit,                             // G
    slot(S(14183)),                       // H Premeditation
    kInherit,                             // c1
    kInherit,                             // c2
    kInherit,                             // c3
};

// The druid (§7.4): caster form.
constexpr Table kDruidCaster{
    slot(S(5176)),                       // P Wrath
    slot(S(8921)),                       // A Moonfire
    slot(S(770)),                        // B Faerie Fire
    slot(S(5570), S(2912)),              // C Insect Swarm > Starfire
    slot(S(16914)),                      // D Hurricane
    slot(G(GroupKind::DruidControl)),    // E <DruidControl>
    slot(G(GroupKind::DruidBuffs)),      // F <DruidBuffs>
    slot(G(GroupKind::DruidDefense)),    // G <DruidDefense>
    slot(G(GroupKind::DruidCooldowns)),  // H <DruidCooldowns>
    slot(Use(TouchConsumable::Health)),  // c1 health
    slot(Use(TouchConsumable::Mana)),    // c2 mana
    slot(Use(TouchConsumable::Bandage)), // c3 bandage
};

// Cat Form: what it lacks hides (a caster's spell cannot be cast in it).
constexpr Table kDruidCat{
    slot(S(1082), S(5221), kHide),      // P Claw > Shred > nothing
    slot(S(1822), kHide),               // A Rake > nothing
    slot(S(5215), kHide),               // B Prowl > nothing
    slot(S(22568), S(1079), kHide),     // C Ferocious Bite > Rip > nothing
    slot(S(1079), kHide),               // D Rip > nothing
    slot(S(16857), kHide),              // E Faerie Fire (Feral) > nothing
    slot(S(5217), kHide),               // F Tiger's Fury > nothing
    slot(S(1850), kHide),               // G Dash > nothing
    slot(S(8998), kHide),               // H Cower > nothing
    kInherit,                           // c1
    slot(Use(TouchConsumable::Energy)), // c2 energy
    kInherit,                           // c3
};

// Prowl (Cat Form stealthed): the openers, the rest as in Cat Form.
constexpr Table kDruidProwl{
    slot(S(6785), S(9005), Same(TouchContext::Cat)), // P Ravage > Pounce > as in Cat
    slot(S(9005), Same(TouchContext::Cat)),          // A Pounce > as in Cat
    slot(Same(TouchContext::Cat)),                   // B as in Cat
    slot(S(5221), Same(TouchContext::Cat)),          // C Shred > as in Cat
    slot(Same(TouchContext::Cat)),                   // D as in Cat
    slot(Same(TouchContext::Cat)),                   // E as in Cat
    slot(Same(TouchContext::Cat)),                   // F as in Cat
    slot(Same(TouchContext::Cat)),                   // G as in Cat
    slot(Same(TouchContext::Cat)),                   // H as in Cat
    kInherit,                                        // c1
    slot(Same(TouchContext::Cat)),                   // c2 as in Cat
    kInherit,                                        // c3
};

// Bear Form (and Dire Bear Form).
constexpr Table kDruidBear{
    slot(S(6807), kHide),                        // P Maul > nothing
    slot(S(16857), S(99), kHide),                // A Faerie Fire (Feral) > Demoralizing Roar > nothing
    slot(G(GroupKind::DruidBearTaunts), kHide),  // B <DruidBearTaunts> > nothing
    slot(S(99), kHide),                          // C Demoralizing Roar > nothing
    slot(S(779), kHide),                         // D Swipe > nothing
    slot(S(5211), kHide),                        // E Bash > nothing
    slot(S(5229), kHide),                        // F Enrage > nothing
    slot(G(GroupKind::DruidBearDefense), kHide), // G <DruidBearDefense> > nothing
    slot(G(GroupKind::Racial), kHide),           // H <Racial> > nothing
    kInherit,                                    // c1
    slot(Use(TouchConsumable::Rage)),            // c2 rage
    kInherit,                                    // c3
};

// The restoration druid ("Heal").
constexpr Table kDruidHeal{
    slot(S(5185)),                          // P Healing Touch
    slot(S(774)),                           // A Rejuvenation
    slot(S(18562), S(17116)),               // B Swiftmend > Nature's Swiftness
    slot(S(8936)),                          // C Regrowth
    slot(S(740)),                           // D Tranquility
    slot(G(GroupKind::DruidCures)),         // E <DruidCures>
    slot(G(GroupKind::DruidBuffs)),         // F <DruidBuffs>
    slot(G(GroupKind::DruidHealDefense)),   // G <DruidHealDefense>
    slot(G(GroupKind::DruidHealCooldowns)), // H <DruidHealCooldowns>
    slot(Use(TouchConsumable::Health)),     // c1 health
    slot(Use(TouchConsumable::Mana)),       // c2 mana
    slot(Use(TouchConsumable::Bandage)),    // c3 bandage
};

// The hunter (§7.5): at range (Raptor Strike leads without a ranged weapon).
constexpr Table kHunterBase{
    slot(S(75), S(2973)),                      // P Auto Shot > Raptor Strike
    slot(S(19434), S(3044)),                   // A Aimed Shot > Arcane Shot
    slot(S(1130)),                             // B Hunter's Mark
    slot(S(3044), G(GroupKind::HunterStings)), // C Arcane Shot > <HunterStings>
    slot(G(GroupKind::HunterArea)),            // D <HunterArea>
    slot(G(GroupKind::HunterControl)),         // E <HunterControl>
    slot(G(GroupKind::HunterAspects)),         // F <HunterAspects>
    slot(G(GroupKind::HunterTraps)),           // G <HunterTraps>
    slot(G(GroupKind::HunterCooldowns)),       // H <HunterCooldowns>
    slot(Use(TouchConsumable::Health)),        // c1 health
    slot(Use(TouchConsumable::Mana)),          // c2 mana
    slot(Use(TouchConsumable::Bandage)),       // c3 bandage
};

// In melee (the target within reach, app/TouchBarScreens' hysteresis).
constexpr Table kHunterMelee{
    slot(S(2973)),                          // P Raptor Strike
    slot(S(2974)),                          // A Wing Clip
    slot(S(781)),                           // B Disengage
    slot(S(1495)),                          // C Mongoose Bite
    kInherit,                               // D
    slot(S(2974)),                          // E Wing Clip
    kInherit,                               // F
    slot(G(GroupKind::HunterMeleeDefense)), // G <HunterMeleeDefense>
    kInherit,                               // H
    kInherit,                               // c1
    kInherit,                               // c2
    kInherit,                               // c3
};

// The mage (§7.6): Frostbolt for the frost tree (and without talents from level 4), Fireball else.
constexpr Table kMageBase{
    slot(TopOrNone(kTreeFrost, 116), S(133)), // P Frostbolt (Frost tree or none) > Fireball
    slot(S(2136)),                            // A Fire Blast
    slot(G(GroupKind::MagePolymorphs)),       // B <MagePolymorphs>
    // C Cone of Cold (Frost tree) > Arcane Missiles > Scorch > Shoot
    slot(Top(kTreeFrost, 120), S(5143), S(2948), S(5019)),
    slot(G(GroupKind::MageArea)),        // D <MageArea>
    slot(S(2139)),                       // E Counterspell
    slot(G(GroupKind::MageBuffs)),       // F <MageBuffs>
    slot(G(GroupKind::MageDefense)),     // G <MageDefense>
    slot(G(GroupKind::MageCooldowns)),   // H <MageCooldowns>
    slot(Use(TouchConsumable::Health)),  // c1 health
    slot(Use(TouchConsumable::Mana)),    // c2 mana
    slot(Use(TouchConsumable::Bandage)), // c3 bandage
};

// The warlock (§7.7).
constexpr Table kWarlockBase{
    slot(S(686)),                        // P Shadow Bolt
    slot(S(172)),                        // A Corruption
    slot(G(GroupKind::WarlockCurses)),   // B <WarlockCurses>
    slot(G(GroupKind::WarlockFire)),     // C <WarlockFire>
    slot(G(GroupKind::WarlockArea)),     // D <WarlockArea>
    slot(G(GroupKind::WarlockControl)),  // E <WarlockControl>
    slot(G(GroupKind::WarlockBuffs)),    // F <WarlockBuffs>
    slot(G(GroupKind::WarlockLife)),     // G <WarlockLife>
    slot(G(GroupKind::WarlockDrain)),    // H <WarlockDrain>
    slot(Use(TouchConsumable::Health)),  // c1 health
    slot(Use(TouchConsumable::Mana)),    // c2 mana
    slot(Use(TouchConsumable::Bandage)), // c3 bandage
};

// The priest (§7.8): "Heal".
constexpr Table kPriestHeal{
    slot(S(2061), S(2050)),                  // P Flash Heal > Lesser Heal
    slot(S(139)),                            // A Renew
    slot(S(17)),                             // B Power Word: Shield
    slot(S(2060), S(2054)),                  // C Greater Heal > Heal
    slot(S(596)),                            // D Prayer of Healing
    slot(G(GroupKind::PriestDispels)),       // E <PriestDispels>
    slot(G(GroupKind::PriestBuffs)),         // F <PriestBuffs>
    slot(G(GroupKind::PriestHealDefense)),   // G <PriestHealDefense>
    slot(G(GroupKind::PriestHealCooldowns)), // H <PriestHealCooldowns>
    slot(Use(TouchConsumable::Health)),      // c1 health
    slot(Use(TouchConsumable::Mana)),        // c2 mana
    slot(Use(TouchConsumable::Bandage)),     // c3 bandage
};

// Shadowform (both templates: no Holy spell can be cast in it).
constexpr Table kPriestShadowform{
    slot(S(15407)), // P Mind Flay
    slot(S(8092)),  // A Mind Blast
    kInherit,       // B
    slot(S(589)),   // C Shadow Word: Pain
    slot(S(15286)), // D Vampiric Embrace
    kInherit,       // E
    kInherit,       // F
    kInherit,       // G
    kInherit,       // H
    kInherit,       // c1
    kInherit,       // c2
    kInherit,       // c3
};

// The priest: "Damage" (Mind Flay once talented, Smite before).
constexpr Table kPriestDamage{
    slot(S(15407), S(585)),                  // P Mind Flay > Smite
    slot(S(8092)),                           // A Mind Blast
    slot(S(17)),                             // B Power Word: Shield
    slot(S(589)),                            // C Shadow Word: Pain
    slot(S(15237)),                          // D Holy Nova
    slot(S(15487), S(8122)),                 // E Silence > Psychic Scream
    slot(G(GroupKind::PriestBuffs)),         // F <PriestBuffs>
    slot(G(GroupKind::PriestDamageDefense)), // G <PriestDamageDefense>
    slot(G(GroupKind::PriestWand)),          // H <PriestWand>
    slot(Use(TouchConsumable::Health)),      // c1 health
    slot(Use(TouchConsumable::Mana)),        // c2 mana
    slot(Use(TouchConsumable::Bandage)),     // c3 bandage
};

// The paladin (§7.9): "Retribution": the seal is the main button (tap: put it back), the judgement A.
constexpr Table kPaladinRet{
    slot(G(GroupKind::PaladinSeals)),     // P <PaladinSeals>
    slot(S(20271)),                       // A Judgement
    slot(G(GroupKind::PaladinBlessings)), // B <PaladinBlessings>
    slot(G(GroupKind::PaladinHeals)),     // C <PaladinHeals>
    slot(G(GroupKind::PaladinHoly)),      // D <PaladinHoly>
    slot(S(853)),                         // E Hammer of Justice
    slot(G(GroupKind::PaladinAuras)),     // F <PaladinAuras>
    slot(G(GroupKind::PaladinDefense)),   // G <PaladinDefense>
    slot(G(GroupKind::PaladinCooldowns)), // H <PaladinCooldowns>
    slot(Use(TouchConsumable::Health)),   // c1 health
    slot(Use(TouchConsumable::Mana)),     // c2 mana
    slot(Use(TouchConsumable::Bandage)),  // c3 bandage
};

// The paladin: "Heal".
constexpr Table kPaladinHeal{
    slot(S(19750), S(635)),                   // P Flash of Light > Holy Light
    slot(S(635)),                             // A Holy Light
    slot(G(GroupKind::PaladinBlessings)),     // B <PaladinBlessings>
    slot(S(20473)),                           // C Holy Shock
    slot(S(26573)),                           // D Consecration
    slot(G(GroupKind::PaladinCleanse)),       // E <PaladinCleanse>
    slot(G(GroupKind::PaladinAuras)),         // F <PaladinAuras>
    slot(G(GroupKind::PaladinDefense)),       // G <PaladinDefense>
    slot(G(GroupKind::PaladinHealCooldowns)), // H <PaladinHealCooldowns>
    slot(Use(TouchConsumable::Health)),       // c1 health
    slot(Use(TouchConsumable::Mana)),         // c2 mana
    slot(Use(TouchConsumable::Bandage)),      // c3 bandage
};

// The paladin: "Protect".
constexpr Table kPaladinProt{
    slot(G(GroupKind::PaladinSeals)),     // P <PaladinSeals>
    slot(S(20271)),                       // A Judgement
    slot(G(GroupKind::PaladinBlessings)), // B <PaladinBlessings>
    slot(S(20925)),                       // C Holy Shield
    slot(S(26573)),                       // D Consecration
    slot(S(853)),                         // E Hammer of Justice
    slot(G(GroupKind::PaladinAuras)),     // F <PaladinAuras>
    slot(G(GroupKind::PaladinDefense)),   // G <PaladinDefense>
    slot(G(GroupKind::PaladinCooldowns)), // H <PaladinCooldowns>
    slot(Use(TouchConsumable::Health)),   // c1 health
    slot(Use(TouchConsumable::Mana)),     // c2 mana
    slot(Use(TouchConsumable::Bandage)),  // c3 bandage
};

// The shaman (§7.10): "Elemental"; the four totem groups go on the class bar.
constexpr Table kShamanElemental{
    slot(S(403)),                         // P Lightning Bolt
    slot(S(8050)),                        // A Flame Shock
    slot(S(324)),                         // B Lightning Shield
    slot(S(421)),                         // C Chain Lightning
    slot(G(GroupKind::ShamanFireTotems)), // D <ShamanFireTotems>
    slot(G(GroupKind::ShamanShocks)),     // E <ShamanShocks>
    slot(G(GroupKind::ShamanWeapons)),    // F <ShamanWeapons>
    slot(G(GroupKind::ShamanHeals)),      // G <ShamanHeals>
    slot(G(GroupKind::ShamanCooldowns)),  // H <ShamanCooldowns>
    slot(Use(TouchConsumable::Health)),   // c1 health
    slot(Use(TouchConsumable::Mana)),     // c2 mana
    slot(Use(TouchConsumable::Bandage)),  // c3 bandage
};

// The shaman: "Enhancement".
constexpr Table kShamanEnhancement{
    slot(S(17364), S(6603)),               // P Stormstrike > Attack
    slot(S(8042)),                         // A Earth Shock
    slot(S(324)),                          // B Lightning Shield
    slot(S(8050)),                         // C Flame Shock
    slot(S(403)),                          // D Lightning Bolt
    slot(G(GroupKind::ShamanFrostShocks)), // E <ShamanFrostShocks>
    slot(G(GroupKind::ShamanWeapons)),     // F <ShamanWeapons>
    slot(G(GroupKind::ShamanHeals)),       // G <ShamanHeals>
    slot(G(GroupKind::Racial)),            // H <Racial>
    slot(Use(TouchConsumable::Health)),    // c1 health
    slot(Use(TouchConsumable::Mana)),      // c2 mana
    slot(Use(TouchConsumable::Bandage)),   // c3 bandage
};

// The shaman: "Heal".
constexpr Table kShamanHeal{
    slot(S(8004), S(331)),                // P Lesser Healing Wave > Healing Wave
    slot(S(331)),                         // A Healing Wave
    slot(S(324)),                         // B Lightning Shield
    slot(S(1064)),                        // C Chain Heal
    slot(G(GroupKind::ShamanCures)),      // D <ShamanCures>
    slot(G(GroupKind::ShamanHealShocks)), // E <ShamanHealShocks>
    slot(G(GroupKind::ShamanWeapons)),    // F <ShamanWeapons>
    slot(S(16188)),                       // G Nature's Swiftness
    slot(G(GroupKind::ShamanManaTide)),   // H <ShamanManaTide>
    slot(Use(TouchConsumable::Health)),   // c1 health
    slot(Use(TouchConsumable::Mana)),     // c2 mana
    slot(Use(TouchConsumable::Bandage)),  // c3 bandage
};

// The templates of each class, in their order (§5.3: 1-3 by role; the first the one a character without talents
// starts with).
constexpr TemplateDef kTemplates[] = {
    {PresetRole::WarriorArmsFury,
     kWarrior,
     "VD_TOUCH_CLASSES_ARMS_FURY",
     {kTreeArms, kTreeFury},
     {{{TouchContext::Base, &kWarriorBattle},
       {TouchContext::DefensiveStance, &kWarriorDefensive},
       {TouchContext::BerserkerStance, &kWarriorBerserker}}},
     0,
     {}},
    {PresetRole::WarriorTank,
     kWarrior,
     "VD_TOUCH_CLASSES_TANK",
     {kTreeWarriorProtection},
     {{{TouchContext::Base, &kWarriorTank},
       {TouchContext::BattleStance, &kWarriorBattle},
       {TouchContext::BerserkerStance, &kWarriorBerserker}}},
     0,
     {}},
    {PresetRole::Rogue,
     kRogue,
     "VD_TOUCH_CLASSES_SOLO",
     {},
     {{{TouchContext::Base, &kRogueBase}, {TouchContext::Stealth, &kRogueStealth}}},
     0,
     {}},
    {PresetRole::Druid,
     kDruid,
     "VD_TOUCH_CLASSES_SOLO",
     {kTreeBalance, kTreeFeralCombat},
     {{{TouchContext::Base, &kDruidCaster},
       {TouchContext::Cat, &kDruidCat},
       {TouchContext::Prowl, &kDruidProwl},
       {TouchContext::Bear, &kDruidBear}}},
     contextBit(TouchContext::Moonkin),
     {}},
    {PresetRole::DruidHeal,
     kDruid,
     "VD_TOUCH_CLASSES_HEAL",
     {kTreeDruidRestoration},
     {{{TouchContext::Base, &kDruidHeal},
       {TouchContext::Cat, &kDruidCat},
       {TouchContext::Prowl, &kDruidProwl},
       {TouchContext::Bear, &kDruidBear}}},
     contextBit(TouchContext::Moonkin),
     {}},
    {PresetRole::Hunter,
     kHunter,
     "VD_TOUCH_CLASSES_SOLO",
     {},
     {{{TouchContext::Base, &kHunterBase}, {TouchContext::Melee, &kHunterMelee}}},
     0,
     {}},
    {PresetRole::Mage, kMage, "VD_TOUCH_CLASSES_SOLO", {}, {{{TouchContext::Base, &kMageBase}}}, 0, {}},
    {PresetRole::Warlock,
     kWarlock,
     "VD_TOUCH_CLASSES_SOLO",
     {},
     {{{TouchContext::Base, &kWarlockBase}}},
     0,
     {}},
    {PresetRole::PriestHeal,
     kPriest,
     "VD_TOUCH_CLASSES_HEAL",
     {kTreePriestHoly, kTreeDiscipline},
     {{{TouchContext::Base, &kPriestHeal}, {TouchContext::Shadowform, &kPriestShadowform}}},
     0,
     {}},
    {PresetRole::PriestDamage,
     kPriest,
     "VD_TOUCH_CLASSES_DAMAGE",
     {kTreeShadow},
     {{{TouchContext::Base, &kPriestDamage}, {TouchContext::Shadowform, &kPriestShadowform}}},
     0,
     {}},
    {PresetRole::PaladinRetribution,
     kPaladin,
     "VD_TOUCH_CLASSES_RETRIBUTION",
     {kTreeRetribution},
     {{{TouchContext::Base, &kPaladinRet}}},
     0,
     {}},
    {PresetRole::PaladinHeal,
     kPaladin,
     "VD_TOUCH_CLASSES_HEAL",
     {kTreePaladinHoly},
     {{{TouchContext::Base, &kPaladinHeal}}},
     0,
     {}},
    {PresetRole::PaladinProtect,
     kPaladin,
     "VD_TOUCH_CLASSES_PROTECT",
     {kTreePaladinProtection},
     {{{TouchContext::Base, &kPaladinProt}}},
     0,
     {}},
    {PresetRole::ShamanElemental,
     kShaman,
     "VD_TOUCH_CLASSES_ELEMENTAL",
     {kTreeElemental},
     {{{TouchContext::Base, &kShamanElemental}}},
     0,
     {GroupKind::ShamanEarthTotems, GroupKind::ShamanFireTotems, GroupKind::ShamanWaterTotems,
      GroupKind::ShamanAirTotems}},
    {PresetRole::ShamanEnhancement,
     kShaman,
     "VD_TOUCH_CLASSES_ENHANCEMENT",
     {kTreeEnhancement},
     {{{TouchContext::Base, &kShamanEnhancement}}},
     0,
     {GroupKind::ShamanEarthTotems, GroupKind::ShamanFireTotems, GroupKind::ShamanWaterTotems,
      GroupKind::ShamanAirTotems}},
    {PresetRole::ShamanHeal,
     kShaman,
     "VD_TOUCH_CLASSES_HEAL",
     {kTreeShamanRestoration},
     {{{TouchContext::Base, &kShamanHeal}}},
     0,
     {GroupKind::ShamanEarthTotems, GroupKind::ShamanFireTotems, GroupKind::ShamanWaterTotems,
      GroupKind::ShamanAirTotems}},
};

// The demo's forms (the class bar): the stances, the druid's forms, Ghost Wolf.
// Battle Stance, Defensive Stance, Berserker Stance.
constexpr u32 kWarriorForms[] = {2457, 71, 2458};
// Bear Form, Aquatic Form, Cat Form, Travel Form, Dire Bear Form.
constexpr u32 kDruidForms[] = {5487, 1066, 768, 783, 9634};
// Ghost Wolf.
constexpr u32 kShamanForms[] = {2645};

} // namespace

std::span<const TemplateDef> templates() {
    return kTemplates;
}

const TemplateDef* templateOf(PresetRole role) {
    for (const TemplateDef& def : kTemplates) {
        if (def.role == role) {
            return &def;
        }
    }
    return nullptr;
}

std::span<const GroupDef> groups() {
    return kGroups;
}

const GroupDef* groupOf(GroupKind kind) {
    for (const GroupDef& def : kGroups) {
        if (def.kind == kind) {
            return &def;
        }
    }
    return nullptr;
}

u32 groupClass(GroupKind kind) {
    for (const GroupOwner& owner : kGroupOwners) {
        if (owner.kind == kind) {
            return owner.cls;
        }
    }
    return 0;
}

u32 raceMaskOf(u32 spell) {
    for (const RaceSpell& r : kRaceSpells) {
        if (r.spell == spell) {
            return r.races;
        }
    }
    return 0;
}

std::span<const u32> demoExtraSpells(u32 cls) {
    switch (cls) {
    case kWarrior: return kWarriorForms;
    case kDruid: return kDruidForms;
    case kShaman: return kShamanForms;
    default: return {};
    }
}

} // namespace vd::game::presets
