// Phase 20 (retouches): the loading screen as WoW.exe 1.12 draws it (LoadingScreen.cpp): the art in a 4:3 rectangle,
// Loading-BarBorder and Loading-BarFill at their fractions of it (0x7FFD34), the tip's block over the bar (0x406640),
// and GameTips.dbc's lines in order through the gameTip CVar (0x46B5F0).
#include "app/Dialogs.h"
#include "app/LoadingLayout.h"
#include "app/SettingsFile.h"
#include "game/AreaData.h"

#include <doctest/doctest.h>

namespace {

using namespace vd;

void checkRect(const ui::Rect& r, f32 x, f32 y, f32 w, f32 h) {
    CHECK(r.x == doctest::Approx(x));
    CHECK(r.y == doctest::Approx(y));
    CHECK(r.w == doctest::Approx(w));
    CHECK(r.h == doctest::Approx(h));
}

} // namespace

TEST_CASE("Loading screen: the 1.12's bar and tip on its own 1024 x 768 screen") {
    const ui::Rect screen{0.0f, 0.0f, 1024.0f, 768.0f};
    const LoadingLayout l = loadingLayout(screen, screen, 0.0f);
    checkRect(l.art, 0.0f, 0.0f, 1024.0f, 768.0f);
    // The border 0.6 x 0.05 centred 0.075 over the bottom; the fill 0.525 x 0.025 inside it, same centre.
    checkRect(l.border, 204.8f, 691.2f, 614.4f, 38.4f);
    checkRect(l.fill, 243.2f, 700.8f, 537.6f, 19.2f);
    CHECK(l.fill.centerY() == doctest::Approx(l.border.centerY()));
    // The tip: 515 of the 1024 units wide, centred, ending on the border's top; a 13.8 unit font.
    CHECK(l.tip.w == doctest::Approx(515.0f));
    CHECK(l.tip.centerX() == doctest::Approx(512.0f));
    CHECK(l.tip.bottom() == doctest::Approx(l.border.y));
    CHECK(l.tipSize == doctest::Approx(13.824f));
    CHECK(l.tipShadow == doctest::Approx(1.0f));
}

TEST_CASE("Loading screen: black bars where the screen is not 4:3, the safe area lifts the bar") {
    // The Fold5's cover screen (wide): black sides.
    const ui::Rect cover{0.0f, 0.0f, 2316.0f, 904.0f};
    LoadingLayout l = loadingLayout(cover, cover, 0.0f);
    CHECK(l.art.h == doctest::Approx(904.0f));
    CHECK(l.art.w == doctest::Approx(904.0f * 4.0f / 3.0f));
    CHECK(l.art.centerX() == doctest::Approx(1158.0f));
    CHECK(l.border.centerX() == doctest::Approx(1158.0f));
    // Its inner screen (taller than 4:3): black above and below.
    const ui::Rect inner{0.0f, 0.0f, 2176.0f, 1812.0f};
    l = loadingLayout(inner, inner, 0.0f);
    checkRect(l.art, 0.0f, 90.0f, 2176.0f, 1632.0f);
    CHECK(l.border.bottom() == doctest::Approx(90.0f + 1632.0f * 0.95f));
    // A system bar over the bottom: the bar and the tip go up out of it.
    const ui::Rect screen{0.0f, 0.0f, 1024.0f, 768.0f};
    l = loadingLayout(screen, {0.0f, 0.0f, 1024.0f, 700.0f}, 0.0f);
    CHECK(l.border.bottom() == doctest::Approx(700.0f));
    CHECK(l.fill.centerY() == doctest::Approx(l.border.centerY()));
    CHECK(l.tip.bottom() == doctest::Approx(l.border.y));
}

TEST_CASE("Loading screen: a legible font on a short screen widens the tip as much") {
    const ui::Rect screen{0.0f, 0.0f, 1158.0f, 452.0f};
    const LoadingLayout small = loadingLayout(screen, screen, 0.0f);
    const LoadingLayout l = loadingLayout(screen, screen, small.tipSize * 2.0f);
    CHECK(l.tipSize == doctest::Approx(small.tipSize * 2.0f));
    CHECK(l.tip.w == doctest::Approx(small.tip.w * 2.0f)); // the same line breaks
    CHECK(l.tip.centerX() == doctest::Approx(579.0f));
    // Never wider than the safe area.
    const LoadingLayout huge = loadingLayout(screen, screen, small.tipSize * 10.0f);
    CHECK(huge.tip.w == doctest::Approx(1158.0f));
    CHECK(huge.tip.x == doctest::Approx(0.0f));
}

TEST_CASE("Loading screen tips: GameTips.dbc's rows in order, as the gameTip CVar keeps them") {
    game::AreaData areas;
    u32 next = 0;
    CHECK(areas.gameTip(next).empty()); // no tips: none, the counter stays
    CHECK(next == 0);
    areas.setGameTips({"first", "second", "third"});
    CHECK(areas.gameTip(next) == "first");
    CHECK(next == 1);
    CHECK(areas.gameTip(next) == "second");
    CHECK(areas.gameTip(next) == "third");
    CHECK(next == 3);
    CHECK(areas.gameTip(next) == "first"); // past the last: the first again
    CHECK(next == 1);
    next = 40; // a counter from a longer table
    CHECK(areas.gameTip(next) == "first");
    CHECK(next == 1);
}

TEST_CASE("Loading screen tips: the option and the counter go to the settings file") {
    SettingsView settings;
    CHECK(settings.showGameTips); // UIOptionsFrame's showGameTips "1"
    CHECK(settings.gameTip == 0); // gameTip "0"
    settings.showGameTips = false;
    settings.gameTip = 17;
    SettingsView read;
    parseSettings(formatSettings(settings), read);
    CHECK_FALSE(read.showGameTips);
    CHECK(read.gameTip == 17);
}
