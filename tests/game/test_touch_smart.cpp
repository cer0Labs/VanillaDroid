// Phase 20 (smart, docs/MOBILE_CONTROLS.md §6.1-6.3): the touch cluster's smart buttons. An item's on-use
// spell in the real Spell.dbc says what it restores (a potion, a healthstone, a mana gem, food, drink, a
// bandage; Well Fed food apart); the consumable buttons choose among synthetic bags (the healthstone before a
// potion in combat, eating and drinking out of it, conjured first, what restores most, what the player can
// use and is ready); the mount button rides the fastest mount, falls back on the class's travel form where no
// mount goes and gets off; the reactive button offers what an aura state or Overpower's combo point makes
// usable, the longest cooldown first.
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/Consumables.h"
#include "game/GameData.h"
#include "game/Mounts.h"
#include "game/ReactiveSpells.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

using namespace vd;
using namespace vd::game;
namespace uf = net::uf;

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

// What spell `id` restores, as an item's on-use spell (`itemCategory`: the item query's category).
ConsumableUse classify(u32 id, u32 itemCategory = 0) {
    const Tables* t = tables();
    const std::optional<SpellInfo> info = t->data.spell(id);
    SpellDetails details;
    REQUIRE(info);
    REQUIRE(t->abilities.spellText().details(id, details));
    return classifyConsumable(*info, details, itemCategory);
}

SpellDetails details(u32 id) {
    SpellDetails d;
    REQUIRE(tables()->abilities.spellText().details(id, d));
    return d;
}

ConsumableItem item(u32 entry, u16 flags, u32 health, u32 power = 0, bool ready = true,
                    bool conjured = false) {
    ConsumableItem c;
    c.entry = entry;
    c.count = 3;
    c.use.flags = flags;
    c.use.health = health;
    c.use.power = power;
    c.ready = ready;
    c.conjured = conjured;
    return c;
}

u32 chosen(TouchConsumable kind, bool inCombat, const std::vector<ConsumableItem>& items, u32 maxHealth = 0,
           u32 maxMana = 0) {
    const i32 i = chooseConsumable(kind, inCombat, items, maxHealth, maxMana);
    return i >= 0 ? items[static_cast<u32>(i)].entry : 0u;
}

TravelOption mount(u32 entry, u32 speed, bool spell = false) {
    TravelOption o;
    o.action = spell ? TouchAction::spell(entry) : TouchAction::item(entry);
    o.speed = speed;
    o.mount = true;
    o.outdoorsOnly = true;
    o.notInCombat = true;
    o.notShapeshifted = true;
    return o;
}

TravelOption form(u32 spellId, u32 formId, bool outdoorsOnly, bool swimming = false) {
    TravelOption o;
    o.action = TouchAction::spell(spellId);
    o.form = formId;
    o.outdoorsOnly = outdoorsOnly;
    o.swimming = swimming;
    return o;
}

Entity unit(u64 guid, std::vector<std::pair<u16, u32>> fields) {
    Entity e;
    e.guid = guid;
    e.type = ObjectType::Player;
    e.values.assign(uf::PLAYER_END, 0);
    for (const auto& [index, value] : fields) {
        e.values[index] = value;
    }
    return e;
}

} // namespace

