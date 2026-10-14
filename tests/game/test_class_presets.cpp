// Phase 20 (docs/MOBILE_CONTROLS.md §7, §10.3): the class presets of the touch cluster and the usability rules the
// cluster needs. The presets are built from the real Spell.dbc, SkillLineAbility.dbc and Talent.dbc (VD_PACKS_DIR):
// each slot takes the first candidate the player knows that no earlier slot took, the talents and the weapons pick
// among them, a manual slot is never touched, a refill marks what came "New". The usability rules: stealth, combat,
// the target's creature type, the equipped item, reagents, totems and ammunition, with the real spells' rows.
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/ClassPresets.h"
#include "game/GameData.h"
#include "game/TouchLayout.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace vd;
using namespace vd::game;
namespace uf = net::uf;

namespace {

struct Tables {
    pack::PackReader db;
    AbilityData abilities;
    GameData data;
};

// The client tables (VD_PACKS_DIR), loaded once; nullptr without the packs.
const Tables* tables() {
    static Tables t;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && t.db.open(std::filesystem::path(dir) / "db.vdpk").ok() && t.abilities.init(t.db) &&
                        t.data.init(t.db)
                    ? 1
                    : 2;
    }
    return state == 1 ? &t : nullptr;
}

#define REQUIRE_TABLES(var)                                                                                  \
    const Tables* var = tables();                                                                            \
    if (!var) {                                                                                              \
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");                                          \
        return;                                                                                              \
    }

constexpr u32 kWarrior = 1, kPaladin = 2, kHunter = 3, kRogue = 4, kPriest = 5, kShaman = 7, kMage = 8,
              kWarlock = 9, kDruid = 11;
constexpr u32 kHuman = 1, kOrc = 2, kDwarf = 3, kNightElf = 4, kUndead = 5, kTauren = 6, kGnome = 7,
              kTroll = 8;

constexpr u32 slot(TouchPlace p) {
    return touchIndex(p);
}
constexpr u32 layer(TouchContext c) {
    return static_cast<u32>(c);
}

struct Weapons {
    i32 main[2] = {2, 7}; // a sword
    i32 off[2] = {-1, 0};
    i32 ranged[2] = {-1, 0};
};
constexpr Weapons kSword{};
constexpr Weapons kSwordShield{{2, 7}, {4, 6}, {-1, 0}};
constexpr Weapons kDagger{{2, 15}, {2, 7}, {-1, 0}};
constexpr Weapons kStaffWand{{2, 10}, {-1, 0}, {2, 19}};
constexpr Weapons kStaff{{2, 10}, {-1, 0}, {-1, 0}};
constexpr Weapons kAxeBow{{2, 1}, {-1, 0}, {2, 2}};
constexpr Weapons kAxe{{2, 1}, {-1, 0}, {-1, 0}};

// A character of `cls` at `level` (classDemoKnown: the presets' spells it would know, `tree`'s talents).
struct Character {
    std::vector<u32> known;
    PresetInput in;
};
Character character(const Tables& t, u32 cls, u32 race, u32 level, u32 tree, const Weapons& w) {
    Character c;
    classDemoKnown(cls, race, level, tree, t.abilities, c.known);
    std::sort(c.known.begin(), c.known.end());
    c.in.cls = cls;
    c.in.race = race;
    c.in.level = level;
    c.in.known = c.known;
    c.in.text = &t.abilities.spellText();
    c.in.abilities = &t.abilities;
    c.in.data = &t.data;
    c.in.mainHandClass = w.main[0];
    c.in.mainHandSubclass = w.main[1];
    c.in.offHandClass = w.off[0];
    c.in.offHandSubclass = w.off[1];
    c.in.rangedClass = w.ranged[0];
    c.in.rangedSubclass = w.ranged[1];
    return c;
}

u32 tree(const Tables& t, u32 cls, const char* name) {
    const u32 id = talentTreeByName(cls, name, t.abilities);
    REQUIRE(id != 0);
    return id;
}

TouchLayout layoutOf(const Character& c) {
    TouchLayout out;
    REQUIRE(buildClassLayout(c.in, out));
    return out;
}

// What a slot shows in a layer: a spell's id (its first rank), a group's current spell, 0 nothing.
u32 shown(const TouchTemplate& t, TouchContext context, TouchPlace place) {
    const TouchAction a = resolveTouchAction(t, context, slot(place));
    if (a.kind == TouchActionKind::Spell) {
        return a.id;
    }
    if (a.kind == TouchActionKind::Group && a.id < kTouchGroups) {
        const TouchGroup& g = t.groups[a.id];
        return g.count > 0 ? g.actions[g.current].id : 0;
    }
    return 0;
}

// The spells of a group a slot shows (empty: not a group).
std::vector<u32> members(const TouchTemplate& t, TouchContext context, TouchPlace place) {
    const TouchAction a = resolveTouchAction(t, context, slot(place));
    std::vector<u32> out;
    if (a.kind == TouchActionKind::Group && a.id < kTouchGroups) {
        for (u32 i = 0; i < t.groups[a.id].count; ++i) {
            out.push_back(t.groups[a.id].actions[i].id);
        }
    }
    return out;
}

bool has(const std::vector<u32>& v, u32 id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

// Every spell a template shows in a layer (groups' members too): nothing twice.
void checkNoDuplicates(const TouchTemplate& t, TouchContext context, const SpellText& text) {
    std::map<std::string, int> seen;
    for (u32 s = 0; s < kTouchSkills; ++s) {
        const TouchAction a = resolveTouchAction(t, context, s);
        std::vector<u32> ids;
        if (a.kind == TouchActionKind::Spell) {
            ids.push_back(a.id);
        } else if (a.kind == TouchActionKind::Group) {
            for (u32 i = 0; i < t.groups[a.id].count; ++i) {
                ids.push_back(t.groups[a.id].actions[i].id);
            }
        }
        for (const u32 id : ids) {
            const std::string name(text.name(id));
            INFO("layer " << touchContextName(context) << " spell " << name);
            CHECK(++seen[name] == 1);
        }
    }
}

} // namespace

TEST_CASE("class presets: the warrior's stances, its talents and its shield") {
    REQUIRE_TABLES(t);
    // Level 10, no shield: Heroic Strike, Rend in A (Mortal Strike is a talent of 40), Sunder Armor in C (Rend taken).
    const TouchLayout l10 = layoutOf(character(*t, kWarrior, kHuman, 10, tree(*t, kWarrior, "arms"), kSword));
    REQUIRE(l10.count == 2); // "Arms/Fury" and "Tank"
    const TouchTemplate& dps = l10.templates[0];
    CHECK(dps.role == static_cast<u8>(PresetRole::WarriorArmsFury));
    CHECK(shown(dps, TouchContext::Base, TouchPlace::Main) == 78);      // Heroic Strike
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingA) == 772);    // Rend
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingC) == 7386);   // Sunder Armor
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingD) == 6343);   // Thunder Clap
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingE) == 1715);   // Hamstring
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingB) == 100);    // Charge
    CHECK(shown(dps, TouchContext::Base, TouchPlace::ColumnF) == 6673); // Battle Shout
    CHECK(shown(dps, TouchContext::Base, TouchPlace::ColumnH) == 2687); // Bloodrage
    // Battle Stance is the base itself; Defensive Stance: Sunder Armor in A (no Shield Slam), the base's Sunder
    // Armor in C hidden there (nothing twice), Shield Block and Shield Bash need a shield: the base shows.
    CHECK(dps.layers[layer(TouchContext::BattleStance)][slot(TouchPlace::Main)].empty());
    CHECK(shown(dps, TouchContext::BattleStance, TouchPlace::RingA) == 772);
    CHECK(shown(dps, TouchContext::DefensiveStance, TouchPlace::RingA) == 7386);
    CHECK(shown(dps, TouchContext::DefensiveStance, TouchPlace::RingC) == 0);
    CHECK(shown(dps, TouchContext::DefensiveStance, TouchPlace::RingE) == 1715); // Hamstring: the base's
    CHECK(shown(dps, TouchContext::DefensiveStance, TouchPlace::RingB) == 355);  // Taunt
    checkNoDuplicates(dps, TouchContext::DefensiveStance, t->abilities.spellText());

    // Level 60: Arms takes Mortal Strike, Fury Bloodthirst (the same A, §7.1); a shield brings Shield Bash, Block.
    const TouchLayout arms =
        layoutOf(character(*t, kWarrior, kHuman, 60, tree(*t, kWarrior, "arms"), kSwordShield));
    CHECK(arms.active == 0);
    CHECK(shown(arms.templates[0], TouchContext::Base, TouchPlace::RingA) == 12294);
    CHECK(shown(arms.templates[0], TouchContext::BerserkerStance, TouchPlace::RingA) == 12294);
    CHECK(shown(arms.templates[0], TouchContext::BerserkerStance, TouchPlace::RingC) == 1680);  // Whirlwind
    CHECK(shown(arms.templates[0], TouchContext::BerserkerStance, TouchPlace::RingE) == 6552);  // Pummel
    CHECK(shown(arms.templates[0], TouchContext::BerserkerStance, TouchPlace::RingB) == 20252); // Intercept
    CHECK(shown(arms.templates[0], TouchContext::DefensiveStance, TouchPlace::RingD) == 2565); // Shield Block
    CHECK(shown(arms.templates[0], TouchContext::DefensiveStance, TouchPlace::RingE) == 72);   // Shield Bash
    CHECK(has(members(arms.templates[0], TouchContext::Base, TouchPlace::RingD), 12292)); // Sweeping Strikes
    const TouchLayout fury =
        layoutOf(character(*t, kWarrior, kHuman, 60, tree(*t, kWarrior, "fury"), kSword));
    CHECK(shown(fury.templates[0], TouchContext::Base, TouchPlace::RingA) == 23881);
    CHECK(has(members(fury.templates[0], TouchContext::Base, TouchPlace::ColumnH), 12328)); // Death Wish
    // A protection warrior starts on "Tank", whose base is Defensive Stance with Shield Slam.
    const TouchLayout prot =
        layoutOf(character(*t, kWarrior, kHuman, 60, tree(*t, kWarrior, "protection"), kSwordShield));
    CHECK(prot.active == 1);
    CHECK(shown(prot.templates[1], TouchContext::Base, TouchPlace::RingA) == 23922);
    CHECK(shown(prot.templates[1], TouchContext::Base, TouchPlace::RingC) == 6572);   // Revenge
    CHECK(shown(prot.templates[1], TouchContext::Base, TouchPlace::ColumnF) == 1160); // Demoralizing Shout
    CHECK(shown(prot.templates[1], TouchContext::BattleStance, TouchPlace::RingB) == 100); // Charge
    for (const TouchContext c : {TouchContext::Base, TouchContext::BattleStance,
                                 TouchContext::DefensiveStance, TouchContext::BerserkerStance}) {
        checkNoDuplicates(arms.templates[0], c, t->abilities.spellText());
        checkNoDuplicates(prot.templates[1], c, t->abilities.spellText());
    }
}

