// Phase 20 (assists, docs/MOBILE_CONTROLS.md §4 and §8): the rules and state machines of the touch cluster's assists
// (game/TouchAssistRules): when a spell starts the auto attack (1.12's flags and the "attack macro"), the repeat of a
// held button with the server's refusals, the spell queue, the stance and form assist (the form first, the spell once
// the server shows it, or forgotten), the template by site, the ground aim's mapping, and the healer frames of the
// templates (their file and the class presets). The real spells come from Spell.dbc (VD_PACKS_DIR; skipped without).
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/ClassPresets.h"
#include "game/GameData.h"
#include "game/SpellText.h"
#include "game/TouchAssistRules.h"
#include "game/TouchContext.h"
#include "game/TouchLayout.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace vd;
using namespace vd::game;

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
        state =
            dir && t.db.open(std::filesystem::path(dir) / "db.vdpk").ok() && t.abilities.init(t.db) && t.data.init(t.db)
                ? 1
                : 2;
    }
    return state == 1 ? &t : nullptr;
}

#define REQUIRE_TABLES(var)                                                                                            \
    const Tables* var = tables();                                                                                      \
    if (!var) {                                                                                                        \
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");                                                    \
        return;                                                                                                        \
    }

SpellDetails details(const Tables& t, u32 spellId) {
    SpellDetails d;
    REQUIRE(t.abilities.spellText().details(spellId, d));
    return d;
}

// SpellCastResult codes (SpellDefines.h).
constexpr u32 kNotReady = 60;
constexpr u32 kNoPower = 77;
constexpr u32 kOutOfRange = 89;

} // namespace

TEST_CASE("Touch assists: which spells start the auto attack") {
    SpellDetails plain;
    CHECK(attackStartOf(plain, true) == AttackStart::None);
    // 1.12's own: SPELL_ATTR_EX_INITIATES_COMBAT as it casts, _EX2_INITIATE_COMBAT_POST_CAST once it went off (that one
    // first: the stealth openers carry both).
    SpellDetails initiates;
    initiates.attributesEx = 0x200;
    CHECK(attackStartOf(initiates, false) == AttackStart::AtCast);
    SpellDetails postCast;
    postCast.attributesEx = 0x200;
    postCast.attributesEx2 = 0x100000;
    CHECK(attackStartOf(postCast, false) == AttackStart::AfterCast);
    // The assist's "attack macro": the next-swing spells and the seals.
    SpellDetails nextSwing;
    nextSwing.attributes = 0x4;
    CHECK(attackStartOf(nextSwing, false) == AttackStart::None);
    CHECK(attackStartOf(nextSwing, true) == AttackStart::AtCast);
    SpellDetails seal;
    seal.spellFamily = 10;
    seal.familyFlags = 0x08000000;
    CHECK(attackStartOf(seal, false) == AttackStart::None);
    CHECK(attackStartOf(seal, true) == AttackStart::AtCast);
    seal.familyFlags = 0x00800000; // Judgement's flag: not a seal
    CHECK(attackStartOf(seal, true) == AttackStart::None);
}

TEST_CASE("Touch assists: the auto attack of the real spells") {
    REQUIRE_TABLES(t);
    struct Case {
        u32 spell;
        AttackStart without, with;
    };
    const Case cases[] = {
        {78, AttackStart::None, AttackStart::AtCast},            // Heroic Strike: no INITIATES_COMBAT
        {845, AttackStart::AtCast, AttackStart::AtCast},         // Cleave has it
        {2973, AttackStart::None, AttackStart::AtCast},          // Raptor Strike: none
        {6807, AttackStart::AtCast, AttackStart::AtCast},        // Maul has it
        {1752, AttackStart::AtCast, AttackStart::AtCast},        // Sinister Strike
        {12294, AttackStart::AtCast, AttackStart::AtCast},       // Mortal Strike
        {21084, AttackStart::None, AttackStart::AtCast},         // Seal of Righteousness
        {20375, AttackStart::None, AttackStart::AtCast},         // Seal of Command
        {21082, AttackStart::None, AttackStart::AtCast},         // Seal of the Crusader
        {20271, AttackStart::AfterCast, AttackStart::AfterCast}, // Judgement: after SMSG_SPELL_GO
        {8676, AttackStart::AfterCast, AttackStart::AfterCast},  // Ambush (both flags)
        {1833, AttackStart::AfterCast, AttackStart::AfterCast},  // Cheap Shot
        {116, AttackStart::None, AttackStart::None},             // Frostbolt
        {2050, AttackStart::None, AttackStart::None},            // Lesser Heal
        {19740, AttackStart::None, AttackStart::None},           // Blessing of Might
    };
    for (const Case& c : cases) {
        CAPTURE(c.spell);
        const SpellDetails d = details(*t, c.spell);
        CHECK(attackStartOf(d, false) == c.without);
        CHECK(attackStartOf(d, true) == c.with);
    }
}

