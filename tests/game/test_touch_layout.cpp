// Phase 20 (docs/MOBILE_CONTROLS.md): the touch layout. A layer shows its own actions and inherits the base's
// elsewhere (a hidden slot shows nothing); the file keeps everything and refuses another format (1.12's own
// layout-cache.txt shares the server's account data type 6); the player's state picks the layer (Stealth's form, not
// the stealth flag; Prowl over Cat; the hunter's melee range with its hysteresis); a spell casts its highest known rank
// (the real Spell.dbc).
#include "game/AbilityData.h"
#include "game/SpellLines.h"
#include "game/TouchContext.h"
#include "game/TouchLayout.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace vd;
using namespace vd::game;

namespace {

const AbilityData* abilityData() {
    static pack::PackReader db;
    static AbilityData data;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

constexpr u32 slot(TouchPlace p) {
    return touchIndex(p);
}

} // namespace

TEST_CASE("touch layout: a layer inherits the base where it is empty, a hidden slot shows nothing") {
    TouchTemplate t = makeTouchTemplate("Solo");
    t.layers[0][slot(TouchPlace::Main)] = TouchAction::spell(1752);  // Sinister Strike
    t.layers[0][slot(TouchPlace::RingA)] = TouchAction::spell(2098); // Eviscerate
    t.layers[0][slot(TouchPlace::RingB)] = TouchAction::spell(1784); // Stealth
    const u32 stealth = static_cast<u32>(TouchContext::Stealth);
    t.layers[stealth][slot(TouchPlace::Main)] = TouchAction::spell(8676); // Ambush
    t.layers[stealth][slot(TouchPlace::RingA)].flags = kTouchHidden;

    CHECK(resolveTouchAction(t, TouchContext::Base, slot(TouchPlace::Main)).id == 1752);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, slot(TouchPlace::Main)).id == 8676);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, slot(TouchPlace::RingA)).empty());
    CHECK(resolveTouchAction(t, TouchContext::Stealth, slot(TouchPlace::RingB)).id == 1784);
    // A layer turned off: the base everywhere.
    t.disabledLayers = static_cast<u16>(1u << stealth);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, slot(TouchPlace::Main)).id == 1752);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, slot(TouchPlace::RingA)).id == 2098);
    CHECK(resolveTouchAction(t, TouchContext::Base, kTouchSlots).empty());
}

TEST_CASE("touch layout: the file keeps templates, places, layers and groups") {
    TouchLayout layout;
    layout.count = 2;
    layout.active = 1;
    layout.templates[0] = makeTouchTemplate("Solo");
    layout.templates[1] = makeTouchTemplate("Dungeon \"tank\""); // a quote would end the name in the file
    TouchTemplate& t = layout.templates[1];
    t.role = 3;
    t.site = static_cast<u8>(TouchSite::Dungeon);
    t.disabledLayers = 0x0100;
    t.geometry[0].scale = 0.85f;
    t.geometry[1].places[slot(TouchPlace::RingE)] = {-20.5f, -6.25f, 9.75f};
    t.layers[0][slot(TouchPlace::Main)] = TouchAction::spell(78, kTouchManual);
    t.layers[0][slot(TouchPlace::Consumable1)] = TouchAction::consumable(TouchConsumable::Health);
    t.layers[0][slot(TouchPlace::Consumable3)] = TouchAction::item(6450, kTouchManual);
    t.layers[0][slot(TouchPlace::ColumnF)] = {TouchActionKind::Group, 0, 0, 2};
    t.layers[0][slot(TouchPlace::ColumnG)] = TouchAction::spell(2054, kTouchFixedRank);
    t.layers[0][slot(TouchPlace::ColumnH)] = TouchAction::special(TouchSpecial::Attack);
    t.layers[static_cast<u32>(TouchContext::DefensiveStance)][slot(TouchPlace::RingC)] = TouchAction::spell(6572);
    t.layers[static_cast<u32>(TouchContext::DefensiveStance)][slot(TouchPlace::RingD)].flags = kTouchHidden;
    t.groups[2].count = 3;
    t.groups[2].current = 1;
    t.groups[2].autoKind = 7;
    t.groups[2].actions[0] = TouchAction::spell(6673);
    t.groups[2].actions[1] = TouchAction::spell(1160);
    t.groups[2].actions[2] = {TouchActionKind::Macro, 0, 0, 0x1000003};
    t.classBarGroups[0] = 5;
    t.classBarGroups[1] = 6;
    layout.mount = TouchAction::item(5656); // a mount the player chose

    const std::string text = formatTouchLayout(layout);
    CHECK(text.rfind("VDTOUCH 3\n", 0) == 0);
    TouchLayout back;
    REQUIRE(parseTouchLayout(text, back));
    REQUIRE(back.count == 2);
    CHECK(back.active == 1);
    const TouchTemplate& b = back.templates[1];
    CHECK(b.nameView() == "Dungeon 'tank'");
    CHECK(b.role == 3);
    CHECK(b.site == static_cast<u8>(TouchSite::Dungeon));
    CHECK(b.disabledLayers == 0x0100);
    CHECK(b.geometry[0].scale == doctest::Approx(0.85f));
    CHECK(b.geometry[1].places[slot(TouchPlace::RingE)].x == doctest::Approx(-20.5f));
    CHECK(b.geometry[1].places[slot(TouchPlace::RingE)].size == doctest::Approx(9.75f));
    CHECK(b.geometry[0].places[slot(TouchPlace::Main)].size == doctest::Approx(12.4f));
    for (u32 c = 0; c < kTouchContexts; ++c) {
        for (u32 s = 0; s < kTouchSlots; ++s) {
            CHECK(b.layers[c][s] == t.layers[c][s]);
        }
    }
    CHECK(b.groups[2].count == 3);
    CHECK(b.groups[2].current == 1);
    CHECK(b.groups[2].autoKind == 7);
    CHECK(b.groups[2].actions[2] == t.groups[2].actions[2]);
    CHECK(b.classBarGroups[0] == 5);
    CHECK(b.classBarGroups[1] == 6);
    CHECK(b.classBarGroups[2] == 0);
    CHECK(back.mount == layout.mount);
}