TEST_CASE("class presets: the rogue's stealth, its dagger and its talents") {
    REQUIRE_TABLES(t);
    const TouchLayout l10 = layoutOf(character(*t, kRogue, kHuman, 10, tree(*t, kRogue, "combat"), kDagger));
    REQUIRE(l10.count == 1);
    const TouchTemplate& r10 = l10.templates[0];
    CHECK(shown(r10, TouchContext::Base, TouchPlace::Main) == 1752);     // Sinister Strike
    CHECK(shown(r10, TouchContext::Base, TouchPlace::RingA) == 2098);    // Eviscerate
    CHECK(shown(r10, TouchContext::Base, TouchPlace::RingC) == 5171);    // Slice and Dice
    CHECK(shown(r10, TouchContext::Base, TouchPlace::RingB) == 1784);    // Stealth
    CHECK(shown(r10, TouchContext::Base, TouchPlace::RingE) == 1776);    // Gouge (Kick comes at 12)
    CHECK(shown(r10, TouchContext::Base, TouchPlace::ColumnF) == 2983);  // Sprint: no talent of the group yet
    CHECK(shown(r10, TouchContext::Stealth, TouchPlace::RingE) == 921);  // Pick Pocket
    CHECK(shown(r10, TouchContext::Stealth, TouchPlace::RingD) == 6770); // Sap

    // Level 40 with a dagger: Ambush opens; at 30 without one Cheap Shot does, and Garrote moves up to A.
    const TouchLayout dagger =
        layoutOf(character(*t, kRogue, kHuman, 40, tree(*t, kRogue, "subtlety"), kDagger));
    const TouchTemplate& d = dagger.templates[0];
    CHECK(shown(d, TouchContext::Stealth, TouchPlace::Main) == 8676);
    CHECK(shown(d, TouchContext::Stealth, TouchPlace::RingA) == 1833);
    CHECK(shown(d, TouchContext::Stealth, TouchPlace::RingC) == 703);
    CHECK(shown(d, TouchContext::Base, TouchPlace::Main) == 16511);       // Hemorrhage (subtlety's fifth row)
    CHECK(shown(d, TouchContext::Stealth, TouchPlace::ColumnH) == 14183); // Premeditation
    CHECK(members(d, TouchContext::Stealth, TouchPlace::RingB) ==
          std::vector<u32>{1784, 1725}); // Stealth, Distract
    const TouchLayout sword = layoutOf(character(*t, kRogue, kHuman, 30, tree(*t, kRogue, "combat"), kSword));
    const TouchTemplate& s = sword.templates[0];
    CHECK(shown(s, TouchContext::Stealth, TouchPlace::Main) == 1833);
    CHECK(shown(s, TouchContext::Stealth, TouchPlace::RingA) == 703);
    CHECK(shown(s, TouchContext::Stealth, TouchPlace::RingC) ==
          5171); // Garrote taken: the base's Slice and Dice
    CHECK(shown(s, TouchContext::Base, TouchPlace::Main) == 1752);
    CHECK(shown(s, TouchContext::Base, TouchPlace::ColumnF) == 13877); // Blade Flurry (alone: no fan)
    checkNoDuplicates(s, TouchContext::Stealth, t->abilities.spellText());
    checkNoDuplicates(d, TouchContext::Stealth, t->abilities.spellText());
}

