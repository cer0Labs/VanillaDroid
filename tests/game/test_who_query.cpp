// Retouches (friends frame): a who query's text read as 1.12's SendWho (WoW.exe 0x5aebb0): tags, levels, quotes,
// the words searched and their limits (game/WhoQuery).
#include "game/WhoQuery.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>

namespace {

using namespace vd;
using namespace vd::game;

constexpr std::array<std::string_view, 10> kRaces = {"",          "Human",  "Orc",    "Dwarf", "Night Elf",
                                                     "Undead",    "Tauren", "Gnome",  "Troll", "Goblin"};
constexpr std::array<std::string_view, 10> kClasses = {"",      "Warrior", "Paladin", "Hunter", "Rogue",
                                                       "Priest", "",        "Shaman",  "Mage",   "Warlock"};
constexpr std::array<u32, 4> kZones = {12, 1519, 40, 1537};
constexpr std::array<std::string_view, 4> kZoneNames = {"Elwynn Forest", "Stormwind City", "Westfall", "Ironforge"};

WhoQuery parse(std::string_view text) {
    WhoQueryNames names;
    names.races = kRaces;
    names.classes = kClasses;
    names.zones = kZones;
    names.zoneNames = kZoneNames;
    WhoQuery q;
    parseWhoQuery(text, names, q);
    return q;
}

} // namespace

TEST_CASE("Who query: the defaults") {
    const WhoQuery q = parse("");
    CHECK(q.minLevel == 0);
    CHECK(q.maxLevel == 100);
    CHECK(q.raceMask == 0xFFFFFFFFu);
    CHECK(q.classMask == 0xFFFFFFFFu);
    CHECK(q.zoneCount == 0);
    CHECK(q.termCount == 0);
    CHECK(q.name.empty());
    CHECK(q.guild.empty());
}

TEST_CASE("Who query: levels") {
    CHECK(parse("60").minLevel == 60);
    CHECK(parse("60").maxLevel == 60);
    CHECK(parse("10-20").minLevel == 10);
    CHECK(parse("10-20").maxLevel == 20);
    CHECK(parse("10-").maxLevel == 100);
    CHECK(parse("10abc").maxLevel == 10);
    CHECK(parse("-20").minLevel == 0);
    CHECK(parse("-20").maxLevel == 20);
    CHECK(parse("10-20 30").minLevel == 30); // the last one wins, no check of min <= max
    const WhoQuery dash = parse("-abc");     // not a level: searched
    CHECK(dash.termCount == 1);
    CHECK(dash.terms[0] == "-abc");
}

TEST_CASE("Who query: tags, quotes and case") {
    const WhoQuery q = parse("N-Bob g-\"The Wanderers\" Z-elwynn");
    CHECK(q.name == "Bob");
    CHECK(q.guild == "The Wanderers");
    REQUIRE(q.zoneCount == 1);
    CHECK(q.zones[0] == 12);
    // A zone is found by part of its name, every one that matches; none found: zone 0 (nobody).
    const WhoQuery s = parse("z-st");
    REQUIRE(s.zoneCount == 3); // "Elwynn Forest", "Stormwind City" and "Westfall" contain "st"
    CHECK(s.zones[0] == 12);
    CHECK(s.zones[1] == 1519);
    CHECK(s.zones[2] == 40);
    const WhoQuery none = parse("z-Durotar");
    REQUIRE(none.zoneCount == 1);
    CHECK(none.zones[0] == 0);
    // The quote may hold the whole word; the case of what is searched stays.
    CHECK(parse("\"z-Stormwind City\"").zones[0] == 1519);
    CHECK(parse("Elwynn").terms[0] == "Elwynn");
    // A word left inside an unclosed quote is dropped.
    CHECK(parse("bob \"elwynn").termCount == 1);
    // "" is an empty word searched.
    const WhoQuery empty = parse("\"\"");
    CHECK(empty.termCount == 1);
    CHECK(empty.terms[0].empty());
    // The name and guild keep 47 bytes, the last one wins.
    CHECK(parse("n-a n-b").name == "b");
    CHECK(parse("n-" + std::string(60, 'x')).name.size() == 47);
}

TEST_CASE("Who query: races and classes by part of their names") {
    const WhoQuery war = parse("c-war");
    CHECK(war.classMask == ((1u << 1) | (1u << 9))); // Warrior and Warlock
    CHECK(parse("c-mage c-priest").classMask == ((1u << 8) | (1u << 5)));
    CHECK(parse("r-elf").raceMask == (1u << 4));
    CHECK(parse("c-nothing").classMask == 0); // no match: nobody
    CHECK(parse("mage").classMask == 0xFFFFFFFFu); // a bare word is searched, not a class
}

TEST_CASE("Who query: four words searched end the reading") {
    const WhoQuery q = parse("a b c d 60 n-Bob e");
    CHECK(q.termCount == 4);
    CHECK(q.terms[3] == "d");
    CHECK(q.minLevel == 0); // ignored after the fourth
    CHECK(q.name.empty());
    // Only the first 127 bytes count.
    const std::string long_ = std::string(126, 'x') + " 60";
    CHECK(parse(long_).minLevel == 0);
}

TEST_CASE("Who query: an empty /who asks the player's zone and levels") {
    CHECK(defaultWhoQuery("Elwynn Forest", 12) == "z-\"Elwynn Forest\" 9-15");
    CHECK(defaultWhoQuery("Elwynn Forest", 2) == "z-\"Elwynn Forest\" 1-5");
    const WhoQuery q = parse(defaultWhoQuery("Westfall", 20));
    CHECK(q.zones[0] == 40);
    CHECK(q.minLevel == 17);
    CHECK(q.maxLevel == 23);
}