TEST_CASE("touch layout: another format is refused") {
    TouchLayout out;
    CHECK_FALSE(parseTouchLayout("", out));
    // What 1.12 itself keeps as its layout-cache.txt (the frames' places).
    CHECK_FALSE(parseTouchLayout("VERSION 3\nFRAME PlayerFrame\n", out));
    CHECK(out.count == 0);
    // Ours with lines it does not know (a later version's): kept what it knows.
    REQUIRE(parseTouchLayout("VDTOUCH 1\nactive 5\nwhatever\ntemplate \"A\"\n  layer base\n    main spell 133\n"
                             "    nowhere spell 1\n  layer nothing\n    main spell 2\nend\n",
                             out));
    CHECK(out.count == 1);
    CHECK(out.active == 0);
    CHECK(out.templates[0].layers[0][0].id == 133);
}

TEST_CASE("touch layout: a file of the first version keeps its actions, its places go back to the defaults") {
    // Version 1 kept the places in millimetres; version 2 keeps them in hundredths of the screen's height.
    TouchLayout out;
    REQUIRE(parseTouchLayout("VDTOUCH 1\nactive 0\ntemplate \"A\"\n  geometry square 1.000 27.00 25.00\n"
                             "    main 0.00 0.00 16.00\n  layer base\n    main spell 133\nend\n",
                             out));
    REQUIRE(out.count == 1);
    CHECK(out.templates[0].layers[0][0].id == 133);
    const TouchGeometry defaults = defaultTouchGeometry(TouchProfile::Square);
    CHECK(out.templates[0].geometry[0].places[0].size == doctest::Approx(defaults.places[0].size));
    CHECK(out.templates[0].geometry[0].mainRight == doctest::Approx(defaults.mainRight));
}

TEST_CASE("touch layout: a file of the second version grows a Target button of the old default size") {
    TouchLayout out;
    REQUIRE(parseTouchLayout("VDTOUCH 2\nactive 0\ntemplate \"A\"\n  geometry square 1.000 24.00 23.00\n"
                             "    target -25.00 24.00 6.00\n  geometry wide 1.000 24.00 23.00\n"
                             "    target -20.00 20.00 9.00\nend\n",
                             out));
    REQUIRE(out.count == 1);
    const u32 target = touchIndex(TouchPlace::Target);
    CHECK(out.templates[0].geometry[0].places[target].size ==
          doctest::Approx(defaultTouchGeometry(TouchProfile::Square).places[target].size));
    CHECK(out.templates[0].geometry[0].places[target].size == doctest::Approx(7.0f));
    CHECK(out.templates[0].geometry[1].places[target].size == doctest::Approx(9.0f)); // the player's own size stays
}