TEST_CASE("class presets: the mage's tree picks Frostbolt or Fireball, the wand's Shoot fills in") {
    REQUIRE_TABLES(t);
    // Without talents: Fireball until Frostbolt comes at 4; Shoot with a wand before Arcane Missiles (8).
    const TouchTemplate m3 = layoutOf(character(*t, kMage, kGnome, 3, 0, kStaffWand)).templates[0];
    CHECK(shown(m3, TouchContext::Base, TouchPlace::Main) == 133);
    const TouchTemplate m6 = layoutOf(character(*t, kMage, kGnome, 6, 0, kStaffWand)).templates[0];
    CHECK(shown(m6, TouchContext::Base, TouchPlace::Main) == 116);
    CHECK(shown(m6, TouchContext::Base, TouchPlace::RingC) == 5019);
    const TouchTemplate m6staff = layoutOf(character(*t, kMage, kGnome, 6, 0, kStaff)).templates[0];
    CHECK(shown(m6staff, TouchContext::Base, TouchPlace::RingC) == 0);
    // Level 60: frost keeps Frostbolt and puts Cone of Cold in C; fire turns to Fireball and Arcane Missiles.
    const TouchTemplate frost =
        layoutOf(character(*t, kMage, kGnome, 60, tree(*t, kMage, "frost"), kStaffWand)).templates[0];
    CHECK(shown(frost, TouchContext::Base, TouchPlace::Main) == 116);
    CHECK(shown(frost, TouchContext::Base, TouchPlace::RingC) == 120);
    CHECK_FALSE(has(members(frost, TouchContext::Base, TouchPlace::RingD), 120)); // not twice
    const TouchTemplate fire =
        layoutOf(character(*t, kMage, kGnome, 60, tree(*t, kMage, "fire"), kStaffWand)).templates[0];
    CHECK(shown(fire, TouchContext::Base, TouchPlace::Main) == 133);
    CHECK(shown(fire, TouchContext::Base, TouchPlace::RingC) == 5143);
    CHECK(has(members(fire, TouchContext::Base, TouchPlace::RingD), 120));
    CHECK(has(members(fire, TouchContext::Base, TouchPlace::ColumnH), 11129)); // Combustion
    // Ice Armor stands for Frost Armor once known (one member, the better one); the gnome's Escape Artist in G.
    const std::vector<u32> buffs = members(fire, TouchContext::Base, TouchPlace::ColumnF);
    CHECK(has(buffs, 7302));
    CHECK_FALSE(has(buffs, 168));
    CHECK(has(members(fire, TouchContext::Base, TouchPlace::ColumnG), 20589));
}

TEST_CASE("class presets: the priest's templates, its race's spells and Shadowform") {
    REQUIRE_TABLES(t);
    const TouchLayout human =
        layoutOf(character(*t, kPriest, kHuman, 40, tree(*t, kPriest, "holy"), kStaffWand));
    REQUIRE(human.count == 2); // "Heal", "Damage"
    CHECK(human.active == 0);
    const TouchTemplate& heal = human.templates[0];
    CHECK(shown(heal, TouchContext::Base, TouchPlace::Main) == 2061);  // Flash Heal
    CHECK(shown(heal, TouchContext::Base, TouchPlace::RingA) == 139);  // Renew
    CHECK(shown(heal, TouchContext::Base, TouchPlace::RingC) == 2060); // Greater Heal
    CHECK(shown(heal, TouchContext::Base, TouchPlace::RingB) == 17);   // Power Word: Shield
    const std::vector<u32> h = members(heal, TouchContext::Base, TouchPlace::ColumnH);
    CHECK(has(h, 13908));       // Desperate Prayer (human)
    CHECK(has(h, 13896));       // Feedback (human)
    CHECK(has(h, 20600));       // Perception
    CHECK_FALSE(has(h, 10797)); // no Starshards for a human
    // A night elf's: Starshards and Elune's Grace, and Shadowmeld.
    const TouchLayout elf =
        layoutOf(character(*t, kPriest, kNightElf, 40, tree(*t, kPriest, "holy"), kStaffWand));
    const std::vector<u32> e = members(elf.templates[0], TouchContext::Base, TouchPlace::ColumnH);
    CHECK(has(e, 10797));
    CHECK(has(e, 2651));
    CHECK(has(e, 20580));
    // A shadow priest starts on "Damage": Mind Flay (the talent) over Smite, its Shadowform layer.
    const TouchLayout shadow =
        layoutOf(character(*t, kPriest, kUndead, 60, tree(*t, kPriest, "shadow"), kStaffWand));
    CHECK(shadow.active == 1);
    const TouchTemplate& damage = shadow.templates[1];
    CHECK(shown(damage, TouchContext::Base, TouchPlace::Main) == 15407);
    CHECK(shown(damage, TouchContext::Base, TouchPlace::RingA) == 8092);        // Mind Blast
    CHECK(shown(damage, TouchContext::Base, TouchPlace::RingE) == 15487);       // Silence
    CHECK(shown(damage, TouchContext::Shadowform, TouchPlace::RingD) == 15286); // Vampiric Embrace
    CHECK(has(members(damage, TouchContext::Base, TouchPlace::ColumnH), 2944)); // Devouring Plague (undead)
    CHECK(shown(damage, TouchContext::Base, TouchPlace::ColumnG) == 586);       // Fade (PW: Shield is in B)
    checkNoDuplicates(damage, TouchContext::Shadowform, t->abilities.spellText());
}

