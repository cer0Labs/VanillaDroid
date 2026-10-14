// Character creation (phase 8): client-side name checks, and the races, classes and customizations of the real
// client tables (db.vdpk of the working tree; skipped without it) under the server's validation rules.
#include "game/CharCreate.h"
#include "game/GameData.h"
#include "game/Social.h"
#include "net/ResponseCodes.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>

namespace {

using namespace vd;
using net::ResponseCode;

u8 check(std::string_view name, std::string* normalized = nullptr) {
    std::string out;
    const u8 result = game::checkCharacterName(name, out);
    if (normalized) {
        *normalized = out;
    }
    return result;
}

constexpr u8 code(ResponseCode c) {
    return static_cast<u8>(c);
}

} // namespace

TEST_CASE("Character names: the original client's checks and the server's capitalization") {
    std::string n;
    CHECK(check("arthel", &n) == code(ResponseCode::CHAR_NAME_SUCCESS));
    CHECK(n == "Arthel");
    CHECK(check("ARTHEL", &n) == code(ResponseCode::CHAR_NAME_SUCCESS));
    CHECK(n == "Arthel");
    CHECK(check("\xc3\xb1" "and\xc3\xba", &n) == code(ResponseCode::CHAR_NAME_SUCCESS)); // ñandú
    CHECK(n == "\xc3\x91" "and\xc3\xba");                                           // Ñandú
    CHECK(check("\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xbd", &n) == code(ResponseCode::CHAR_NAME_SUCCESS)); // иван
    CHECK(n == "\xd0\x98\xd0\xb2\xd0\xb0\xd0\xbd");                                  // Иван
    CHECK(check("") == code(ResponseCode::CHAR_NAME_NO_NAME));
    CHECK(check("a") == code(ResponseCode::CHAR_NAME_TOO_SHORT));
    CHECK(check("Ab") == code(ResponseCode::CHAR_NAME_SUCCESS));
    CHECK(check("Abcdefghijkl") == code(ResponseCode::CHAR_NAME_SUCCESS)); // 12
    CHECK(check("Abcdefghijklm") == code(ResponseCode::CHAR_NAME_TOO_LONG));
    CHECK(check("Arthel2") == code(ResponseCode::CHAR_NAME_INVALID_CHARACTER));
    CHECK(check("Ar thel") == code(ResponseCode::CHAR_NAME_INVALID_SPACE));
    CHECK(check("Ar'thel") == code(ResponseCode::CHAR_NAME_INVALID_APOSTROPHE));
    CHECK(check("Abc\xd0\xb4") == code(ResponseCode::CHAR_NAME_MIXED_LANGUAGES)); // Latin + Cyrillic
    CHECK(check("Aaab") == code(ResponseCode::CHAR_NAME_THREE_CONSECUTIVE));
    CHECK(check("aAAb") == code(ResponseCode::CHAR_NAME_THREE_CONSECUTIVE));
    CHECK(check("Aab") == code(ResponseCode::CHAR_NAME_SUCCESS));
    CHECK(check("\xff\xfe") == code(ResponseCode::CHAR_NAME_INVALID_CHARACTER)); // not UTF-8
    CHECK(check("Ab\xc3") == code(ResponseCode::CHAR_NAME_INVALID_CHARACTER));   // cut sequence
    CHECK(check("\xc3\x97" "ab") == code(ResponseCode::CHAR_NAME_INVALID_CHARACTER)); // ×
    // Phase 18 (create): the random names are GetRandomName's (tests/game/test_char_create_rules.cpp).
}

