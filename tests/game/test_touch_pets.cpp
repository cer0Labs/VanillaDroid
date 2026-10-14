// Phase 20 (pets, docs/MOBILE_CONTROLS.md §6.8, §5.2 rows 1-3): the touch cluster's pet button and the states
// that take the cluster over. The pet button's decisions (no pet, dead, attacking, not attacking; the
// warlock's demons), a pet action by spell whatever the pet's bar, the controlled unit's layer, the
// no-control rule of vMaNGOS' Spell::CheckCasterAuras with the real Spell.dbc (Will of the Forsaken against a
// fear but not Death Coil's horror, Berserker Rage, Blink, Divine Shield, a druid's forms against Polymorph),
// the death's buttons and Feed Pet's memory (and its place in the layout's file).
#include "game/Abilities.h"
#include "game/AbilityData.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/SpellText.h"
#include "game/TouchLayout.h"
#include "game/TouchPet.h"
#include "net/CombatMessages.h"
#include "net/GameMessages.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace vd;
using namespace vd::game;

namespace {

struct Tables {
    pack::PackReader db;
    GameData data;
    AbilityData abilities;
};

// The client tables (VD_PACKS_DIR), loaded once; nullptr without the packs.
const Tables* tables() {
    static Tables t;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && t.db.open(std::filesystem::path(dir) / "db.vdpk").ok() && t.data.init(t.db) &&
                        t.abilities.init(t.db)
                    ? 1
                    : 2;
    }
    return state == 1 ? &t : nullptr;
}

SpellDetails details(u32 id) {
    SpellDetails d;
    REQUIRE(tables()->abilities.spellText().details(id, d));
    return d;
}

// The pet bar vMaNGOS' CharmInfo::InitPetActionBar lays out (Attack, Follow, Stay, four spells, Aggressive,
// Defensive, Passive), with `spells` in its spell slots.
PetState petBar(std::array<u32, 4> spells) {
    PetState pet;
    pet.guid = 0xF140000000001234ull;
    pet.bar[0] = net::petButton(net::kPetCommandAttack, net::kPetActCommand);
    pet.bar[1] = net::petButton(net::kPetCommandFollow, net::kPetActCommand);
    pet.bar[2] = net::petButton(net::kPetCommandStay, net::kPetActCommand);
    for (u32 i = 0; i < 4; ++i) {
        pet.bar[3 + i] = net::petButton(spells[i], spells[i] ? net::kPetActEnabled : net::kPetActDisabled);
    }
    pet.bar[7] = net::petButton(net::kPetReactAggressive, net::kPetActReaction);
    pet.bar[8] = net::petButton(net::kPetReactDefensive, net::kPetActReaction);
    pet.bar[9] = net::petButton(net::kPetReactPassive, net::kPetActReaction);
    return pet;
}

ControlAura control(u32 spellId) {
    ControlAura c;
    REQUIRE(controlAura(details(spellId), c));
    return c;
}

bool breaks(u32 spellId, std::initializer_list<u32> controls) {
    std::vector<ControlAura> list;
    for (const u32 id : controls) {
        list.push_back(control(id));
    }
    return breaksControl(spellImmunity(details(spellId)), list);
}

} // namespace