TEST_CASE("class presets: the druid's forms, Prowl and the hidden caster spells") {
    REQUIRE_TABLES(t);
    const TouchLayout l = layoutOf(character(*t, kDruid, kTauren, 40, tree(*t, kDruid, "feral"), kStaff));
    REQUIRE(l.count == 2); // "Solo", "Heal"
    const TouchTemplate& solo = l.templates[0];
    CHECK(shown(solo, TouchContext::Base, TouchPlace::Main) == 5176);     // Wrath
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::Main) == 1082);      // Claw
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::RingA) == 1822);     // Rake
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::RingC) == 22568);    // Ferocious Bite
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::RingD) == 1079);     // Rip
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::RingE) == 16857);    // Faerie Fire (Feral)
    CHECK(shown(solo, TouchContext::Cat, TouchPlace::RingB) == 5215);     // Prowl
    CHECK(shown(solo, TouchContext::Prowl, TouchPlace::Main) == 6785);    // Ravage
    CHECK(shown(solo, TouchContext::Prowl, TouchPlace::RingA) == 9005);   // Pounce
    CHECK(shown(solo, TouchContext::Prowl, TouchPlace::RingC) == 5221);   // Shred
    CHECK(shown(solo, TouchContext::Prowl, TouchPlace::RingD) == 1079);   // Rip: as in Cat Form
    CHECK(shown(solo, TouchContext::Prowl, TouchPlace::ColumnH) == 8998); // Cower
    CHECK(shown(solo, TouchContext::Bear, TouchPlace::Main) == 6807);     // Maul
    CHECK(shown(solo, TouchContext::Bear, TouchPlace::RingA) == 16857);   // Faerie Fire (Feral)
    // War Stomp cannot be cast shapeshifted in 1.12 (SPELL_ATTR_NOT_SHAPESHIFT): nothing there in Bear Form.
    CHECK(resolveTouchAction(solo, TouchContext::Bear, slot(TouchPlace::ColumnH)).empty());
    CHECK(has(members(solo, TouchContext::Base, TouchPlace::ColumnH), 20549)); // in caster form it is
    // Moonkin Form keeps the base (no layer of its own by default, §7.4).
    CHECK_FALSE(solo.layerEnabled(TouchContext::Moonkin));
    // Level 12: no Cat Form spells yet: the layer hides what it lacks instead of showing the caster's spells.
    const TouchTemplate low = layoutOf(character(*t, kDruid, kNightElf, 12, 0, kStaff)).templates[0];
    CHECK(shown(low, TouchContext::Bear, TouchPlace::Main) == 6807);
    CHECK(resolveTouchAction(low, TouchContext::Bear, slot(TouchPlace::RingE)).empty()); // Bash comes at 14
    CHECK(low.layers[layer(TouchContext::Bear)][slot(TouchPlace::RingE)].hidden());
    CHECK(shown(low, TouchContext::Bear, TouchPlace::ColumnH) == 0); // no racial for a night elf in bear form
    // The consumables follow the form's power: energy as a cat, rage as a bear.
    CHECK(resolveTouchAction(solo, TouchContext::Cat, slot(TouchPlace::Consumable2)).id ==
          static_cast<u32>(TouchConsumable::Energy));
    CHECK(resolveTouchAction(solo, TouchContext::Bear, slot(TouchPlace::Consumable2)).id ==
          static_cast<u32>(TouchConsumable::Rage));
    CHECK(resolveTouchAction(solo, TouchContext::Base, slot(TouchPlace::Consumable2)).id ==
          static_cast<u32>(TouchConsumable::Mana));
    // "Heal" for a restoration druid.
    const TouchLayout resto =
        layoutOf(character(*t, kDruid, kNightElf, 60, tree(*t, kDruid, "restoration"), kStaff));
    CHECK(resto.active == 1);
    CHECK(shown(resto.templates[1], TouchContext::Base, TouchPlace::Main) == 5185);   // Healing Touch
    CHECK(shown(resto.templates[1], TouchContext::Base, TouchPlace::RingB) == 18562); // Swiftmend
    CHECK(members(resto.templates[1], TouchContext::Base, TouchPlace::RingE) == std::vector<u32>{2782, 2893});
}

TEST_CASE("class presets: the hunter at range and in melee") {
    REQUIRE_TABLES(t);
    const TouchTemplate h =
        layoutOf(character(*t, kHunter, kDwarf, 30, tree(*t, kHunter, "survival"), kAxeBow)).templates[0];
    CHECK(shown(h, TouchContext::Base, TouchPlace::Main) == 75);     // Auto Shot
    CHECK(shown(h, TouchContext::Base, TouchPlace::RingA) == 3044);  // Arcane Shot (no Aimed Shot)
    CHECK(shown(h, TouchContext::Base, TouchPlace::RingC) == 1978);  // the stings: Serpent Sting
    CHECK(shown(h, TouchContext::Base, TouchPlace::RingB) == 1130);  // Hunter's Mark
    CHECK(shown(h, TouchContext::Melee, TouchPlace::Main) == 2973);  // Raptor Strike
    CHECK(shown(h, TouchContext::Melee, TouchPlace::RingA) == 2974); // Wing Clip
    CHECK(shown(h, TouchContext::Melee, TouchPlace::RingC) == 1495); // Mongoose Bite
    CHECK(shown(h, TouchContext::Melee, TouchPlace::RingE) ==
          5116); // the base's Concussive Shot (Wing Clip in A)
    CHECK(shown(h, TouchContext::Melee, TouchPlace::ColumnG) == 19263);     // Deterrence (survival)
    CHECK(has(members(h, TouchContext::Base, TouchPlace::ColumnG), 20594)); // Stoneform (dwarf)
    CHECK(has(members(h, TouchContext::Base, TouchPlace::ColumnF), 13165)); // Aspect of the Hawk
    const TouchTemplate mm =
        layoutOf(character(*t, kHunter, kOrc, 60, tree(*t, kHunter, "marksmanship"), kAxeBow)).templates[0];
    CHECK(shown(mm, TouchContext::Base, TouchPlace::RingA) == 19434); // Aimed Shot
    CHECK(shown(mm, TouchContext::Base, TouchPlace::RingC) == 3044);
    // Without a ranged weapon Auto Shot cannot be: Raptor Strike leads.
    const TouchTemplate melee = layoutOf(character(*t, kHunter, kOrc, 10, 0, kAxe)).templates[0];
    CHECK(shown(melee, TouchContext::Base, TouchPlace::Main) == 2973);
}