TEST_CASE("touch smart: what an item restores, from its on-use spell in Spell.dbc") {
    if (!tables()) {
        MESSAGE("VD_PACKS_DIR not set: skipped");
        return;
    }
    // Healing Potion: an instant heal of 70-90 (base points 69 plus a d21), potion category 4.
    ConsumableUse u = classify(439);
    CHECK(u.flags == kConsumeHealth);
    CHECK(u.health == 90);
    // Minor Healthstone: a heal of 100 in category 30, its own cooldown: before the potions.
    u = classify(6262);
    CHECK(u.flags == (kConsumeHealth | kConsumeStone));
    CHECK(u.health == 100);
    // Restore Mana (Minor Mana Potion) and Replenish Mana (Mana Agate, category 30: a gem).
    CHECK(classify(437).flags == kConsumeMana);
    CHECK(classify(437).power == 180);
    CHECK(classify(5405).flags == (kConsumeMana | kConsumeStone));
    // Rejuvenation Potion: both.
    CHECK(classify(2370).flags == (kConsumeHealth | kConsumeMana));
    // Rage Potion and Thistle Tea's Restore Energy: energize of the power in the effect's misc value.
    CHECK(classify(6612).flags == kConsumeRage);
    CHECK(classify(6612).power == 400);
    CHECK(classify(9512).flags == kConsumeEnergy);
    // Mighty Rage Potion: rage, its strength aura does not make it something else.
    CHECK(classify(17528).flags == kConsumeRage);
    // Food 433 (Tough Jerky): MOD_REGEN of 17 every 5 s for 18 s, not in combat: 61, the tooltip's $o1.
    u = classify(433);
    CHECK(u.flags == kConsumeFood);
    CHECK(u.health == 61);
    // Drink 430: MOD_POWER_REGEN of mana.
    CHECK(classify(430).flags == kConsumeDrink);
    // Food 5004 triggers Well Fed (19705): more than eating, never chosen by itself.
    CHECK(classify(5004).flags == (kConsumeFood | kConsumeExtra));
    // Food 2639 eats and drinks: both, nothing more.
    CHECK(classify(2639).flags == (kConsumeFood | kConsumeDrink));
    // Green Tea 833: a drink with an instant heal: more than drinking.
    CHECK(classify(833).flags == (kConsumeDrink | kConsumeExtra));
    // First Aid 746 (Linen Bandage): the channeled periodic heal, 11 a second for 6 s.
    u = classify(746);
    CHECK(u.flags == kConsumeBandage);
    CHECK(u.health == 66);
    CHECK(classify(18610).flags == kConsumeBandage); // Heavy Runecloth Bandage
    // Not consumables: a mount, a stance.
    CHECK_FALSE(classify(458).any());
    CHECK_FALSE(classify(2457).any());
    // An item's own category of 30 makes a heal a stone too.
    CHECK((classify(439, kHealthstoneCategory).flags & kConsumeStone) != 0);
}