TEST_CASE("touch pets: pet actions by slot, command, reaction and spell") {
    CHECK(decodeTouchPetAction(3).kind == TouchPetKind::Slot);
    CHECK(decodeTouchPetAction(touchPetCommand(net::kPetCommandStay)).kind == TouchPetKind::Command);
    CHECK(decodeTouchPetAction(touchPetCommand(net::kPetCommandStay)).value == net::kPetCommandStay);
    CHECK(decodeTouchPetAction(touchPetReaction(net::kPetReactPassive)).kind == TouchPetKind::Reaction);
    CHECK(decodeTouchPetAction(19647).kind == TouchPetKind::Spell);
    CHECK(decodeTouchPetAction(19647).value == 19647);

    // A felhunter's bar: Spell Lock rank 2 on its fifth button; the commands and reactions where the server
    // put them.
    PetState pet = petBar({19505, 19647, 0, 0}); // Devour Magic, Spell Lock
    CHECK(touchPetSlot(pet, 3, nullptr) == 3);
    CHECK(touchPetSlot(pet, 5, nullptr) == -1); // an empty spell slot
    CHECK(touchPetSlot(pet, 19647, nullptr) == 4);
    CHECK(touchPetSlot(pet, touchPetCommand(net::kPetCommandFollow), nullptr) == 1);
    CHECK(touchPetSlot(pet, touchPetReaction(net::kPetReactPassive), nullptr) == 9);
    // The player moved Stay elsewhere: the command follows it.
    std::swap(pet.bar[2], pet.bar[6]);
    CHECK(touchPetSlot(pet, touchPetCommand(net::kPetCommandStay), nullptr) == 6);
    // Another demon out: nothing does it.
    const PetState imp = petBar({3110, 0, 0, 0}); // Firebolt
    CHECK(touchPetSlot(imp, 19647, nullptr) == -1);
    CHECK(touchPetSlot(PetState{}, 0, nullptr) == -1);

    if (const Tables* t = tables()) {
        // Spell Lock rank 1 kept in a slot: the felhunter's rank 2 casts (same name).
        const SpellText* text = &t->abilities.spellText();
        CHECK(touchPetSlot(petBar({19505, 19647, 0, 0}), 19244, text) == 4);
        const std::array<u32, 3> book = {19505, 19647, 54053};
        CHECK(touchPetBookSpell(book, 19244, text) == 19647);
        CHECK(touchPetBookSpell(book, 3110, text) == 0);
    }
}

TEST_CASE("touch pets: the player's pet spells") {
    const Tables* t = tables();
    if (!t) {
        MESSAGE("VD_PACKS_DIR not set: skipped");
        return;
    }
    // A hunter: Call Pet, Revive Pet, Feed Pet, Dismiss Pet, two ranks of Mend Pet, Eyes of the Beast, Tame
    // Beast.
    const std::vector<u32> hunter = {883, 982, 6991, 2641, 136, 3111, 1002, 1515, 75};
    PetSpellBook spells;
    scanPetSpells(hunter, t->data, spells);
    CHECK(spells.callPet == 883);
    CHECK(spells.revivePet == 982);
    CHECK(spells.feedPet == 6991);
    CHECK(spells.dismissPet == 2641);
    CHECK(spells.demonCount == 0);
    REQUIRE(spells.onPetCount == 2); // Mend Pet (its highest rank), Eyes of the Beast
    CHECK(spells.onPet[0] == 3111);
    CHECK(spells.onPet[1] == 1002);
    // A warlock: the demons by the level they come at, Health Funnel on the pet, Eye of Kilrogg not a pet.
    const std::vector<u32> warlock = {691, 688, 712, 697, 755, 126, 686};
    scanPetSpells(warlock, t->data, spells);
    REQUIRE(spells.demonCount == 4);
    CHECK(spells.demons[0] == 688); // Imp
    CHECK(spells.demons[1] == 697); // Voidwalker
    CHECK(spells.demons[2] == 712); // Succubus
    CHECK(spells.demons[3] == 691); // Felhunter
    CHECK(spells.callPet == 0);
    REQUIRE(spells.onPetCount == 1);
    CHECK(spells.onPet[0] == 755);
}

