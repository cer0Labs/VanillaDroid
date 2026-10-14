// Phase 11: which units are drawn translucent or tinted, and how much (game/UnitFade): the SpellVisualKit procs of
// their auras' state kits, from the 1.12.1 client's tables (VD_PACKS_DIR), and the rules around them.
#include "game/UnitFade.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::game;

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

bool openPack(pack::PackReader& pack, const char* name) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && pack.open(std::filesystem::path(dir) / name).ok();
}

void checkTint(const glm::vec3& tint, u32 rgb) {
    CHECK(tint.x == doctest::Approx(static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f));
    CHECK(tint.y == doctest::Approx(static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f));
    CHECK(tint.z == doctest::Approx(static_cast<f32>(rgb & 0xFF) / 255.0f));
}

Entity unitWithAuras(std::initializer_list<u32> spells) {
    Entity e;
    e.type = ObjectType::Player;
    e.values.resize(net::uf::PLAYER_END, 0);
    u16 slot = 0;
    for (u32 spell : spells) {
        e.values[net::uf::UNIT_FIELD_AURA + slot++] = spell;
    }
    return e;
}

} // namespace

TEST_CASE("Unit fades: the character procs of a visual kit") {
    // Kit 989 (the Ghost aura's state): proc 1 = color 0x8CB9FD, proc 14 = opacity 0.5. Parameter p of proc i at
    // [p * 4 + i], as SpellVisualKit.dbc lays them.
    std::array<f32, 16> ghost{};
    ghost[0] = 9222653.0f; // 0x8CB9FD
    ghost[1] = 0.5f;
    const UnitFade g = fadeFromProcs({1, 14, kNone, kNone}, ghost);
    CHECK(g.alpha == doctest::Approx(0.5f));
    checkTint(g.tint, 0x8CB9FD);
    CHECK(g.fadeSeconds == 0.0f);

    // Kit 312 (Stealth): proc 7 (not ours) and 14 = 0.3, faded in over 1000 ms.
    std::array<f32, 16> stealth{};
    stealth[0] = 1.0f;
    stealth[1] = 0.3f;
    stealth[4] = 1.0f;
    stealth[5] = 2.0f;
    stealth[9] = 1000.0f;
    const UnitFade s = fadeFromProcs({7, 14, kNone, kNone}, stealth);
    CHECK(s.alpha == doctest::Approx(0.3f));
    CHECK(s.tint == glm::vec3(1.0f));
    CHECK(s.fadeSeconds == doctest::Approx(1.0f));

    // Phase 14: a color with more parameters (Immolate's burn, kit 235) tints steadily too: WoW.exe reads
    // only p0.
    std::array<f32, 16> immolate{};
    immolate[0] = 15160897.0f; // 0xE75641
    immolate[4] = 0.8f;
    immolate[8] = 0.2f;
    immolate[12] = 0.2f;
    const UnitFade burning = fadeFromProcs({1, kNone, kNone, kNone}, immolate);
    CHECK(burning.alpha == 1.0f);
    checkTint(burning.tint, 0xE75641);
    // A steady one (Stoneshield, kit 4069: 0x626262) tints.
    std::array<f32, 16> stone{};
    stone[0] = 6447714.0f;
    const UnitFade st = fadeFromProcs({1, kNone, kNone, kNone}, stone);
    CHECK(st.alpha == 1.0f);
    checkTint(st.tint, 0x626262);
}

TEST_CASE("Unit fades: several auras, and the fade over time") {
    UnitFade ghost{0.5f, glm::vec3(0.5f, 0.75f, 1.0f), 0.0f};
    UnitFade stealth{0.3f, glm::vec3(1.0f), 1.0f};
    const UnitFade both = combineFades(ghost, stealth);
    CHECK(both.alpha == doctest::Approx(0.3f)); // the most translucent wins, with its fade time
    CHECK(both.fadeSeconds == doctest::Approx(1.0f));
    CHECK(both.tint.x == doctest::Approx(0.5f)); // tints multiply
    CHECK(combineFades(UnitFade{}, UnitFade{}) == UnitFade{});

    // Into stealth over a second, back out at once (the aura gone: no fade time).
    glm::vec4 shown(1.0f);
    shown = approachFade(shown, stealth, 0.5f);
    CHECK(shown.w == doctest::Approx(0.5f));
    shown = approachFade(shown, stealth, 0.5f);
    CHECK(shown.w == doctest::Approx(0.3f));
    shown = approachFade(shown, stealth, 0.5f);
    CHECK(shown.w == doctest::Approx(0.3f));
    shown = approachFade(shown, UnitFade{}, 0.016f);
    CHECK(shown == glm::vec4(1.0f));
    // A ghost at once.
    shown = approachFade(shown, ghost, 0.016f);
    CHECK(shown == glm::vec4(ghost.tint, 0.5f));
}