TEST_CASE("touch smart: the consumable buttons choose what is used now") {
    constexpr u32 kPotion = 1, kBigPotion = 2, kStone = 3, kFood = 4, kConjuredFood = 5, kWellFed = 6,
                  kDrink = 7, kGem = 8, kManaPotion = 9, kRage = 10, kBandage = 11, kHeavyBandage = 12,
                  kPercentFood = 13;
    std::vector<ConsumableItem> bags = {
        item(kPotion, kConsumeHealth, 90),
        item(kBigPotion, kConsumeHealth, 455),
        item(kStone, kConsumeHealth | kConsumeStone, 100),
        item(kFood, kConsumeFood, 243),
        item(kConjuredFood, kConsumeFood, 61, 0, true, true),
        item(kWellFed, kConsumeFood | kConsumeExtra, 552),
        item(kDrink, kConsumeDrink, 0, 436),
        item(kGem, kConsumeMana | kConsumeStone, 0, 425, true, true),
        item(kManaPotion, kConsumeMana, 0, 585),
        item(kRage, kConsumeRage, 0, 400),
        item(kBandage, kConsumeBandage, 66),
        item(kHeavyBandage, kConsumeBandage, 400),
    };
    // In combat: the healthstone while it is ready, then the best potion (separate cooldowns in 1.12).
    CHECK(chosen(TouchConsumable::Health, true, bags) == kStone);
    bags[2].ready = false;
    CHECK(chosen(TouchConsumable::Health, true, bags) == kBigPotion);
    bags[1].ready = false;
    CHECK(chosen(TouchConsumable::Health, true, bags) ==
          kPotion); // a potion ready before a better one cooling down
    bags[0].ready = false;
    CHECK(chosen(TouchConsumable::Health, true, bags) == kStone); // none ready: the stone, its cooldown shown
    for (ConsumableItem& c : bags) {
        c.ready = true;
    }
    // Out of combat: eating, conjured first, never Well Fed food.
    CHECK(chosen(TouchConsumable::Health, false, bags) == kConjuredFood);
    CHECK(chosen(TouchConsumable::Food, true, bags) == kConjuredFood); // a Food button eats whenever
    bags[4].count = 0;
    CHECK(chosen(TouchConsumable::Health, false, bags) == kFood);
    bags[3].usable = false; // a level too low
    CHECK(chosen(TouchConsumable::Health, false, bags) == 0u);
    // Mana: the gem in combat, then the potion; drinking out of combat.
    CHECK(chosen(TouchConsumable::Mana, true, bags) == kGem);
    bags[7].ready = false;
    CHECK(chosen(TouchConsumable::Mana, true, bags) == kManaPotion);
    CHECK(chosen(TouchConsumable::Mana, false, bags) == kDrink);
    // Rage: its potion in and out of combat (the third slot has the bandages).
    CHECK(chosen(TouchConsumable::Rage, true, bags) == kRage);
    CHECK(chosen(TouchConsumable::Rage, false, bags) == kRage);
    CHECK(chosen(TouchConsumable::Energy, true, bags) == 0u);
    // Bandages: the best the First Aid skill allows.
    CHECK(chosen(TouchConsumable::Bandage, true, bags) == kHeavyBandage);
    bags[11].usable = false;
    CHECK(chosen(TouchConsumable::Bandage, true, bags) == kBandage);
    // A food restoring a share of the health: worth what that share is of this player's.
    std::vector<ConsumableItem> fruitcake = {item(kFood, kConsumeFood, 243),
                                             item(kPercentFood, kConsumeFood | kConsumePercent, 50)};
    CHECK(chosen(TouchConsumable::Health, false, fruitcake, 400) == kFood);
    CHECK(chosen(TouchConsumable::Health, false, fruitcake, 2000) == kPercentFood);
    // Nothing carried.
    CHECK(chosen(TouchConsumable::Drink, false, {}) == 0u);
    // The hold's list and "Automatic".
    CHECK(consumableFits(TouchConsumable::Health, bags[5].use)); // Well Fed food can be fixed by hand
    CHECK_FALSE(consumableFits(TouchConsumable::Health, bags[6].use));
    CHECK(consumableKindOf(bags[6].use, TouchConsumable::Health) == TouchConsumable::Mana);
    CHECK(consumableKindOf(bags[9].use, TouchConsumable::Health) == TouchConsumable::Rage);
    CHECK(consumableKindOf({}, TouchConsumable::Bandage) == TouchConsumable::Bandage);
}

TEST_CASE("touch smart: mounts and the travel forms from Spell.dbc") {
    if (!tables()) {
        MESSAGE("VD_PACKS_DIR not set: skipped");
        return;
    }
    u32 speed = 0;
    CHECK(isMountSpell(details(458), &speed)); // Brown Horse: SPELL_AURA_MOUNTED, +60%
    CHECK(speed == 60);
    CHECK(isMountSpell(details(23219), &speed)); // Swift Mistsaber: +100%
    CHECK(speed == 100);
    CHECK(isMountSpell(details(5784), &speed)); // Summon Felsteed, the warlock's
    CHECK(speed == 60);
    CHECK_FALSE(isMountSpell(details(783)));
    const TravelOption horse = travelOptionOf(details(458), TouchAction::item(2411));
    CHECK(horse.mount);
    CHECK(horse.outdoorsOnly);
    CHECK(horse.notInCombat);
    CHECK(horse.notShapeshifted);
    const TravelOption travel = travelOptionOf(details(783), TouchAction::spell(783));
    CHECK(travel.form == 3);
    CHECK(travel.outdoorsOnly);
    CHECK_FALSE(travel.notInCombat);
    const TravelOption aquatic = travelOptionOf(details(1066), TouchAction::spell(1066));
    CHECK(aquatic.swimming);
    CHECK_FALSE(aquatic.outdoorsOnly);
    // Cat Form anywhere; Ghost Wolf outdoors only (its data says so); Aspect of the Cheetah anywhere.
    CHECK(travelOptionOf(details(768), TouchAction::spell(768)).form == 1);
    CHECK(travelOptionOf(details(2645), TouchAction::spell(2645)).outdoorsOnly);
    CHECK_FALSE(travelOptionOf(details(5118), TouchAction::spell(5118)).outdoorsOnly);
}

