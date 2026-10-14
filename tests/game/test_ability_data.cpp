// Phase-8 client tables against the real baked db.vdpk (VD_PACKS_DIR; skipped without it): spell descriptions
// with their tokens resolved, tooltip lines, spell book skill lines, talent trees and factions.
#include "TestData.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/Progress.h"
#include "game/SpellBookTabs.h"
#include "game/SpellText.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace vd;

// The tables, loaded once for every test case of this file (nullptr without the packs).
const game::AbilityData* abilityData() {
    static pack::PackReader db;
    static game::AbilityData data;
    static int state = 0; // 0 not tried, 1 ready, 2 missing
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

#define REQUIRE_ABILITY_DATA(var)                                                                            \
    const game::AbilityData* var = abilityData();                                                            \
    if (!var) {                                                                                              \
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");                                          \
        return;                                                                                              \
    }

// The interface's texts (ui.vdpk, English) for the test case's life: tooltip lines and the ranges and
// durations of the descriptions are the client's texts.
struct Texts {
    bool loaded = test::loadTexts();
    ~Texts() { text::unload(); }
};

std::string describe(const game::SpellText& text, u32 spell, u32 level = 0, u8 gender = 0) {
    std::string out;
    REQUIRE(game::spellDescription(text, spell, out, {level, gender}));
    return out;
}

// What a description must read: the client's own template (Spell.dbc, from the pack) with each of its tokens
// replaced by the value the resolver has to give it.
std::string expected(const game::SpellText& text, u32 spell,
                     std::initializer_list<std::pair<std::string_view, std::string_view>> tokens, bool aura = false) {
    game::SpellDetails details;
    REQUIRE(text.details(spell, details));
    std::string out(aura ? details.auraText : details.description);
    for (const auto& [token, value] : tokens) {
        const usize at = out.find(token);
        REQUIRE_MESSAGE(at != std::string::npos, std::string(token));
        out.replace(at, token.size(), value);
    }
    CHECK(out.find('$') == std::string::npos); // every token given
    return out;
}

} // namespace

TEST_CASE("Spell descriptions: tokens resolved from Spell.dbc") {
    REQUIRE_ABILITY_DATA(data);
    const Texts texts;
    REQUIRE(texts.loaded);
    const game::SpellText& text = data->spellText();
    // $s1 as a range, $o2 over the periodic ticks, $d.
    CHECK(describe(text, 133) == expected(text, 133, {{"$s1", "14 to 22"}, {"$o2", "2"}, {"$d", "4 sec"}}));
    // Negative points shown as their size ($s1%).
    CHECK(describe(text, 116) == expected(text, 116, {{"$s2", "18 to 20"}, {"$s1", "40"}, {"$d", "5 sec"}}));
    // Another spell's numbers ($6136s2, $6136d) and minutes.
    CHECK(describe(text, 168) == expected(text, 168,
                                          {{"$s1", "30"},
                                           {"$6136s2", "30"},
                                           {"$6136s1", "25"},
                                           {"$6136d", "5 sec"},
                                           {"$d", "30 min"}}));
    // $o1 and $t1 of a periodic aura; $a1 radius.
    CHECK(describe(text, 589) == expected(text, 589, {{"$o1", "30"}, {"$d", "18 sec"}}));
    CHECK(describe(text, 6673) == expected(text, 6673, {{"$a1", "20"}, {"$s1", "15"}, {"$d", "2 min"}}));
    // $lsingular:plural; after a number.
    CHECK(describe(text, 1752) == expected(text, 1752, {{"$s1", "3"}, {"$s2", "1"}, {"$lpoint:points;", "point"}}));
    // $/1000;s1.
    CHECK(describe(text, 2641) == expected(text, 2641, {{"$/1000;s1", "50"}}));
    // $*10;F1 (damage multiplier) and per-level growth with the caster's level.
    CHECK(describe(text, 5308).find("additional damage") != std::string::npos);
    CHECK(describe(text, 5308).find('$') == std::string::npos);
    CHECK(describe(text, 133, 5) == expected(text, 133, {{"$s1", "16 to 24"}, {"$o2", "2"}, {"$d", "4 sec"}}));
    // The aura tooltip.
    std::string aura;
    REQUIRE(game::auraDescription(text, 589, aura));
    CHECK(aura == expected(text, 589, {{"$s1", "5"}, {"$t1", "3"}}, true));
    // Unknown spells say so.
    std::string none = "x";
    CHECK_FALSE(game::spellDescription(text, 999999, none));
    CHECK(none.empty());

    // No token is left unresolved in the descriptions of the spells a level 1-10 character meets.
    for (const u32 spell :
         {133u,  116u,  168u,   2050u,  6673u, 7384u, 688u,  75u,   5019u, 589u,   17u,   774u,  1459u, 772u,
          8921u, 2136u, 1515u,  14251u, 6572u, 2973u, 1752u, 2098u, 53u,   1966u,  6991u, 3044u, 78u,   100u,
          1160u, 2457u, 20271u, 21084u, 635u,  639u,  465u,  5176u, 5185u, 1126u,  8921u, 403u,  331u,  8017u,
          8042u, 324u,  687u,   172u,   348u,  702u,  1454u, 5782u, 1978u, 13163u, 1494u}) {
        INFO("spell " << spell);
        CHECK(describe(text, spell, 10).find('$') == std::string::npos);
    }
}

TEST_CASE("Spell tooltip lines with the client's texts") {
    REQUIRE_ABILITY_DATA(data);
    const Texts texts;
    REQUIRE(texts.loaded);
    const game::SpellText& text = data->spellText();
    char line[96];
    game::SpellDetails fireball;
    REQUIRE(text.details(133, fireball));
    CHECK(fireball.name == "Fireball");
    CHECK(fireball.rank == "Rank 1");
    CHECK(game::formatSpellCost(line, sizeof(line), fireball) > 0);
    CHECK(std::string(line) == "30 Mana");
    game::formatSpellRange(line, sizeof(line), fireball);
    CHECK(std::string(line) == "35 yd range");
    game::formatSpellCastTime(line, sizeof(line), fireball);
    CHECK(std::string(line) == "1.5 sec cast");
    CHECK(game::formatSpellCooldown(line, sizeof(line), fireball) == 0);

    game::SpellDetails rend, overpower, charge, autoShot, riposte, frostNova;
    REQUIRE(text.details(772, rend));
    game::formatSpellCost(line, sizeof(line), rend);
    CHECK(std::string(line) == "10 Rage"); // rage in tenths
    game::formatSpellRange(line, sizeof(line), rend);
    CHECK(std::string(line) == "Melee Range");
    game::formatSpellCastTime(line, sizeof(line), rend);
    CHECK(std::string(line) == "Instant"); // an ability: no mana
    REQUIRE(text.details(122, frostNova));
    game::formatSpellCastTime(line, sizeof(line), frostNova);
    CHECK(std::string(line) == "Instant cast"); // a spell
    game::formatSpellCooldown(line, sizeof(line), frostNova);
    CHECK(std::string(line) == "25 sec cooldown");
    REQUIRE(text.details(7384, overpower));
    CHECK(overpower.needsComboPoints()); // usable after the target dodged: the warrior's combo point
    game::formatSpellCooldown(line, sizeof(line), overpower);
    CHECK(std::string(line) == "5 sec cooldown");
    REQUIRE(text.details(100, charge));
    game::formatSpellRange(line, sizeof(line), charge);
    CHECK(std::string(line) == "8-25 yd range");
    REQUIRE(text.details(75, autoShot));
    CHECK(autoShot.autoRepeat());
    REQUIRE(text.details(14251, riposte));
    CHECK(riposte.casterAuraState == 1); // AURA_STATE_DEFENSE: after a parry
    game::SpellDetails tame;
    REQUIRE(text.details(1515, tame));
    CHECK(tame.channeled());
    game::formatSpellCastTime(line, sizeof(line), tame);
    CHECK(std::string(line) == "Channeled");

    char duration[32];
    game::formatSpellDuration(duration, sizeof(duration), 1800000);
    CHECK(std::string(duration) == "30 min");
    game::formatSpellDuration(duration, sizeof(duration), 3600000);
    CHECK(std::string(duration) == "1 hour");
    game::formatSpellDuration(duration, sizeof(duration), 7200000);
    CHECK(std::string(duration) == "2 hrs");
    game::formatSpellDuration(duration, sizeof(duration), 1500);
    CHECK(std::string(duration) == "1.50 sec");
}

TEST_CASE("Skill lines and the spell book's skill of each spell") {
    REQUIRE_ABILITY_DATA(data);
    game::SkillLineInfo fire;
    REQUIRE(data->skillLine(8, fire));
    CHECK(fire.name == "Fire");
    CHECK(fire.category == game::SkillCategory::Class);
    CHECK(data->spellText().iconHash(fire.spellIcon) != 0);
    game::SkillLineInfo cooking;
    REQUIRE(data->skillLine(185, cooking));
    CHECK(cooking.category == game::SkillCategory::Secondary);
    // The skill window's group titles (SkillLineCategory.dbc).
    CHECK(data->skillCategoryName(game::SkillCategory::Class) == "Class Skills");
    CHECK(data->skillCategoryName(game::SkillCategory::Weapon) == "Weapon Skills");
    CHECK(data->skillCategoryName(game::SkillCategory::Armor) == "Armor Proficiencies");
    CHECK(data->skillCategoryName(game::SkillCategory::Profession) == "Professions");
    CHECK(data->skillCategoryName(static_cast<game::SkillCategory>(99)).empty());
    CHECK(data->skillOfSpell(133, 1, 8) == 8);   // Fireball: Fire (a human mage)
    CHECK(data->skillOfSpell(116, 1, 8) == 6);   // Frostbolt: Frost
    CHECK(data->skillOfSpell(772, 2, 1) == 26);  // Rend: Arms (an orc warrior)
    CHECK(data->skillOfSpell(2050, 1, 5) == 56); // Lesser Heal: Holy (a human priest)
    CHECK(data->skillOfSpell(999999, 1, 1) == 0);
}

TEST_CASE("Talent trees, talents and prerequisites") {
    REQUIRE_ABILITY_DATA(data);
    std::array<game::TalentTabInfo, 4> tabs{};
    REQUIRE(data->talentTabs(8, tabs) == 3); // mage
    CHECK(tabs[0].name == "Arcane");
    CHECK(tabs[1].name == "Fire");
    CHECK(tabs[2].name == "Frost");
    CHECK(tabs[1].background == "MageFire");
    REQUIRE(data->talentTabs(1, tabs) == 3); // warrior
    CHECK(tabs[0].name == "Arms");
    CHECK(tabs[2].name == "Protection");
    const std::span<const game::TalentInfo> arms = data->talents(tabs[0].id);
    REQUIRE(arms.size() > 10);
    for (usize i = 1; i < arms.size(); ++i) {
        CHECK((arms[i - 1].row < arms[i].row ||
               (arms[i - 1].row == arms[i].row && arms[i - 1].column < arms[i].column)));
    }
    // Improved Heroic Strike: the first Arms talent, three ranks.
    u32 rank = 99;
    const game::TalentInfo* improved = data->talentOfSpell(12282, rank);
    REQUIRE(improved != nullptr);
    CHECK(rank == 0);
    CHECK(improved->rankCount() == 3);
    CHECK(improved->row == 0);
    CHECK(data->talent(improved->id) == improved);
    // Talents with a prerequisite point at a talent of the same tree.
    bool any = false;
    for (const game::TalentInfo& t : arms) {
        if (t.dependsOn != 0) {
            const game::TalentInfo* other = data->talent(t.dependsOn);
            REQUIRE(other != nullptr);
            CHECK(other->tab == t.tab);
            CHECK(other->row < t.row);
            any = true;
        }
    }
    CHECK(any);
}

TEST_CASE("Factions and reputation ranks") {
    REQUIRE_ABILITY_DATA(data);
    // Stormwind (72): a human starts Friendly... no: the base of the faction table, by race.
    const game::FactionInfo* stormwind = data->faction(72);
    REQUIRE(stormwind != nullptr);
    CHECK(stormwind->name == "Stormwind");
    REQUIRE(stormwind->listId >= 0);
    CHECK(data->factionByList(static_cast<u32>(stormwind->listId)) == stormwind);
    CHECK(stormwind->baseReputation(1, 1) >= 0); // human
    CHECK(stormwind->baseReputation(2, 1) < 0);  // orc
    CHECK(game::reputationRank(-42000) == 0);    // Hated
    CHECK(game::reputationRank(-6001) == 0);
    CHECK(game::reputationRank(-6000) == 1); // Hostile
    CHECK(game::reputationRank(-1) == 2);    // Unfriendly
    CHECK(game::reputationRank(0) == 3);     // Neutral
    CHECK(game::reputationRank(3000) == 4);  // Friendly
    CHECK(game::reputationRank(9000) == 5);  // Honored
    CHECK(game::reputationRank(21000) == 6); // Revered
    CHECK(game::reputationRank(42000) == 7); // Exalted
    CHECK(game::reputationRank(50000) == 7);
    CHECK(game::reputationRankStart(3) == 0);
    CHECK(game::reputationRankStart(4) == 3000);
    CHECK(game::reputationRankSize(5) == 12000);
}

TEST_CASE("Spell book tabs: General with the professions, the class's trees; highest ranks") {
    REQUIRE_ABILITY_DATA(data);
    game::SpellBookTabs tabs;
    // A human mage (Fireball ranks 1 and 2, Frost spells, racials, proficiencies, professions, hidden
    // spells; phase 13: a First Aid recipe, Linen Bandage, and an Enchanting craft, Enchant Bracer - Minor Health).
    const std::vector<u32> known = {133, 143, 168, 116,  6603, 20600, 20599, 9078, 668,  81,  203,
                                    204, 522, 2479, 5009, 227,  7266,  3273,  2550, 3275, 7418};
    tabs.update(*data, known, 1, 8, false);
    REQUIRE(tabs.tabs().size() == 3); // phase 13: 1.12 has no professions tab
    CHECK(tabs.tabs()[0].kind == game::SpellTabKind::General);
    CHECK(tabs.tabs()[1].kind == game::SpellTabKind::Class);
    CHECK(tabs.tabs()[1].name == "Fire");
    CHECK(tabs.tabs()[1].skillLine == 8);
    CHECK(tabs.tabs()[2].name == "Frost"); // Arcane has no known spell: no tab
    // General: Attack, the racials (Diplomacy is passive); proficiencies and languages are not listed.
    const std::span<const game::SpellBookEntry> general = tabs.entries(0);
    u32 tab0 = 0, index0 = 0;
    std::vector<u32> generalIds;
    for (const game::SpellBookEntry& e : general) {
        generalIds.push_back(e.spellId);
    }
    CHECK(std::find(generalIds.begin(), generalIds.end(), 6603u) != generalIds.end());
    CHECK(std::find(generalIds.begin(), generalIds.end(), 20600u) != generalIds.end());
    CHECK(std::find(generalIds.begin(), generalIds.end(), 9078u) == generalIds.end());
    CHECK(std::find(generalIds.begin(), generalIds.end(), 668u) == generalIds.end());
    CHECK(std::find(generalIds.begin(), generalIds.end(), 7266u) == generalIds.end());
    // Phase 13: the professions' own spells open their windows from General (SkillRaceClassInfo flag 0x80); recipes
    // and crafts are only in those windows.
    CHECK(std::find(generalIds.begin(), generalIds.end(), 3273u) != generalIds.end());
    CHECK(std::find(generalIds.begin(), generalIds.end(), 2550u) != generalIds.end());
    CHECK_FALSE(tabs.find(3275, tab0, index0));
    CHECK_FALSE(tabs.find(7418, tab0, index0));
    // Only the highest rank of Fireball by default; both with the toggle.
    REQUIRE(tabs.entries(1).size() == 1);
    CHECK(tabs.entries(1)[0].spellId == 143);
    CHECK(tabs.entries(1)[0].rankNumber == 2);
    CHECK(tabs.highestRank(143) == 143);
    tabs.update(*data, known, 1, 8, true);
    REQUIRE(tabs.entries(1).size() == 2);
    CHECK(tabs.entries(1)[0].spellId == 133);
    CHECK_FALSE(tabs.entries(1)[0].highest);
    CHECK(tabs.highestRank(133) == 143);
    u32 tab = 9, index = 9;
    REQUIRE(tabs.find(116, tab, index));
    CHECK(tab == 2);
    CHECK_FALSE(tabs.find(9078, tab, index));
}

TEST_CASE("Talent status: rows open every 5 points, prerequisites, free points") {
    REQUIRE_ABILITY_DATA(data);
    std::array<game::TalentTabInfo, 4> trees{};
    REQUIRE(data->talentTabs(1, trees) == 3); // warrior
    const std::span<const game::TalentInfo> arms = data->talents(trees[0].id);
    game::SpellBook book;
    const game::TalentInfo* first = nullptr;
    const game::TalentInfo* secondRow = nullptr;
    const game::TalentInfo* dependent = nullptr;
    for (const game::TalentInfo& t : arms) {
        first = !first && t.row == 0 ? &t : first;
        secondRow = !secondRow && t.row == 1 && t.dependsOn == 0 ? &t : secondRow;
        dependent = !dependent && t.dependsOn != 0 ? &t : dependent;
    }
    REQUIRE(first != nullptr);
    REQUIRE(secondRow != nullptr);
    REQUIRE(dependent != nullptr);
    game::TalentStatus s = game::talentStatus(*data, *first, book, 1);
    CHECK(s.rank == 0);
    CHECK(s.rowOpen);
    CHECK(s.available);
    CHECK_FALSE(game::talentStatus(*data, *first, book, 0).available); // no free points
    CHECK_FALSE(game::talentStatus(*data, *secondRow, book, 5).rowOpen);
    // Five points in row 0 open row 1 (ranks of the row-0 talents, the highest rank spell known).
    u32 points = 0;
    for (const game::TalentInfo& t : arms) {
        for (u32 r = 0; t.row == 0 && r < t.rankCount() && points < 5; ++r) {
            if (r > 0) {
                book.onSuperceded(t.ranks[r - 1], t.ranks[r]); // the server replaces the rank
            } else {
                book.onLearned(t.ranks[r]);
            }
            ++points;
        }
    }
    CHECK(game::treePoints(*data, trees[0].id, book) == 5);
    CHECK(game::talentStatus(*data, *secondRow, book, 1).available);
    CHECK(game::talentRank(*first, book) >= 1);
    // A full talent can take no more points.
    const game::TalentInfo& full = *first;
    for (u32 r = 0; r < full.rankCount(); ++r) {
        book.onLearned(full.ranks[r]);
    }
    s = game::talentStatus(*data, full, book, 3);
    CHECK(s.rank == full.rankCount());
    CHECK_FALSE(s.available);
    // A prerequisite not met.
    const game::TalentStatus dep = game::talentStatus(*data, *dependent, book, 10);
    const game::TalentInfo* needed = data->talent(dependent->dependsOn);
    REQUIRE(needed != nullptr);
    CHECK(dep.prerequisiteMet == (game::talentRank(*needed, book) > dependent->dependsOnRank));
}

TEST_CASE("Shapeshift forms: bonus bars, stances and the spells each form allows") {
    REQUIRE_ABILITY_DATA(data);
    CHECK(data->formBonusBar(0) == 0);
    CHECK(data->formBonusBar(1) == 1);    // cat
    CHECK(data->formBonusBar(5) == 3);    // bear
    CHECK(data->formBonusBar(0x11) == 1); // battle stance
    CHECK(data->formBonusBar(0x12) == 2); // defensive stance
    CHECK(data->formBonusBar(0x13) == 3); // berserker stance
    CHECK(data->formBonusBar(0x1E) == 1); // stealth
    CHECK(data->formIsStance(0x11));
    CHECK(data->formIsStance(0x1E));
    CHECK_FALSE(data->formIsStance(1));
    CHECK_FALSE(data->formIsStance(5));
    const game::SpellText& text = data->spellText();
    game::SpellDetails heal, overpower, claw, charge, rend;
    REQUIRE(text.details(2050, heal));      // Lesser Heal: not in Shadowform, fine without a form
    REQUIRE(text.details(7384, overpower)); // Battle Stance only
    REQUIRE(text.details(1082, claw));      // Cat Form only
    REQUIRE(text.details(100, charge));
    REQUIRE(text.details(772, rend)); // Battle or Defensive Stance
    CHECK(data->castableInForm(heal, 0));
    CHECK_FALSE(data->castableInForm(heal, 28)); // Shadowform
    CHECK_FALSE(data->castableInForm(heal, 5));  // a bear heals nobody
    CHECK(data->castableInForm(overpower, 0x11));
    CHECK_FALSE(data->castableInForm(overpower, 0x12));
    CHECK_FALSE(data->castableInForm(overpower, 0));
    CHECK(data->castableInForm(claw, 1));
    CHECK_FALSE(data->castableInForm(claw, 0));
    CHECK_FALSE(data->castableInForm(claw, 5));
    CHECK(data->castableInForm(charge, 0x11));
    CHECK(data->castableInForm(rend, 0x12));
    CHECK_FALSE(data->castableInForm(rend, 0x13));
}