TEST_CASE("class presets: the paladin's seal is its main button, the shaman's totems are its class bar") {
    REQUIRE_TABLES(t);
    const TouchLayout pal =
        layoutOf(character(*t, kPaladin, kDwarf, 30, tree(*t, kPaladin, "retribution"), kSword));
    REQUIRE(pal.count == 3); // "Retribution", "Heal", "Protect"
    const TouchTemplate& ret = pal.templates[0];
    CHECK(shown(ret, TouchContext::Base, TouchPlace::Main) == 20154);         // Seal of Righteousness
    CHECK(has(members(ret, TouchContext::Base, TouchPlace::Main), 21082));    // Seal of the Crusader
    CHECK(has(members(ret, TouchContext::Base, TouchPlace::Main), 20375));    // Seal of Command (the talent)
    CHECK(shown(ret, TouchContext::Base, TouchPlace::RingA) == 20271);        // Judgement
    CHECK(shown(ret, TouchContext::Base, TouchPlace::RingE) == 853);          // Hammer of Justice
    CHECK(shown(ret, TouchContext::Base, TouchPlace::ColumnF) == 465);        // the auras: Devotion Aura
    CHECK(has(members(ret, TouchContext::Base, TouchPlace::ColumnG), 20594)); // Stoneform (dwarf)
    const TouchTemplate& protect =
        layoutOf(character(*t, kPaladin, kHuman, 60, tree(*t, kPaladin, "protection"), kSwordShield))
            .templates[2];
    CHECK(shown(protect, TouchContext::Base, TouchPlace::RingC) == 20925); // Holy Shield
    CHECK(
        has(members(protect, TouchContext::Base, TouchPlace::ColumnG), 642)); // Divine Shield over Protection
    CHECK_FALSE(has(members(protect, TouchContext::Base, TouchPlace::ColumnG), 498));

    const TouchLayout sha =
        layoutOf(character(*t, kShaman, kOrc, 30, tree(*t, kShaman, "elemental"), kSwordShield));
    REQUIRE(sha.count == 3); // "Elemental", "Enhancement", "Heal"
    const TouchTemplate& ele = sha.templates[0];
    // Four groups on the class bar, Earth / Fire / Water / Air; the Fire one is also D.
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(ele.classBarGroups[i] != 0);
    }
    const TouchGroup& fire = ele.groups[ele.classBarGroups[1] - 1u];
    CHECK(fire.actions[0].id == 3599); // Searing Totem
    const TouchAction d = resolveTouchAction(ele, TouchContext::Base, slot(TouchPlace::RingD));
    CHECK(d.kind == TouchActionKind::Group);
    CHECK(d.id == ele.classBarGroups[1] - 1u);
    CHECK(ele.groups[ele.classBarGroups[0] - 1u].count ==
          5); // Stoneskin, Earthbind, Stoneclaw, Strength, Tremor
    CHECK(shown(ele, TouchContext::Base, TouchPlace::Main) == 403);   // Lightning Bolt
    CHECK(shown(ele, TouchContext::Base, TouchPlace::RingE) == 8042); // Earth Shock
    CHECK(shown(sha.templates[1], TouchContext::Base, TouchPlace::Main) ==
          6603); // Attack (no Stormstrike yet)
    CHECK(shown(sha.templates[2], TouchContext::Base, TouchPlace::Main) == 8004); // Lesser Healing Wave
}

TEST_CASE("class presets: the warlock's curses, its wand and its demons' spells") {
    REQUIRE_TABLES(t);
    const TouchTemplate w =
        layoutOf(character(*t, kWarlock, kUndead, 40, tree(*t, kWarlock, "destruction"), kStaffWand))
            .templates[0];
    CHECK(shown(w, TouchContext::Base, TouchPlace::Main) == 686);         // Shadow Bolt
    CHECK(shown(w, TouchContext::Base, TouchPlace::RingA) == 172);        // Corruption
    CHECK(shown(w, TouchContext::Base, TouchPlace::RingC) == 348);        // Immolate
    CHECK(has(members(w, TouchContext::Base, TouchPlace::RingC), 17877)); // Shadowburn
    CHECK(has(members(w, TouchContext::Base, TouchPlace::RingC), 17962)); // Conflagrate
    CHECK(shown(w, TouchContext::Base, TouchPlace::RingB) == 980);        // Curse of Agony
    CHECK(members(w, TouchContext::Base, TouchPlace::RingB).size() ==
          5); // Agony .. Elements (no Shadow, Doom yet)
    CHECK(has(members(w, TouchContext::Base, TouchPlace::ColumnF), 706)); // Demon Armor over Demon Skin
    CHECK_FALSE(has(members(w, TouchContext::Base, TouchPlace::ColumnF), 687));
    CHECK(has(members(w, TouchContext::Base, TouchPlace::ColumnH), 5019)); // Shoot
    CHECK(has(members(w, TouchContext::Base, TouchPlace::ColumnG), 7744)); // Will of the Forsaken
}

TEST_CASE("class presets: every class at 10, 30 and 60 fills its main button, nothing twice") {
    REQUIRE_TABLES(t);
    for (const u32 cls : {kWarrior, kPaladin, kHunter, kRogue, kPriest, kShaman, kMage, kWarlock, kDruid}) {
        std::array<TalentTabInfo, 3> tabs{};
        const u32 n = t->abilities.talentTabs(cls, tabs);
        for (const u32 level : {10u, 30u, 60u}) {
            for (u32 i = 0; i < n; ++i) {
                const u32 race = cls == kShaman || cls == kHunter ? kOrc : cls == kDruid ? kTauren : kHuman;
                const Weapons& w = cls == kHunter                      ? kAxeBow
                                   : cls == kWarrior || cls == kShaman ? kSwordShield
                                   : cls == kRogue                     ? kDagger
                                                                       : kStaffWand;
                const TouchLayout l = layoutOf(character(*t, cls, race, level, tabs[i].id, w));
                for (u32 k = 0; k < l.count; ++k) {
                    INFO("class " << cls << " level " << level << " tree " << tabs[i].id << " template "
                                  << k);
                    CHECK(shown(l.templates[k], TouchContext::Base, TouchPlace::Main) != 0);
                    for (u32 c = 0; c < kTouchContexts; ++c) {
                        checkNoDuplicates(l.templates[k], static_cast<TouchContext>(c),
                                          t->abilities.spellText());
                    }
                    // The file keeps it whole.
                    TouchLayout back;
                    REQUIRE(parseTouchLayout(formatTouchLayout(l), back));
                    CHECK(back.templates[k].groups[0].count == l.templates[k].groups[0].count);
                    CHECK(back.templates[k].layers == l.templates[k].layers);
                }
            }
        }
    }
}