TEST_CASE("Character creation: races, classes and customizations from the client tables") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::CharCreateData data;
    REQUIRE(data.init(db));
    // CharBaseInfo of 1.12: 40 pairs among the eight playable races.
    u32 pairs = 0;
    for (const u8 race : game::CharCreateData::kRaces) {
        for (const u8 cls : game::CharCreateData::kClasses) {
            pairs += data.canBe(race, cls) ? 1u : 0u;
        }
    }
    CHECK(pairs == 40);
    CHECK(data.canBe(1, 2));       // human paladin
    CHECK_FALSE(data.canBe(1, 3)); // no human hunters in 1.12
    CHECK(data.canBe(6, 11));      // tauren druid
    CHECK_FALSE(data.canBe(4, 2)); // no night elf paladins
    CHECK(data.canBe(8, 7));       // troll shaman
    CHECK_FALSE(data.canBe(9, 1)); // goblins are not playable
    CHECK(data.firstClass(4) == 1);
    CHECK(data.facialHairKind(7, 1) == game::FeatureKind::Earrings);
    CHECK(data.facialHairKind(8, 0) == game::FeatureKind::Tusks);
    CHECK(data.hairKind(6) == game::FeatureKind::Horns);

    // Human male: 10 skins (2 more are NPC-only), 12 faces, 12 hair styles in 10 colors, 9 facial hairs.
    game::CharAppearance a;
    a.race = 1;
    a.cls = 1;
    data.fix(a);
    CHECK(data.valid(a));
    CHECK(data.optionCount(a, game::AppearanceOption::Skin) == 10);
    CHECK(data.optionCount(a, game::AppearanceOption::Face) == 12);
    CHECK(data.optionCount(a, game::AppearanceOption::HairStyle) == 12);
    CHECK(data.optionCount(a, game::AppearanceOption::HairColor) == 10);
    CHECK(data.optionCount(a, game::AppearanceOption::FacialHair) == 9);
    // Stepping wraps around and stays valid.
    for (u32 i = 0; i < 12; ++i) {
        data.step(a, game::AppearanceOption::Face, 1);
        CHECK(data.valid(a));
    }
    CHECK(data.optionIndex(a, game::AppearanceOption::Face) == 0);
    data.step(a, game::AppearanceOption::Face, -1);
    CHECK(data.optionIndex(a, game::AppearanceOption::Face) == 11);
    // A tauren female keeps 7 facial styles (no CharSections check for them) and has horns for hair.
    a.race = 6;
    a.gender = 1;
    data.fix(a);
    CHECK(a.cls == 1); // warrior: the class stays
    CHECK(data.valid(a));
    CHECK(data.optionCount(a, game::AppearanceOption::FacialHair) == 5);
    // A race change drops a class it cannot be.
    a.cls = 2; // paladin
    a.race = 2; // orc
    data.fix(a);
    CHECK(a.cls == 1);
    // Every race and gender, random looks: all of them pass the server's rules.
    u32 seed = 77;
    for (const u8 race : game::CharCreateData::kRaces) {
        for (u8 gender = 0; gender < 2; ++gender) {
            for (u32 i = 0; i < 25; ++i) {
                game::CharAppearance r;
                r.race = race;
                r.gender = gender;
                r.cls = data.firstClass(race);
                data.randomize(r, seed);
                INFO("race " << int(race) << " gender " << int(gender));
                CHECK(data.valid(r));
            }
        }
    }
    // Not valid: an NPC-only skin, a face of no skin.
    game::CharAppearance bad;
    bad.race = 1;
    data.fix(bad);
    bad.skin = 11;
    CHECK_FALSE(data.valid(bad));

    // A human warrior starts in a shirt, pants, boots and a sword (CharStartOutfit, 1.12).
    std::array<u32, 19> displays;
    std::array<u8, 19> types;
    REQUIRE(data.startOutfit(1, 1, 0, displays, types));
    CHECK(displays[3] == 9891);  // Recruit's Shirt
    CHECK(displays[6] == 9892);  // Recruit's Pants
    CHECK(displays[7] == 10141); // Recruit's Boots
    CHECK(displays[15] == 1542); // Worn Shortsword
    CHECK(types[15] == 21);
    CHECK(displays[0] == 0);

    // The duel spell is SPELL_EFFECT_DUEL (83).
    game::GameData gameData;
    REQUIRE(gameData.init(db));
    const std::optional<game::SpellInfo> duel = gameData.spell(game::kDuelSpell);
    REQUIRE(duel.has_value());
    CHECK(duel->effects[0] == 83);
}