TEST_CASE("Unit fades: ghosts, stealth and invisibility in the 1.12.1 tables (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openPack(db, "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    UnitFades fades;
    REQUIRE(fades.init(db));
    CHECK(fades.size() > 50);

    // Ghost (8326, what vMaNGOS casts on release) and the other ghost aura with its visual (9036).
    for (u32 spell : {8326u, 9036u}) {
        const UnitFade* ghost = fades.ofSpell(spell);
        REQUIRE(ghost);
        CHECK(ghost->alpha == doctest::Approx(0.5f));
        checkTint(ghost->tint, 0x8CB9FD);
    }
    CHECK(fades.ofSpell(20584) == nullptr); // the night elf's wisp aura has no visual: its model does the rest
    // Stealth (every rank), Prowl and Vanish's stealth: 0.3, fading in over a second.
    for (u32 spell : {1784u, 1785u, 1786u, 1787u, 5215u, 1856u}) {
        const UnitFade* stealth = fades.ofSpell(spell);
        REQUIRE(stealth);
        CHECK(stealth->alpha == doctest::Approx(0.3f));
        CHECK(stealth->tint == glm::vec3(1.0f));
        CHECK(stealth->fadeSeconds == doctest::Approx(1.0f));
    }
    // Invisibility potions (Lesser Invisibility 3680, Invisibility 885) and the priest's Fade (586): 0.5.
    for (u32 spell : {3680u, 885u, 586u}) {
        const UnitFade* invisible = fades.ofSpell(spell);
        REQUIRE(invisible);
        CHECK(invisible->alpha == doctest::Approx(0.5f));
    }
    // Shadowform: 0.65 and dark purple.
    const UnitFade* shadowform = fades.ofSpell(15473);
    REQUIRE(shadowform);
    CHECK(shadowform->alpha == doctest::Approx(0.65f));
    checkTint(shadowform->tint, 0x270042);
    // Phase 14: Frostbolt's chill and Immolate's burn tint their target steadily (WoW.exe reads p0 only).
    for (u32 spell : {116u, 348u}) {
        const UnitFade* tinted = fades.ofSpell(spell);
        REQUIRE(tinted);
        CHECK(tinted->alpha == 1.0f);
        CHECK(tinted->tint != glm::vec3(1.0f));
    }
    // Spells whose visual does nothing of the kind.
    CHECK(fades.ofSpell(1243) == nullptr); // Power Word: Fortitude
    CHECK(fades.ofSpell(0) == nullptr);

    // A unit by its visible auras: a ghost with Power Word: Fortitude (1243) is a ghost; nothing on: as it is.
    const UnitFade ghost = fades.ofUnit(unitWithAuras({1243, 8326}));
    CHECK(ghost.alpha == doctest::Approx(0.5f));
    CHECK(fades.ofUnit(unitWithAuras({1243})) == UnitFade{});
    CHECK(fades.ofUnit(unitWithAuras({})) == UnitFade{});
    Entity object;
    object.type = ObjectType::GameObject;
    object.values.resize(net::uf::UNIT_END, 8326);
    CHECK(fades.ofUnit(object) == UnitFade{}); // only units wear auras
}

TEST_CASE("Unit fades: cost of a unit's look (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openPack(db, "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    UnitFades fades;
    REQUIRE(fades.init(db));
    // A player with a dozen buffs and debuffs, the last one Stealth: every slot is looked up.
    const Entity unit = unitWithAuras({1243, 1459, 6673, 21562, 467, 1126, 19740, 20217, 1038, 774, 8936, 1784});
    constexpr u32 kRounds = 100000;
    glm::vec4 shown(1.0f);
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRounds; ++i) {
        shown = approachFade(shown, fades.ofUnit(unit), 0.016f);
    }
    const f64 us = std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("ofUnit + approachFade: " << us * 1000.0 / kRounds << " ns per unit and frame");
    CHECK(shown.w == doctest::Approx(0.3f));
}