TEST_CASE("class presets: a refill marks what came New and never touches the player's own") {
    REQUIRE_TABLES(t);
    // A warrior at 9 learns level 10's spells: Sunder Armor comes into C ("New"); the inheriting layers say so too.
    Character before = character(*t, kWarrior, kHuman, 9, 0, kSword);
    TouchLayout l = layoutOf(before);
    TouchTemplate& dps = l.templates[0];
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingC) == 0);
    // The player put Battle Shout in G by hand, and chose Demoralizing Shout... (a manual slot).
    dps.layers[0][slot(TouchPlace::ColumnG)] = TouchAction::spell(6673, kTouchManual);
    const Character after = character(*t, kWarrior, kHuman, 14, 0, kSword);
    const PresetChanges changes = refreshClassTemplate(after.in, dps);
    CHECK(shown(dps, TouchContext::Base, TouchPlace::RingC) == 7386);
    CHECK((changes.slots[layer(TouchContext::Base)] & (1u << slot(TouchPlace::RingC))) != 0);
    CHECK((changes.slots[layer(TouchContext::BattleStance)] & (1u << slot(TouchPlace::RingC))) != 0);
    CHECK((changes.slots[layer(TouchContext::Base)] & (1u << slot(TouchPlace::Main))) ==
          0); // Heroic Strike stays
    // The manual slot is left alone, and its spell is not put twice: F's shouts lose Battle Shout.
    CHECK(dps.layers[0][slot(TouchPlace::ColumnG)] == TouchAction::spell(6673, kTouchManual));
    CHECK_FALSE(has(members(dps, TouchContext::Base, TouchPlace::ColumnF), 6673));
    CHECK(shown(dps, TouchContext::Base, TouchPlace::ColumnF) == 1160); // Demoralizing Shout alone
    // Nothing new: nothing marked.
    const PresetChanges again = refreshClassTemplate(after.in, dps);
    CHECK_FALSE(again.any());
}

TEST_CASE("class presets: a group keeps the member the player chose") {
    REQUIRE_TABLES(t);
    Character c = character(*t, kPaladin, kHuman, 30, 0, kSword);
    TouchLayout l = layoutOf(c);
    TouchTemplate& ret = l.templates[0];
    const TouchAction seals = resolveTouchAction(ret, TouchContext::Base, slot(TouchPlace::Main));
    REQUIRE(seals.kind == TouchActionKind::Group);
    TouchGroup& g = ret.groups[seals.id];
    REQUIRE(g.count >= 3);
    g.current = 2; // the player picked the third seal from the fan
    const u32 chosen = g.actions[2].id;
    const Character later = character(*t, kPaladin, kHuman, 40, 0, kSword);
    refreshClassTemplate(later.in, ret);
    CHECK(ret.groups[seals.id].actions[ret.groups[seals.id].current].id == chosen);
    // The auras' group, untouched by the player, follows its first member.
    const TouchAction auras = resolveTouchAction(ret, TouchContext::Base, slot(TouchPlace::ColumnF));
    REQUIRE(auras.kind == TouchActionKind::Group);
    CHECK(ret.groups[auras.id].current == 0);
    // Back to the class preset drops the choice.
    REQUIRE(resetClassTemplate(later.in, ret));
    CHECK(ret.groups[resolveTouchAction(ret, TouchContext::Base, slot(TouchPlace::Main)).id].current == 0);
}

TEST_CASE("class presets: the roles, the trees by name, no preset for an unknown class") {
    REQUIRE_TABLES(t);
    std::array<PresetRole, 4> roles{};
    CHECK(classPresetRoles(kPaladin, roles) == 3);
    CHECK(roles[0] == PresetRole::PaladinRetribution);
    CHECK(presetRoleNameKey(PresetRole::ShamanHeal) == "VD_TOUCH_CLASSES_HEAL");
    CHECK(talentTreeByName(kHunter, "beastmastery", t->abilities) == 361);
    CHECK(talentTreeByName(kHunter, "beast", t->abilities) == 361);
    CHECK(talentTreeByName(kDruid, "Feral", t->abilities) == 281);
    CHECK(talentTreeByName(kMage, "shadow", t->abilities) == 0);
    Character c = character(*t, kWarrior, kHuman, 10, 0, kSword);
    c.in.cls = 6; // no class 6 in 1.12
    TouchLayout out;
    CHECK_FALSE(buildClassLayout(c.in, out));
}

// --- The usability rules (§10.3) ----------------------------------------------------------------------------

namespace {

constexpr u64 kPlayerGuid = 0x11;
constexpr u64 kOtherPlayer = 0x12;
constexpr u64 kCreature = 0xF130000000000456ull;

Entity unit(u64 guid, std::vector<std::pair<u16, u32>> fields) {
    Entity e;
    e.guid = guid;
    e.type = (guid >> 48) == 0 ? ObjectType::Player : ObjectType::Unit;
    e.values.assign(uf::PLAYER_END, 0);
    e.values[uf::UNIT_FIELD_HEALTH] = 100;
    for (const auto& [index, value] : fields) {
        e.values[index] = value;
    }
    return e;
}

struct Bags {
    std::map<u32, u32> items;
    SpellNeedsState state(std::span<const WornItem> worn, u32 creatureType = 0) const {
        SpellNeedsState s;
        s.worn = worn;
        s.targetCreatureType = creatureType;
        s.context = this;
        s.carried = [](const void* context, u32 entry) {
            const auto& items = static_cast<const Bags*>(context)->items;
            const auto it = items.find(entry);
            return it != items.end() ? it->second : 0u;
        };
        return s;
    }
};

struct Spell {
    SpellInfo info;
    SpellDetails details;
    SpellRequirements needs;
};
Spell spell(const Tables& t, u32 id) {
    Spell s;
    const std::optional<SpellInfo> info = t.data.spell(id);
    const std::optional<SpellRequirements> needs = t.data.requirements(id);
    REQUIRE(info);
    REQUIRE(needs);
    REQUIRE(t.abilities.spellText().details(id, s.details));
    s.info = *info;
    s.needs = *needs;
    return s;
}

SpellNeed need(const Spell& s, const Entity& me, const Entity* target, const SpellNeedsState& state) {
    return spellNeedMissing(s.info, &s.details, s.needs, me, target, state);
}

std::array<WornItem, kWornSlots> worn(std::initializer_list<std::pair<u32, WornItem>> items) {
    std::array<WornItem, kWornSlots> out{};
    for (const auto& [slotIndex, item] : items) {
        out[slotIndex] = item;
    }
    return out;
}
WornItem item(u32 entry, i32 itemClass, i32 subclass, u32 count = 1) {
    WornItem w;
    w.entry = entry;
    w.itemClass = itemClass;
    w.subclass = subclass;
    w.count = count;
    return w;
}

} // namespace