TEST_CASE("touch pets: the pet button's decisions") {
    PetSpellBook hunter;
    hunter.callPet = 883;
    hunter.revivePet = 982;
    PetButtonState s;
    s.spells = &hunter;
    // No pet: Call Pet; known dead (seen dying and gone, PETTAME_DEAD): Revive Pet.
    CHECK(choosePetButton(s).mode == PetButtonMode::CallPet);
    CHECK(choosePetButton(s).spellId == 883);
    s.knownDead = true;
    CHECK(choosePetButton(s).mode == PetButtonMode::RevivePet);
    CHECK(choosePetButton(s).spellId == 982);
    // Out and lying dead: Revive Pet.
    s.knownDead = false;
    s.petOut = true;
    s.petDead = true;
    CHECK(choosePetButton(s).mode == PetButtonMode::RevivePet);
    // Out and alive: attack the target; already attacking it: come back; no target: come back. A hold: the
    // panel.
    s.petDead = false;
    s.target = 0x42;
    PetButtonChoice c = choosePetButton(s);
    CHECK(c.mode == PetButtonMode::Command);
    CHECK(c.command == net::kPetCommandAttack);
    CHECK(c.target == 0x42);
    CHECK(c.panel);
    s.petTarget = 0x42;
    CHECK(choosePetButton(s).command == net::kPetCommandFollow);
    s.petTarget = 0x77; // attacking something else: sent at the target
    CHECK(choosePetButton(s).command == net::kPetCommandAttack);
    s.target = 0;
    CHECK(choosePetButton(s).command == net::kPetCommandFollow);
    // A level 1 hunter: nothing to show.
    PetButtonState none;
    PetSpellBook empty;
    none.spells = &empty;
    CHECK(choosePetButton(none).mode == PetButtonMode::Hidden);
    // A warlock: the last demon (if still known), else the one learned last; a fan when there are several.
    PetSpellBook warlock;
    warlock.demons = {688, 697, 712, 691};
    warlock.demonCount = 4;
    PetButtonState w;
    w.spells = &warlock;
    c = choosePetButton(w);
    CHECK(c.mode == PetButtonMode::SummonDemon);
    CHECK(c.spellId == 691);
    CHECK(c.fan);
    w.lastDemon = 697;
    CHECK(choosePetButton(w).spellId == 697);
    w.lastDemon = 12345; // not known (another character's file)
    CHECK(choosePetButton(w).spellId == 691);
    warlock.demonCount = 1;
    CHECK(!choosePetButton(w).fan);
    CHECK(choosePetButton(w).spellId == 688);
    // A demon died: no Revive Pet for warlocks; the button summons again once it is gone.
    w.petOut = true;
    w.petDead = true;
    CHECK(choosePetButton(w).mode == PetButtonMode::Hidden);
}

TEST_CASE("touch pets: the controlled unit's layer") {
    // Mind Control on a creature with two spells (PossessSpellInitialize: Attack, then its spells as
    // ACT_PASSIVE).
    PetState possessed;
    possessed.guid = 0xF130000000000042ull;
    possessed.bar[0] = net::petButton(net::kPetCommandAttack, net::kPetActCommand);
    possessed.bar[1] = net::petButton(11977, net::kPetActPassive); // Rend
    possessed.bar[2] = net::petButton(3391, net::kPetActPassive);  // Thrash
    for (u32 i = 3; i < 10; ++i) {
        possessed.bar[i] = net::petButton(0, net::kPetActPassive);
    }
    std::array<i32, kTouchSkills> slots = controlledSlots(possessed, nullptr, nullptr);
    CHECK(slots[touchIndex(TouchPlace::Main)] == 0);
    CHECK(slots[touchIndex(TouchPlace::RingA)] == 1);
    CHECK(slots[touchIndex(TouchPlace::RingC)] == 2);
    CHECK(slots[touchIndex(TouchPlace::RingB)] == -1);
    // Eyes of the Beast: the pet's own bar; Follow, Stay and the reactions are of no use, a passive spell is
    // left out.
    const PetState wolf = petBar({17253, 16827, 2649, 23099}); // Bite, Claw, Growl, Dash
    const auto passive = [](const void*, u32 spellId) { return spellId == 2649; };
    slots = controlledSlots(wolf, passive, nullptr);
    CHECK(slots[touchIndex(TouchPlace::Main)] == 0);
    CHECK(slots[touchIndex(TouchPlace::RingA)] == 3);
    CHECK(slots[touchIndex(TouchPlace::RingC)] == 4);
    CHECK(slots[touchIndex(TouchPlace::RingD)] == 6);
    CHECK(slots[touchIndex(TouchPlace::RingE)] == -1);
    CHECK(controlledSlots(PetState{}, nullptr, nullptr)[0] == -1);
}