TEST_CASE("touch smart: the mount button") {
    // The fastest usable; the favourite when there; a class mount before an item of its speed.
    std::vector<TravelOption> options = {mount(2411, 60), mount(13086, 100), mount(5784, 60, true)};
    CHECK(chooseMount(options, {}) == 1);
    options[1].usable = false; // the riding skill of an epic mount missing
    CHECK(chooseMount(options, {}) == 2);
    CHECK(chooseMount(options, TouchAction::item(2411)) == 0);
    CHECK(chooseMount(options, TouchAction::item(13086)) == 2); // the favourite cannot be ridden: the fastest

    // A druid of level 40 with a horse: Travel Form, Cat Form, Aquatic Form.
    std::vector<TravelOption> druid = {mount(2411, 60), form(783, 3, true), form(768, 1, false),
                                       form(1066, 4, false, true)};
    TravelState state;
    TravelChoice c = chooseTravel(state, druid, {});
    CHECK(c.action == TouchAction::item(2411));
    CHECK(c.usable);
    CHECK_FALSE(c.active);
    state.inCombat = true; // no mount in combat: the form that can
    CHECK(chooseTravel(state, druid, {}).action == TouchAction::spell(783));
    state.indoors = true; // indoors: the cat
    CHECK(chooseTravel(state, druid, {}).action == TouchAction::spell(768));
    state = {};
    state.swimming = true;
    CHECK(chooseTravel(state, druid, {}).action == TouchAction::spell(1066));
    state = {};
    state.form = 3; // in Travel Form: a mount cannot be cast; the button leaves the form
    c = chooseTravel(state, druid, {});
    CHECK(c.action == TouchAction::spell(783));
    CHECK(c.active);
    state = {};
    state.mounted = true;
    state.auraSpell = 458;
    c = chooseTravel(state, druid, {});
    CHECK(c.action == TouchAction::spell(458, kTouchFixedRank)); // dismount: its aura cancelled
    CHECK(c.active);

    // A shaman indoors: Ghost Wolf is outdoors only (its spell says so): shown faint.
    std::vector<TravelOption> shaman = {form(2645, 16, true)};
    state = {};
    state.indoors = true;
    c = chooseTravel(state, shaman, {});
    CHECK(c.action == TouchAction::spell(2645));
    CHECK_FALSE(c.usable);
    // A mount indoors without a form: faint too; nothing at all: hidden.
    std::vector<TravelOption> rogue = {mount(2411, 60)};
    c = chooseTravel(state, rogue, {});
    CHECK(c.action == TouchAction::item(2411));
    CHECK_FALSE(c.usable);
    CHECK(chooseTravel(state, {}, {}).action.empty());
    // A hunter's aspect in use: a tap cancels it (out of combat the mount comes first).
    TravelOption cheetah = form(5118, 0, false);
    cheetah.active = true;
    std::vector<TravelOption> hunter = {mount(2411, 60), cheetah};
    state = {};
    CHECK(chooseTravel(state, hunter, {}).action == TouchAction::item(2411));
    state.inCombat = true;
    c = chooseTravel(state, hunter, {});
    CHECK(c.action == TouchAction::spell(5118));
    CHECK(c.active);
    // A warrior in a stance, a rogue in Stealth, a priest in Shadowform may mount (Unit::IsInDisallowedMountForm);
    // Moonkin Form may not.
    std::vector<TravelOption> warrior = {mount(2411, 60)};
    for (const u32 allowed : {17u, 18u, 19u, 28u, 30u}) {
        state = {};
        state.form = allowed;
        CHECK(chooseTravel(state, warrior, {}).usable);
    }
    state.form = 31;
    CHECK_FALSE(chooseTravel(state, warrior, {}).usable);
}