TEST_CASE("Touch assists: a held button's repeat waits for the server and backs off after a refusal") {
    TouchRepeatGate gate;
    CHECK(gate.ready(1000));
    gate.sent(78, 1000);
    CHECK(gate.waiting() == 78);
    CHECK_FALSE(gate.ready(1100)); // on its way
    CHECK_FALSE(gate.ready(1300)); // still: no answer yet
    gate.answered(78);
    CHECK(gate.ready(1300)); // answered, 250 ms since it was sent
    gate.sent(78, 1300);
    gate.answered(78);
    CHECK_FALSE(gate.ready(1400)); // answered at once, but at most one every 250 ms
    CHECK(gate.ready(1550));
    // A lost answer: not forever.
    gate.sent(78, 2000);
    CHECK_FALSE(gate.ready(2500));
    CHECK(gate.ready(2000 + kTouchInFlightMs));
    // Refused: not before the state could change (no rage, out of range); "not ready" only a moment.
    gate.sent(78, 3000);
    gate.refused(78, kNoPower, 3050);
    CHECK(gate.waiting() == 0);
    CHECK_FALSE(gate.ready(3300));
    CHECK_FALSE(gate.ready(3050 + kTouchRefusedMs - 1));
    CHECK(gate.ready(3050 + kTouchRefusedMs));
    gate.sent(78, 5000);
    gate.refused(78, kNotReady, 5020);
    CHECK_FALSE(gate.ready(5200));
    CHECK(gate.ready(5020 + kTouchNotReadyMs));
    // Another spell's refusal (another button, the classic bars) changes nothing.
    gate.sent(78, 6000);
    gate.answered(78);
    gate.refused(116, kOutOfRange, 6300);
    CHECK(gate.ready(6300));
    // Wrapping clock.
    TouchRepeatGate wrap;
    wrap.sent(78, 0xFFFFFF00u);
    wrap.answered(78);
    CHECK_FALSE(wrap.ready(0xFFFFFF80u));
    CHECK(wrap.ready(0x00000020u));
}

TEST_CASE("Touch assists: the spell queue sends a press of the global cooldown's last moment as it ends") {
    TouchSpellQueue queue;
    CHECK_FALSE(queue.offer(116, 0, 1000));   // nothing in the way: sent now
    CHECK_FALSE(queue.offer(116, 900, 1000)); // too early: sent (the server says "not ready", as 1.12)
    CHECK(queue.offer(116, 150, 1000));       // the last 200 ms: it waits
    CHECK(queue.waiting() == 116);
    CHECK(queue.due(100, 1050) == 0);
    CHECK(queue.offer(133, 120, 1080)); // a later press replaces it
    CHECK(queue.due(0, 1200) == 133);   // its way is free: sent
    CHECK(queue.waiting() == 0);
    CHECK(queue.due(0, 1300) == 0);
    // Forgotten when its moment passed long ago (a cast pushed back).
    CHECK(queue.offer(116, 200, 2000));
    CHECK(queue.due(150, 2000 + 200 + kTouchQueueGraceMs) == 0);
    CHECK(queue.waiting() == 0);
}

TEST_CASE("Touch assists: a spell waits for its form, or is forgotten") {
    TouchFormSwitch sw;
    sw.start(7384, 0xF130000000001234ull, kFormBattleStance, 1000); // Overpower from Berserker Stance
    CHECK(sw.waiting() == 7384);
    CHECK(sw.poll(kFormBerserkerStance, 1100).spellId == 0);
    const TouchFormSwitch::Due due = sw.poll(kFormBattleStance, 1250); // the server shows the stance
    CHECK(due.spellId == 7384);
    CHECK(due.target == 0xF130000000001234ull);
    CHECK(sw.waiting() == 0);
    CHECK(sw.poll(kFormBattleStance, 1300).spellId == 0); // once
    // The stance never comes (refused, on its cooldown): forgotten after the wait.
    sw.start(7384, 0, kFormBattleStance, 5000);
    CHECK(sw.poll(kFormBerserkerStance, 5000 + kTouchFormWaitMs - 1).spellId == 0);
    CHECK(sw.waiting() == 7384);
    CHECK(sw.poll(kFormBerserkerStance, 5000 + kTouchFormWaitMs).spellId == 0);
    CHECK(sw.waiting() == 0);
    CHECK(sw.poll(kFormBattleStance, 5000 + kTouchFormWaitMs + 10).spellId == 0); // too late
    // Leaving an animal form waits for the caster's form (0).
    sw.start(5185, 0, 0, 8000);
    CHECK(sw.poll(kFormCat, 8100).spellId == 0);
    CHECK(sw.poll(0, 8200).spellId == 5185);
}