TEST_CASE("touch pets: what breaks the control (Spell::CheckCasterAuras)") {
    if (!tables()) {
        MESSAGE("VD_PACKS_DIR not set: skipped");
        return;
    }
    // The control auras: Fear (fear), Hammer of Justice (stun), Polymorph (confuse), Death Coil (a fear
    // effect of the horror mechanic), Seduction (a stun of the charm mechanic), Sap (a stun, knockout).
    CHECK(control(5782).effects[0].kind == ControlAura::kFear);
    CHECK(control(853).effects[0].kind == ControlAura::kStun);
    CHECK(control(118).effects[0].kind == ControlAura::kConfuse);
    ControlAura c;
    CHECK(!controlAura(details(133), c)); // Fireball controls nothing
    // Will of the Forsaken: charm, fear, sleep.
    CHECK(breaks(7744, {5782}));
    CHECK(breaks(7744, {6358}));       // Seduction (charm)
    CHECK(breaks(7744, {2637}));       // Hibernate (sleep)
    CHECK(!breaks(7744, {6789}));      // Death Coil's horror
    CHECK(!breaks(7744, {853}));       // a stun
    CHECK(!breaks(7744, {5782, 853})); // feared and stunned: the stun still holds
    // Berserker Rage: fear and knockout (Intimidating Shout's fear, Sap), not a stun.
    CHECK(breaks(18499, {5246}));
    CHECK(breaks(18499, {6770}));
    CHECK(!breaks(18499, {853}));
    // Blink out of a stun; Divine Shield and Ice Block out of anything of their schools.
    CHECK(breaks(1953, {853}));
    CHECK(!breaks(1953, {5782}));
    CHECK(breaks(642, {853}));
    CHECK(breaks(642, {5782}));
    CHECK(breaks(11958, {7922})); // Charge's stun
    // A druid's form against Polymorph (its mechanic), not against a fear.
    CHECK(breaks(768, {118}));
    CHECK(!breaks(768, {5782}));
    // Without the attribute (SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT) nothing: Fireball, a mechanic immunity of
    // an enemy's (Faerie Fire's dispel immunity does not cover a stun).
    CHECK(!breaks(133, {5782}));
    CHECK(!breaks(770, {853}));
    CHECK(!breaksControl(spellImmunity(details(7744)), {}));
}

TEST_CASE("touch pets: the death's buttons") {
    DeathState d;
    CHECK(!deathButtons(d, false, 0).active);
    d.dead = true;
    DeathButtons b = deathButtons(d, false, 0);
    CHECK(b.active);
    CHECK(b.main == DeathAction::Release);
    CHECK(b.reactive == DeathAction::None);
    d.selfResSpell = 20707; // a soulstone
    b = deathButtons(d, false, 0);
    CHECK(b.reactive == DeathAction::SelfRes);
    CHECK(b.selfResSpell == 20707);
    // A ghost away from its corpse: nothing; by it: Resurrect once the corpse delay is over.
    d.ghost = true;
    b = deathButtons(d, false, 12000);
    CHECK(b.active);
    CHECK(b.main == DeathAction::None);
    CHECK(b.reactive == DeathAction::None);
    b = deathButtons(d, true, 12000);
    CHECK(b.main == DeathAction::Reclaim);
    CHECK(b.mainWaitMs == 12000);
    // A resurrection offered (with the corpse delay when it waits for it).
    d.resurrector = 0x9;
    d.resurrectDelayed = true;
    b = deathButtons(d, true, 5000);
    CHECK(b.ringA == DeathAction::AcceptRes);
    CHECK(b.ringAWaitMs == 5000);
    d.resurrectDelayed = false;
    CHECK(deathButtons(d, true, 5000).ringAWaitMs == 0);
}