TEST_CASE("touch smart: the reactive spells") {
    // The longest cooldown first.
    std::array<ReactiveEntry, 3> list{};
    u32 count = 0;
    insertReactive(list, count, {5308, 0});    // Execute
    insertReactive(list, count, {7384, 5000}); // Overpower
    insertReactive(list, count, {6572, 5000}); // Revenge
    insertReactive(list, count, {1, 9000});    // full: left out
    REQUIRE(count == 3);
    CHECK(list[0].spellId == 7384);
    CHECK(list[1].spellId == 6572);
    CHECK(list[2].spellId == 5308);
    // Ready: usable (or only short of power) and off its own cooldown (the global one does not count).
    CHECK(reactiveReady({true, false}, 0, 0));
    CHECK(reactiveReady({false, true}, 0, 0));
    CHECK_FALSE(reactiveReady({false, false}, 0, 0));
    CHECK(reactiveReady({true, false}, 1400, 1500));
    CHECK_FALSE(reactiveReady({true, false}, 4000, 1500));

    if (!tables()) {
        MESSAGE("VD_PACKS_DIR not set: skipped");
        return;
    }
    // From Spell.dbc: aura states and Overpower's combo point; not the finishers of energy, not plain
    // attacks.
    for (const u32 id : {7384u, 6572u, 5308u, 14251u, 1495u, 19306u, 24275u}) {
        CAPTURE(id);
        CHECK(isReactiveSpell(details(id)));
    }
    // ... nor Judgement: its state is the paladin's own seal.
    for (const u32 id : {78u, 2098u, 22568u, 1752u, 133u, 20271u}) {
        CAPTURE(id);
        CHECK_FALSE(isReactiveSpell(details(id)));
    }
    // Execute is usable only on a target under 20% (AURA_STATE_HEALTHLESS_20_PERCENT: bit 1 of its aura
    // state).
    const Tables* t = tables();
    const std::optional<SpellInfo> execute = t->data.spell(5308);
    REQUIRE(execute);
    const SpellDetails executeDetails = details(5308);
    const Entity warrior = unit(
        0x11,
        {{uf::UNIT_FIELD_HEALTH, 900}, {uf::UNIT_FIELD_POWER2, 300}, {uf::UNIT_FIELD_BYTES_1, 17u << 16}});
    const Entity healthy = unit(0x12, {{uf::UNIT_FIELD_HEALTH, 1000}});
    const Entity dying = unit(0x13, {{uf::UNIT_FIELD_HEALTH, 150}, {uf::UNIT_FIELD_AURASTATE, 1u << 1}});
    CHECK_FALSE(spellUsable(*execute, &executeDetails, warrior, &healthy, 0, &t->abilities).usable);
    CHECK(spellUsable(*execute, &executeDetails, warrior, &dying, 0, &t->abilities).usable);
    // Overpower wants the combo point the dodge gave, in Battle Stance.
    const std::optional<SpellInfo> overpower = t->data.spell(7384);
    REQUIRE(overpower);
    const SpellDetails overpowerDetails = details(7384);
    CHECK_FALSE(spellUsable(*overpower, &overpowerDetails, warrior, &healthy, 0, &t->abilities).usable);
    CHECK(spellUsable(*overpower, &overpowerDetails, warrior, &healthy, 1, &t->abilities).usable);
}
