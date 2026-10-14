// Phase 18 (create): the rules of WoW.exe's CharacterCreate C side (game/CharCreate): GetClassesForRace's
// buttons, ResetCharCustomize, SetSelectedRace / SetSelectedSex, and GetRandomName's names learnt from
// NameGen.dbc (synthetic rows here; the real table with VD_PACKS_DIR).
#include "game/CharCreate.h"
#include "net/ResponseCodes.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace vd;
using net::ResponseCode;

bool validName(std::string_view name) {
    std::string normalized;
    return game::checkCharacterName(name, normalized) == static_cast<u8>(ResponseCode::CHAR_NAME_SUCCESS) &&
           normalized == name;
}

std::vector<game::NameGenRow> sampleRows() {
    // As NameGen is sorted: by race, then sex (0 male).
    return {{1, 0, "tobias"}, {1, 1, "anna"}, {1, 1, "annabel"}, {2, 0, "grommash"}, {2, 1, "garona"}};
}

} // namespace

TEST_CASE("Random names: three letters at a time from NameGen's names of the race, as WoW.exe draws them") {
    const std::vector<game::NameGenRow> rows = sampleRows();
    game::NameGenerator names;
    std::array<char, 16> out{};
    u32 seed = 99;
    // A woman's names come from the race's female rows alone: "_anna" and "_annabel" make only these two.
    names.select(rows, 1, 1);
    for (u32 i = 0; i < 100; ++i) {
        const std::string name(names.generate(seed, out));
        INFO(name);
        CHECK((name == "Anna" || name == "Annabel"));
    }
    // A man's names from the first male row to the race's last: the female names too (0x475480).
    names.select(rows, 1, 0);
    std::set<std::string> seen;
    for (u32 i = 0; i < 300; ++i) {
        const std::string name(names.generate(seed, out));
        INFO(name);
        CHECK(validName(name));
        CHECK(name.size() >= 4);
        CHECK(name.size() <= 10);
        seen.insert(name);
    }
    CHECK(seen.count("Tobias") == 1);
    CHECK((seen.count("Anna") == 1 || seen.count("Annabel") == 1));
    // Another race's names never mix in.
    names.select(rows, 2, 1);
    for (u32 i = 0; i < 50; ++i) {
        CHECK(std::string(names.generate(seed, out)) == "Garona");
    }
    // No names for a race: nothing.
    names.select(rows, 5, 0);
    CHECK(names.generate(seed, out).empty());
    CHECK(names.sequenceCount() == 0);
}

TEST_CASE("Character creation rules: classes, reset, race and gender changes (client tables)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::CharCreateData data;
    REQUIRE(data.init(db));

    // GetClassesForRace: CharBaseInfo's order, the dwarf mage left out.
    std::array<u8, 8> classes{};
    auto list = [&](u8 race) {
        return std::vector<u8>(classes.begin(), classes.begin() + data.classesOf(race, classes));
    };
    CHECK(list(1) == std::vector<u8>{1, 2, 4, 5, 8, 9});
    CHECK(list(2) == std::vector<u8>{1, 3, 4, 7, 9});
    CHECK(list(3) == std::vector<u8>{1, 2, 3, 4, 5});
    CHECK(list(4) == std::vector<u8>{1, 3, 4, 5, 11});
    CHECK(list(5) == std::vector<u8>{1, 4, 5, 8, 9});
    CHECK(list(6) == std::vector<u8>{1, 3, 7, 11});
    CHECK(list(7) == std::vector<u8>{1, 4, 8, 9});
    CHECK(list(8) == std::vector<u8>{1, 3, 4, 5, 7, 8});

    // ResetCharCustomize: any of the eight races and both genders come up, with the first class and a valid
    // look.
    u32 seed = 2024;
    std::set<u8> races, genders;
    for (u32 i = 0; i < 200; ++i) {
        game::CharAppearance a;
        data.reset(a, seed);
        races.insert(a.race);
        genders.insert(a.gender);
        CHECK(a.cls == 1);
        CHECK(data.valid(a));
    }
    CHECK(races.size() == 8);
    CHECK(genders.size() == 2);

    // SetSelectedRace: the same race changes nothing; another gives its first class and a random valid look.
    game::CharAppearance a;
    a.race = 1;
    a.cls = 8; // mage
    data.fix(a);
    const game::CharAppearance before = a;
    CHECK_FALSE(data.selectRace(a, 1, seed));
    CHECK(a == before);
    CHECK(data.selectRace(a, 6, seed));
    CHECK(a.race == 6);
    CHECK(a.cls == 1);
    CHECK(data.valid(a));
    // SetSelectedSex: the class stays, the look is drawn again for the other gender; the same gender changes
    // nothing.
    a.cls = 11;
    const game::CharAppearance druid = a;
    CHECK_FALSE(data.selectGender(a, a.gender, seed));
    CHECK(a == druid);
    CHECK(data.selectGender(a, a.gender == 0 ? 1 : 0, seed));
    CHECK(a.cls == 11);
    CHECK(data.valid(a));

    // NameGen.dbc: names for every playable race and gender, all of them usable.
    CHECK(data.nameGen().size() > 1000);
    game::NameGenerator names;
    std::array<char, 16> out{};
    for (const u8 race : game::CharCreateData::kRaces) {
        names.select(data.nameGen(), race, 1);
        const u32 female = names.sequenceCount();
        names.select(data.nameGen(), race, 0);
        // The male table counts the female names as well.
        CHECK(names.sequenceCount() >= female);
        for (u8 gender = 0; gender < 2; ++gender) {
            names.select(data.nameGen(), race, gender);
            for (u32 i = 0; i < 40; ++i) {
                const std::string name(names.generate(seed, out));
                INFO("race " << int(race) << " gender " << int(gender) << ": " << name);
                CHECK(validName(name));
                CHECK(name.size() >= 4);
                CHECK(name.size() <= 10);
            }
        }
    }
}