TEST_CASE("usability: stealth, combat and a peaceful target") {
    REQUIRE_TABLES(t);
    const Bags bags;
    const std::array<WornItem, kWornSlots> dagger = worn({{15, item(1, 2, 15)}});
    const SpellNeedsState state = bags.state(dagger);
    const Entity cat = unit(kPlayerGuid, {{uf::UNIT_FIELD_BYTES_1, 1u << 16}}); // Cat Form
    const Entity prowling = unit(kPlayerGuid, {{uf::UNIT_FIELD_BYTES_1, (1u << 16) | (2u << 24)}}); // + creep
    const Entity wolf = unit(kCreature, {});
    // Ravage and Pounce (SPELL_ATTR_ONLY_STEALTHED): Prowl's creep flag, which Cat Form alone has not.
    CHECK(need(spell(*t, 6785), cat, &wolf, state) == SpellNeed::Stealth);
    CHECK(need(spell(*t, 6785), prowling, &wolf, state) == SpellNeed::Met);
    CHECK(need(spell(*t, 9005), cat, &wolf, state) == SpellNeed::Stealth);
    // Shadowmeld's stealth counts too (any SPELL_AURA_MOD_STEALTH): a night elf's Ambush from it.
    const Entity shadowmelded = unit(kPlayerGuid, {{uf::UNIT_FIELD_BYTES_1, 2u << 24}});
    CHECK(need(spell(*t, 8676), shadowmelded, &wolf, state) == SpellNeed::Met);
    // Stealth and Charge not in combat (SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL).
    const Entity fighting = unit(kPlayerGuid, {{uf::UNIT_FIELD_FLAGS, kUnitFlagInCombat}});
    const Entity calm = unit(kPlayerGuid, {});
    CHECK(need(spell(*t, 1784), fighting, nullptr, state) == SpellNeed::NotInCombat);
    CHECK(need(spell(*t, 1784), calm, nullptr, state) == SpellNeed::Met);
    CHECK(need(spell(*t, 100), fighting, &wolf, state) == SpellNeed::NotInCombat);
    // Sap: its target not in combat (SPELL_ATTR_EX_ONLY_PEACEFUL_TARGETS).
    const Entity angryWolf = unit(kCreature, {{uf::UNIT_FIELD_FLAGS, kUnitFlagInCombat}});
    const Spell sap = spell(*t, 6770);
    CHECK(need(sap, shadowmelded, &angryWolf, bags.state(dagger, 7)) == SpellNeed::PeacefulTarget);
    CHECK(need(sap, shadowmelded, &wolf, bags.state(dagger, 7)) == SpellNeed::Met);
    // spellUsable greys it with the rest.
    const ActionUsable u = spellUsable(sap.info, &sap.details, shadowmelded, &angryWolf, 0, nullptr, nullptr,
                                       &sap.needs, &state);
    CHECK_FALSE(u.usable);
}

TEST_CASE("usability: the target's creature type") {
    REQUIRE_TABLES(t);
    const Bags bags;
    const Entity me = unit(kPlayerGuid, {});
    const Entity creature = unit(kCreature, {});
    const Spell exorcism = spell(*t, 879);                                              // undead and demons
    CHECK(need(exorcism, me, &creature, bags.state({}, 6)) == SpellNeed::Met);          // undead
    CHECK(need(exorcism, me, &creature, bags.state({}, 3)) == SpellNeed::Met);          // demon
    CHECK(need(exorcism, me, &creature, bags.state({}, 7)) == SpellNeed::CreatureType); // humanoid
    CHECK(need(exorcism, me, &creature, bags.state({}, 0)) == SpellNeed::Met);          // not known yet
    CHECK(need(exorcism, me, nullptr, bags.state({}, 7)) == SpellNeed::Met);            // no target
    // Players are humanoids, beasts in an animal form (Hibernate on a druid in Cat Form).
    const Entity human = unit(kOtherPlayer, {});
    const Entity catDruid = unit(kOtherPlayer, {{uf::UNIT_FIELD_BYTES_1, 1u << 16}});
    CHECK(need(exorcism, me, &human, bags.state({})) == SpellNeed::CreatureType);
    const Spell hibernate = spell(*t, 2637);
    CHECK(need(hibernate, me, &human, bags.state({})) == SpellNeed::CreatureType);
    CHECK(need(hibernate, me, &catDruid, bags.state({})) == SpellNeed::Met);
    CHECK(need(spell(*t, 9484), me, &creature, bags.state({}, 6)) == SpellNeed::Met); // Shackle Undead
    CHECK(need(spell(*t, 9484), me, &creature, bags.state({}, 1)) == SpellNeed::CreatureType); // a beast
    CHECK(need(spell(*t, 710), me, &creature, bags.state({}, 4)) == SpellNeed::Met); // Banish, elemental
    // Curse of Doom: never a player.
    CHECK(need(spell(*t, 603), me, &human, bags.state({})) == SpellNeed::CreatureType);
    CHECK(need(spell(*t, 603), me, &creature, bags.state({}, 7)) == SpellNeed::Met);
}