TEST_CASE("Touch assists: the stance or form a spell needs, from the real data") {
    REQUIRE_TABLES(t);
    const SpellText& text = t->abilities.spellText();
    // A level 30 warrior: Battle, Defensive and Berserker Stance.
    const std::vector<u32> warrior = {71, 2457, 2458};
    std::array<ShapeshiftForm, kMaxShapeshiftForms> forms{};
    const u32 count = shapeshiftForms(warrior, text, forms);
    REQUIRE(count == 3);
    const std::span<const ShapeshiftForm> stances{forms.data(), count};
    const auto change = [&](u32 spell, u32 form, bool stancesOn = true, bool druidOn = false) {
        return touchFormChange(details(*t, spell), form, stances, t->abilities, stancesOn, druidOn);
    };
    TouchFormChange c = change(7384, kFormBerserkerStance); // Overpower: Battle Stance first
    CHECK(c.spellId == 2457);
    CHECK(c.form == kFormBattleStance);
    CHECK(c.leaveSpell == 0);
    c = change(6572, kFormBattleStance); // Revenge: Defensive Stance
    CHECK(c.spellId == 71);
    CHECK(c.form == kFormDefensiveStance);
    c = change(6552, kFormDefensiveStance); // Pummel: Berserker Stance
    CHECK(c.spellId == 2458);
    c = change(5308, kFormDefensiveStance); // Execute (Battle or Berserker): the first of the bar's order
    CHECK(c.form == kFormBattleStance);
    CHECK_FALSE(change(7384, kFormBattleStance).needed());           // castable already
    CHECK_FALSE(change(12294, kFormBerserkerStance).needed());       // Mortal Strike: any stance
    CHECK_FALSE(change(7384, kFormBerserkerStance, false).needed()); // the assist off
    // A druid with Bear, Cat and Travel Form.
    const std::vector<u32> druid = {768, 783, 5487};
    std::array<ShapeshiftForm, kMaxShapeshiftForms> druidForms{};
    const u32 druidCount = shapeshiftForms(druid, text, druidForms);
    REQUIRE(druidCount == 3);
    const std::span<const ShapeshiftForm> animal{druidForms.data(), druidCount};
    const auto druidChange = [&](u32 spell, u32 form, bool on) {
        return touchFormChange(details(*t, spell), form, animal, t->abilities, true, on);
    };
    c = druidChange(5185, kFormCat, true); // Healing Touch in Cat Form: out of it
    CHECK(c.leaveSpell == 768);
    CHECK(c.spellId == 0);
    CHECK(c.form == 0);
    CHECK(druidChange(5185, kFormBear, true).leaveSpell == 5487);
    CHECK_FALSE(druidChange(5185, kFormCat, false).needed()); // off by default
    CHECK_FALSE(druidChange(1082, kFormCat, true).needed());  // Claw: a cat's
    CHECK_FALSE(druidChange(5185, 0, true).needed());         // a caster already
    CHECK_FALSE(druidChange(1082, 0, true).needed());         // Claw as a caster: no Cat Form for it
}

