// Phase 16 (pullouts): Blizzard_RaidUI.xml's pullout geometry, RAID_CLASS_COLORS and where a dropped pullout
// stays on a touch screen (app/RaidPulloutLayout).
#include "app/RaidPulloutLayout.h"
#include "game/RaidRoster.h"

#include <doctest/doctest.h>

#include <array>

namespace {

using namespace vd;
using ui::Rect;

bool inside(const Rect& r, const Rect& safe) {
    return r.x >= safe.x - 0.01f && r.y >= safe.y - 0.01f && r.right() <= safe.right() + 0.01f &&
           r.bottom() <= safe.bottom() + 0.01f;
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

} // namespace

TEST_CASE("Raid pullout geometry: RaidPulloutFrameTemplate and RaidPulloutButtonTemplate in units") {
    RaidPulloutGeometry at;
    at.x = 100.0f;
    at.y = 50.0f;
    at.unit = 1.0f;
    at.members = 5;
    // 72 wide, n * RAID_PULLOUT_BUTTON_HEIGHT + 14 high; the name 13 over it.
    CHECK(at.frame().w == doctest::Approx(72.0f));
    CHECK(at.frame().h == doctest::Approx(5 * 33.0f + 14.0f));
    CHECK(at.title().y == doctest::Approx(37.0f));
    // The first button: TOP 1 right and 10 under the frame's (57 x 25: x 8.5 + 1), the next 8 under it.
    CHECK(at.button(0).x == doctest::Approx(108.5f));
    CHECK(at.button(0).y == doctest::Approx(60.0f));
    CHECK(at.button(1).y - at.button(0).bottom() == doctest::Approx(8.0f));
    // Health 51 x 4 at (3, 10) from the button's bottom left, mana 3 under it, the frame art 64 x 16 at the
    // bottom left.
    CHECK(at.health(0).x == doctest::Approx(111.5f));
    CHECK(at.health(0).bottom() == doctest::Approx(at.button(0).bottom() - 10.0f));
    CHECK(at.mana(0).y == doctest::Approx(at.health(0).bottom() + 3.0f));
    CHECK(at.barFrame(0).bottom() == doctest::Approx(at.button(0).bottom()));
    // The name over the button (52 x 12 at (2, 3)), the auras right of it: 6 off its right edge, 1 down, 2
    // x 2.
    CHECK(at.name(0).y == doctest::Approx(at.button(0).y - 3.0f));
    CHECK(at.aura(0, 0).x == doctest::Approx(at.button(0).right() + 6.0f));
    CHECK(at.aura(0, 0).y == doctest::Approx(at.button(0).y + 1.0f));
    CHECK(at.aura(0, 1).x == doctest::Approx(at.aura(0, 0).right() + 1.0f));
    CHECK(at.aura(0, 3).y == doctest::Approx(at.aura(0, 0).bottom() + 1.0f));
    // The rows tile the list: a tap between two buttons belongs to one of them.
    CHECK(at.row(1).y == doctest::Approx(at.row(0).bottom()));
    // The hit rectangle reaches the auras and the title.
    const Rect hit = at.hit();
    CHECK(hit.y == doctest::Approx(at.title().y));
    CHECK(hit.right() >= at.aura(4, 3).right());
    CHECK(hit.bottom() == doctest::Approx(at.frame().bottom()));
}

TEST_CASE("Raid pullout size: the party frames' scale, smaller for a list taller than the screen") {
    const f32 base = 1.5f; // logical pixels per interface unit
    CHECK(raidPulloutUnit(base, 5, 1000.0f) == doctest::Approx(base * kRaidPulloutScale));
    // 15 rows: 13 + 15 * 33 + 14 = 522 units: at 2.4 px a unit 1253 px, more than 1000.
    const f32 shrunk = raidPulloutUnit(base, 15, 1000.0f);
    CHECK(shrunk < base * kRaidPulloutScale);
    CHECK((13.0f + 15.0f * 33.0f + 14.0f) * shrunk == doctest::Approx(1000.0f));
    // Never under the client's own size: the last rows go off the screen instead.
    CHECK(raidPulloutUnit(base, 40, 400.0f) == doctest::Approx(base));
}

TEST_CASE("Raid class colors: RAID_CLASS_COLORS of 1.12") {
    CHECK(game::raidClassColor(1) == ui::rgba(199, 156, 110)); // warrior
    CHECK(game::raidClassColor(7) == game::raidClassColor(2));       // the shaman wears the paladin's pink in 1.12
    CHECK(game::raidClassColor(5) == ui::rgba(255, 255, 255)); // priest
    CHECK(game::raidClassColor(11) == ui::rgba(255, 125, 10)); // druid
    CHECK(game::raidClassColor(0) == 0);
    CHECK(game::raidClassColor(6) == 0); // no class 6 in 1.12
    CHECK(raidPowerColor(0) == ui::rgba(0, 0, 255));
    CHECK(raidPowerColor(3) == ui::rgba(255, 255, 0));
    CHECK(raidPowerColor(9) == ui::rgba(0, 255, 255)); // past the table: its last colour
}

TEST_CASE("Raid pullout placement: on the screen, off the stick and the action buttons") {
    const Rect safe{0.0f, 0.0f, 1000.0f, 600.0f};
    const Rect cluster{700.0f, 400.0f, 300.0f, 200.0f}; // the action buttons in the bottom right corner
    const Rect stick{0.0f, 450.0f, 500.0f, 150.0f};     // the stick's band at the bottom left
    const std::array<Rect, 2> avoid{cluster, stick};

    // Off the right edge: ValidateFramePosition / clampedToScreen bring it back.
    Rect r = raidPulloutPlace({950.0f, 100.0f, 120.0f, 200.0f}, safe, {});
    CHECK(inside(r, safe));
    CHECK(r.y == doctest::Approx(100.0f));

    // Dropped on the action buttons: moved up over them (the shortest way out that stays on the screen).
    r = raidPulloutPlace({750.0f, 380.0f, 120.0f, 150.0f}, safe, avoid);
    CHECK(inside(r, safe));
    CHECK_FALSE(overlaps(r, cluster));
    CHECK(r.bottom() == doctest::Approx(cluster.y));
    CHECK(r.x == doctest::Approx(750.0f));

    // Into the stick's band from the left: up as well.
    r = raidPulloutPlace({40.0f, 420.0f, 120.0f, 100.0f}, safe, avoid);
    CHECK_FALSE(overlaps(r, stick));
    CHECK(inside(r, safe));

    // A wide screen's stick column along the left edge: moved right of it.
    const Rect column{0.0f, 80.0f, 150.0f, 520.0f};
    r = raidPulloutPlace({20.0f, 150.0f, 120.0f, 300.0f}, safe, std::array<Rect, 1>{column});
    CHECK_FALSE(overlaps(r, column));
    CHECK(r.x == doctest::Approx(150.0f));

    // Dropped where the stick's band meets the action buttons: off both (a move off one must not land on the other).
    const Rect band{0.0f, 450.0f, 500.0f, 150.0f};
    const Rect buttons{500.0f, 400.0f, 500.0f, 200.0f};
    r = raidPulloutPlace({450.0f, 380.0f, 120.0f, 200.0f}, safe, std::array<Rect, 2>{band, buttons});
    CHECK(inside(r, safe));
    CHECK_FALSE(overlaps(r, band));
    CHECK_FALSE(overlaps(r, buttons));
    CHECK(r.x == doctest::Approx(450.0f)); // straight up: the nearest place
    CHECK(r.bottom() == doctest::Approx(buttons.y));

    // Taller than the screen's free part: kept on the screen even if it must cover something.
    r = raidPulloutPlace({300.0f, 0.0f, 120.0f, 590.0f}, safe, avoid);
    CHECK(inside(r, safe));

    // Larger than the screen itself: its top left stays visible.
    r = raidPulloutPlace({-50.0f, -40.0f, 120.0f, 900.0f}, safe, {});
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
}