TEST_CASE("touch context: the form picks the layer") {
    CHECK(touchContextOf(0, false, false) == TouchContext::Base);
    CHECK(touchContextOf(kFormBattleStance, false, false) == TouchContext::BattleStance);
    CHECK(touchContextOf(kFormDefensiveStance, false, false) == TouchContext::DefensiveStance);
    CHECK(touchContextOf(kFormBerserkerStance, false, false) == TouchContext::BerserkerStance);
    CHECK(touchContextOf(kFormStealth, false, false) == TouchContext::Stealth);
    CHECK(touchContextOf(kFormCat, false, false) == TouchContext::Cat);
    CHECK(touchContextOf(kFormCat, true, false) == TouchContext::Prowl);
    CHECK(touchContextOf(kFormBear, false, false) == TouchContext::Bear);
    CHECK(touchContextOf(kFormDireBear, false, false) == TouchContext::Bear);
    CHECK(touchContextOf(kFormMoonkin, false, false) == TouchContext::Moonkin);
    CHECK(touchContextOf(kFormTravel, false, false) == TouchContext::Travel);
    CHECK(touchContextOf(kFormAquatic, false, false) == TouchContext::Travel);
    CHECK(touchContextOf(kFormGhostWolf, false, false) == TouchContext::Travel);
    CHECK(touchContextOf(kFormShadowform, false, false) == TouchContext::Shadowform);
    CHECK(touchContextOf(0, false, true) == TouchContext::Melee);
    // The 1.12 bonus bars (SpellShapeshiftForm.dbc).
    CHECK(touchContextBonusBar(TouchContext::BattleStance) == 1);
    CHECK(touchContextBonusBar(TouchContext::Stealth) == 1);
    CHECK(touchContextBonusBar(TouchContext::Cat) == 1);
    CHECK(touchContextBonusBar(TouchContext::DefensiveStance) == 2);
    CHECK(touchContextBonusBar(TouchContext::BerserkerStance) == 3);
    CHECK(touchContextBonusBar(TouchContext::Bear) == 3);
    CHECK(touchContextBonusBar(TouchContext::Prowl) == 0);
    CHECK(touchContextBonusBar(TouchContext::Base) == 0);
}

TEST_CASE("touch context: the hunter's melee layer keeps its state in Auto Shot's dead zone") {
    CHECK_FALSE(hunterMeleeLayer(false, true, 6.0f));
    CHECK(hunterMeleeLayer(false, true, 1.0f));
    CHECK(hunterMeleeLayer(true, true, 6.0f));
    CHECK_FALSE(hunterMeleeLayer(true, true, 9.0f));
    CHECK_FALSE(hunterMeleeLayer(true, false, 1.0f));
}

TEST_CASE("touch layout: the default places do not overlap") {
    for (u32 p = 0; p < kTouchProfiles; ++p) {
        const TouchGeometry g = defaultTouchGeometry(static_cast<TouchProfile>(p));
        for (u32 a = 0; a < kTouchPlaces; ++a) {
            for (u32 b = a + 1; b < kTouchPlaces; ++b) {
                const TouchPlacement& pa = g.places[a];
                const TouchPlacement& pb = g.places[b];
                if (pa.size <= 0.0f || pb.size <= 0.0f || a == touchIndex(TouchPlace::ClassBar) ||
                    b == touchIndex(TouchPlace::ClassBar)) {
                    continue;
                }
                const f32 d = std::hypot(pa.x - pb.x, pa.y - pb.y);
                INFO("profile " << p << ": " << touchPlaceName(static_cast<TouchPlace>(a)) << " / "
                                << touchPlaceName(static_cast<TouchPlace>(b)));
                CHECK(d >= (pa.size + pb.size) * 0.5f);
            }
        }
    }
}

TEST_CASE("spell lines: the highest known rank, the rank's number") {
    CHECK(SpellLines::rankNumber("Rank 3") == 3);
    CHECK(SpellLines::rankNumber("Rango 12") == 12);
    CHECK(SpellLines::rankNumber("") == 0);
    CHECK(SpellLines::rankNumber("Apprentice") == 0);
    const AbilityData* data = abilityData();
    if (!data) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    // Sinister Strike: ranks 1752 (1), 1757 (2), 1758 (3), 1759 (4). Known: 1, 3.
    std::vector<u32> known = {1752, 1758, 2098};
    std::sort(known.begin(), known.end());
    SpellLines lines;
    lines.update(known, data->spellText(), 1);
    CHECK(lines.highest(1752) == 1758);
    CHECK(lines.highest(1757) == 1758);
    CHECK(lines.highest(2098) == 2098);
    CHECK(lines.knows(1759));
    CHECK_FALSE(lines.knows(78)); // Heroic Strike
    CHECK(lines.highest(78) == 78);
}