TEST_CASE("Touch assists: the template of the place entered") {
    CHECK(touchSiteOf(0) == TouchSite::World);
    CHECK(touchSiteOf(1) == TouchSite::Dungeon);
    CHECK(touchSiteOf(2) == TouchSite::Raid);
    CHECK(touchSiteOf(3) == TouchSite::Battleground);
    CHECK(touchSiteOf(9) == TouchSite::None);
    TouchLayout layout;
    layout.count = 3;
    layout.templates[0] = makeTouchTemplate("Solo");
    layout.templates[1] = makeTouchTemplate("Dungeon");
    layout.templates[1].site = static_cast<u8>(TouchSite::Dungeon);
    layout.templates[2] = makeTouchTemplate("PvP");
    layout.templates[2].site = static_cast<u8>(TouchSite::Battleground);
    CHECK(touchTemplateForSite(layout, TouchSite::Dungeon) == 1);
    CHECK(touchTemplateForSite(layout, TouchSite::Raid) == -1);
    TouchSiteSwitch sw;
    CHECK(sw.update(TouchSite::World, layout) == -1);  // no template for the world: the one in use stays
    CHECK(sw.update(TouchSite::Dungeon, layout) == 1); // into a dungeon
    layout.active = 1;
    CHECK(sw.update(TouchSite::Dungeon, layout) == -1); // the same place: nothing
    layout.active = 0;                                  // the player's own choice inside it
    CHECK(sw.update(TouchSite::Dungeon, layout) == -1); // ...stays
    CHECK(sw.update(TouchSite::None, layout) == -1);    // a map not known: nothing, and not a change of place
    CHECK(sw.update(TouchSite::Dungeon, layout) == -1);
    CHECK(sw.update(TouchSite::Battleground, layout) == 2);
    layout.active = 2;
    CHECK(sw.update(TouchSite::World, layout) == -1);
    sw.reset();
    layout.active = 0;
    CHECK(sw.update(TouchSite::Battleground, layout) == 2); // reset (the option turned on): at once
}

TEST_CASE("Touch assists: the ground aim around the player") {
    const glm::vec2 forward(1.0f, 0.0f);                              // the camera looks north (+x)
    const glm::vec2 right(0.0f, -1.0f);                               // east
    glm::vec2 o = touchAimOffset(0.0f, -1.0f, forward, right, 30.0f); // up on the screen: away from the camera
    CHECK(o.x == doctest::Approx(30.0f));
    CHECK(o.y == doctest::Approx(0.0f));
    o = touchAimOffset(0.5f, 0.0f, forward, right, 30.0f); // half way right
    CHECK(o.x == doctest::Approx(0.0f));
    CHECK(o.y == doctest::Approx(-15.0f));
    o = touchAimOffset(0.0f, 0.4f, forward, right, 30.0f); // down: behind the player
    CHECK(o.x == doctest::Approx(-12.0f));
    o = touchAimOffset(3.0f, 4.0f, forward, right, 10.0f); // beyond the pad: the range's edge
    CHECK(std::sqrt(o.x * o.x + o.y * o.y) == doctest::Approx(10.0f));
    o = touchAimOffset(0.0f, 0.0f, forward, right, 30.0f); // at the button's rim: the player's feet
    CHECK(o.x == doctest::Approx(0.0f));
    CHECK(o.y == doctest::Approx(0.0f));
}

TEST_CASE("Touch assists: a template's healer frames, in its file and the healing presets") {
    TouchLayout layout;
    layout.count = 2;
    layout.templates[0] = makeTouchTemplate("Heal");
    layout.templates[0].healerFrames = true;
    layout.templates[1] = makeTouchTemplate("Damage");
    TouchLayout back;
    REQUIRE(parseTouchLayout(formatTouchLayout(layout), back));
    CHECK(back.templates[0].healerFrames);
    CHECK_FALSE(back.templates[1].healerFrames);
    CHECK(presetRoleHeals(PresetRole::PriestHeal));
    CHECK(presetRoleHeals(PresetRole::DruidHeal));
    CHECK(presetRoleHeals(PresetRole::PaladinHeal));
    CHECK(presetRoleHeals(PresetRole::ShamanHeal));
    CHECK_FALSE(presetRoleHeals(PresetRole::PriestDamage));
    CHECK_FALSE(presetRoleHeals(PresetRole::WarriorTank));
    REQUIRE_TABLES(t);
    // A priest's presets: "Heal" brings the party frames beside the cluster, "Damage" does not.
    std::vector<u32> known;
    classDemoKnown(5, 1, 40, 0, t->abilities, known);
    std::sort(known.begin(), known.end());
    PresetInput in;
    in.cls = 5;
    in.race = 1;
    in.level = 40;
    in.known = known;
    in.text = &t->abilities.spellText();
    in.abilities = &t->abilities;
    in.data = &t->data;
    TouchLayout priest;
    REQUIRE(buildClassLayout(in, priest));
    u32 heal = 0, other = 0;
    for (u32 i = 0; i < priest.count; ++i) {
        const TouchTemplate& tpl = priest.templates[i];
        if (tpl.role == static_cast<u8>(PresetRole::PriestHeal)) {
            CHECK(tpl.healerFrames);
            ++heal;
        } else {
            CHECK_FALSE(tpl.healerFrames);
            ++other;
        }
    }
    CHECK(heal == 1);
    CHECK(other >= 1);
}