TEST_CASE("touch pets: Feed Pet's memory") {
    TouchPetFood memory;
    constexpr u32 kWolf = 1, kBoar = 5;
    rememberPetFood(memory, kWolf, 2287, true);  // Haunch of Meat
    rememberPetFood(memory, kWolf, 3770, true);  // Mutton Chop: the last first
    rememberPetFood(memory, kWolf, 4536, false); // Shiny Red Apple: wolves do not eat fruit
    rememberPetFood(memory, kBoar, 4536, true);  // boars do
    CHECK(petFoodAccepted(memory, kWolf, 3770) == 0);
    CHECK(petFoodAccepted(memory, kWolf, 2287) == 1);
    CHECK(petFoodAccepted(memory, kWolf, 4536) == -1);
    CHECK(petFoodRefused(memory, kWolf, 4536));
    CHECK(!petFoodRefused(memory, kBoar, 4536));
    CHECK(memory.families[0].family == kBoar); // the last used first
    // A refused food accepted later (the pet grew into it): taken out of the refused.
    rememberPetFood(memory, kWolf, 4536, true);
    CHECK(!petFoodRefused(memory, kWolf, 4536));
    rememberPetFood(memory, kWolf, 4536, false);
    CHECK(petFoodAccepted(memory, kWolf, 4536) == -1);
    // The list: the accepted first (the last first), then the foods, then the trade goods, the highest level
    // first; the refused left out.
    std::array<PetFoodItem, 6> items = {{
        {4536, 5, 5, true},    // refused
        {769, 4, 5, false},    // Chunk of Boar Meat: a trade good
        {2287, 3, 10, true},   // accepted (second)
        {117, 10, 1, true},    // Tough Jerky: a food
        {3770, 2, 25, true},   // accepted (last)
        {2589, 20, 10, false}, // Linen Cloth: a trade good
    }};
    const u32 n = sortPetFood(memory, kWolf, items);
    REQUIRE(n == 5);
    CHECK(items[0].entry == 3770);
    CHECK(items[1].entry == 2287);
    CHECK(items[2].entry == 117);
    CHECK(items[3].entry == 2589);
    CHECK(items[4].entry == 769);
    // Many families: the oldest makes room.
    for (u32 f = 10; f < 10 + TouchPetFood::kFamilies; ++f) {
        rememberPetFood(memory, f, 1000 + f, true);
    }
    CHECK(petFoodAccepted(memory, kBoar, 4536) == -1);
    // Feed Pet's refusals as build 5875 numbers them (TouchPetScreens).
    CHECK(std::string_view(net::spellCastResultName(31)) == "FOOD_LOWLEVEL");
    CHECK(std::string_view(net::spellCastResultName(125)) == "WRONG_PET_FOOD");
}

TEST_CASE("touch pets: the last demon and the foods in the layout's file") {
    TouchLayout layout;
    layout.templates[0] = makeTouchTemplate("Solo");
    layout.count = 1;
    layout.lastDemon = 691;
    layout.templates[0].layers[0][touchIndex(TouchPlace::ColumnG)] = {TouchActionKind::PetAction,
                                                                      kTouchManual, 0, 19647};
    rememberPetFood(layout.petFood, 1, 2287, true);
    rememberPetFood(layout.petFood, 1, 4536, false);
    rememberPetFood(layout.petFood, 7, 117, true);
    const std::string text = formatTouchLayout(layout);
    CHECK(text.find("demon 691") != std::string::npos);
    TouchLayout back;
    REQUIRE(parseTouchLayout(text, back));
    CHECK(back.lastDemon == 691);
    CHECK(back.templates[0].layers[0][touchIndex(TouchPlace::ColumnG)].id == 19647);
    CHECK(petFoodAccepted(back.petFood, 1, 2287) == 0);
    CHECK(petFoodRefused(back.petFood, 1, 4536));
    CHECK(petFoodAccepted(back.petFood, 7, 117) == 0);
    CHECK(formatTouchLayout(back) == text);
}