TEST_CASE("usability: the equipped item, the reagents, the totems and the ammunition") {
    REQUIRE_TABLES(t);
    Bags bags;
    const Entity me = unit(kPlayerGuid, {});
    const Entity wolf = unit(kCreature, {});
    // Backstab: a dagger in the main hand (one in the off hand does not count, GetWeaponAttackType).
    const Spell backstab = spell(*t, 53);
    CHECK(need(backstab, me, &wolf, bags.state(worn({{15, item(1, 2, 15)}}))) == SpellNeed::Met);
    CHECK(need(backstab, me, &wolf, bags.state(worn({{15, item(1, 2, 7)}, {16, item(2, 2, 15)}}))) ==
          SpellNeed::EquippedItem);
    WornItem unknown;
    unknown.entry = 5;
    unknown.unknown = true;
    CHECK(need(backstab, me, &wolf, bags.state(worn({{15, unknown}}))) ==
          SpellNeed::Met); // its query on the way
    WornItem broken = item(1, -1, 0);
    CHECK(need(backstab, me, &wolf, bags.state(worn({{15, broken}}))) == SpellNeed::EquippedItem);
    // Shield Bash and Shield Wall: a shield in the off hand.
    CHECK(need(spell(*t, 72), me, &wolf, bags.state(worn({{15, item(1, 2, 7)}}))) == SpellNeed::EquippedItem);
    CHECK(need(spell(*t, 72), me, &wolf, bags.state(worn({{15, item(1, 2, 7)}, {16, item(2, 4, 6)}}))) ==
          SpellNeed::Met);
    // Shoot: a wand in the ranged slot.
    CHECK(need(spell(*t, 5019), me, &wolf, bags.state(worn({{17, item(3, 2, 19)}}))) == SpellNeed::Met);
    CHECK(need(spell(*t, 5019), me, &wolf, bags.state(worn({{17, item(3, 2, 2)}}))) ==
          SpellNeed::EquippedItem);
    // Shadowburn: a soul shard (6265); its count on the button.
    const Spell shadowburn = spell(*t, 17877);
    const std::array<WornItem, kWornSlots> none{};
    CHECK(need(shadowburn, me, &wolf, bags.state(none)) == SpellNeed::Reagents);
    CHECK(spellShownCount(shadowburn.info, shadowburn.needs, me, bags.state(none)) == 0);
    bags.items[6265] = 3;
    CHECK(need(shadowburn, me, &wolf, bags.state(none)) == SpellNeed::Met);
    CHECK(spellShownCount(shadowburn.info, shadowburn.needs, me, bags.state(none)) == 3);
    // Searing Totem: the Fire Totem (5176) carried.
    const Spell searing = spell(*t, 3599);
    CHECK(need(searing, me, nullptr, bags.state(none)) == SpellNeed::Totem);
    bags.items[5176] = 1;
    CHECK(need(searing, me, nullptr, bags.state(none)) == SpellNeed::Met);
    // Auto Shot: a bow and its arrows (PLAYER_AMMO_ID); the arrows' count on the button.
    const Spell autoShot = spell(*t, 75);
    const std::array<WornItem, kWornSlots> bow = worn({{17, item(9, 2, 2)}});
    const Entity noAmmo = unit(kPlayerGuid, {});
    const Entity archer = unit(kPlayerGuid, {{uf::PLAYER_AMMO_ID, 2512}});
    CHECK(need(autoShot, noAmmo, &wolf, bags.state(bow)) == SpellNeed::Ammo);
    CHECK(need(autoShot, archer, &wolf, bags.state(bow)) == SpellNeed::Ammo); // none carried
    bags.items[2512] = 200;
    CHECK(need(autoShot, archer, &wolf, bags.state(bow)) == SpellNeed::Met);
    CHECK(spellShownCount(autoShot.info, autoShot.needs, archer, bags.state(bow)) == 200);
    // A thrown weapon is its own ammunition (Throw).
    const std::array<WornItem, kWornSlots> knives = worn({{17, item(7, 2, 16, 150)}});
    CHECK(need(spell(*t, 2764), me, &wolf, bags.state(knives)) == SpellNeed::Met);
    CHECK(spellShownCount(spell(*t, 2764).info, spell(*t, 2764).needs, me, bags.state(knives)) == 150);
    // Without the bags (the classic bars before this phase): not checked.
    CHECK(need(shadowburn, me, &wolf, SpellNeedsState{}) == SpellNeed::Met);
}

TEST_CASE("usability: a player's creature type is its form's") {
    CHECK(playerCreatureType(0) == 7);
    CHECK(playerCreatureType(1) == 1);  // Cat Form
    CHECK(playerCreatureType(16) == 1); // Ghost Wolf
    CHECK(playerCreatureType(17) == 7); // Battle Stance
}

TEST_CASE("class presets: the cost of a build and of a refill (desktop)") {
    REQUIRE_TABLES(t);
    // On the main thread when the known spells or the weapons change (never per frame): a level 60 shaman's three
    // templates with the class bar's totems, the heaviest.
    const Character c = character(*t, kShaman, kOrc, 60, tree(*t, kShaman, "elemental"), kSwordShield);
    TouchLayout l;
    constexpr int kRuns = 50;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        REQUIRE(buildClassLayout(c.in, l));
    }
    const auto built = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        for (u32 k = 0; k < l.count; ++k) {
            refreshClassTemplate(c.in, l.templates[k]);
        }
    }
    const auto refilled = std::chrono::steady_clock::now();
    const auto us = [](auto a, auto b) {
        return std::chrono::duration<double, std::micro>(b - a).count() / kRuns;
    };
    MESSAGE("buildClassLayout (3 templates): " << us(start, built)
                                               << " us; refresh of the 3: " << us(built, refilled) << " us");
    CHECK(us(built, refilled) < 2000.0);
}

TEST_CASE("usability: the cost of the checks of a frame's buttons (desktop)") {
    REQUIRE_TABLES(t);
    Bags bags;
    bags.items[6265] = 5;
    const std::array<WornItem, kWornSlots> gear = worn({{15, item(1, 2, 15)}, {17, item(9, 2, 2)}});
    const SpellNeedsState state = bags.state(gear, 7);
    const Entity me = unit(kPlayerGuid, {{uf::PLAYER_AMMO_ID, 2512}});
    const Entity wolf = unit(kCreature, {});
    std::vector<Spell> spells;
    for (const u32 id : {53u, 72u, 75u, 879u, 6770u, 17877u, 3599u, 1784u, 8676u, 5019u, 2764u, 603u}) {
        spells.push_back(spell(*t, id));
    }
    constexpr int kFrames = 2000;
    u32 met = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        for (int k = 0; k < 6; ++k) { // 72 buttons: the main bar, an extra bar, the cluster and its fan
            for (const Spell& s : spells) {
                met += need(s, me, &wolf, state) == SpellNeed::Met ? 1u : 0u;
            }
        }
    }
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kFrames;
    MESSAGE("spellNeedMissing for 72 buttons: " << us << " us per frame (" << met << ")");
    CHECK(us < 100.0);
}
