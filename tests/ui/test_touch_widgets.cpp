// Phase 20 (docs/MOBILE_CONTROLS.md §3.5): the touch cluster's round widgets. A finger inside a circle is its; one
// in the corner of its square is not; one that lands between round buttons (on no widget) goes to the nearest whose
// margin reaches; the ring and round icon quads carry their parameters for the shader.
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <doctest/doctest.h>

using namespace vd;

namespace {

const ui::Rect kScreen{0.0f, 0.0f, 1000.0f, 500.0f};

void frame(ui::Context& ui, u64 nowMs) {
    ui.begin(kScreen.w, kScreen.h, 1.0f, kScreen, nowMs);
}

// Two buttons 100 px apart, 40 px in radius, 12 px of margin; a finger down at (x, y): who owns it.
ui::WidgetId owner(f32 x, f32 y) {
    ui::Context ui;
    const ui::WidgetId a = ui::widgetId("a"), b = ui::widgetId("b");
    frame(ui, 0);
    ui.widgetCircle(a, 300.0f, 200.0f, 40.0f, 12.0f);
    ui.widgetCircle(b, 400.0f, 200.0f, 40.0f, 12.0f);
    ui.end();
    ui.touchEvent(TouchPhase::Down, 1, x, y);
    frame(ui, 16);
    const bool onA = ui.widgetCircle(a, 300.0f, 200.0f, 40.0f, 12.0f).pressed;
    const bool onB = ui.widgetCircle(b, 400.0f, 200.0f, 40.0f, 12.0f).pressed;
    ui.end();
    return onA ? a : onB ? b : 0;
}

} // namespace

TEST_CASE("touch widgets: a circle takes the fingers inside it, the nearest takes those between") {
    CHECK(owner(300.0f, 200.0f) == ui::widgetId("a"));
    CHECK(owner(330.0f, 180.0f) == ui::widgetId("a"));
    // The square's corner is outside the circle, and farther than the margin: the world's.
    CHECK(owner(268.0f, 168.0f) == ui::widgetId("a")); // 45 px from the center: within 40 + 12
    CHECK(owner(258.0f, 158.0f) == 0);                 // 59 px: nobody's
    // Between the two: the nearer one.
    CHECK(owner(346.0f, 200.0f) == ui::widgetId("a"));
    CHECK(owner(354.0f, 200.0f) == ui::widgetId("b"));
}

TEST_CASE("touch widgets: a rectangle above a circle keeps its fingers") {
    ui::Context ui;
    const ui::WidgetId round = ui::widgetId("round"), panel = ui::widgetId("panel");
    frame(ui, 0);
    ui.widgetCircle(round, 300.0f, 200.0f, 40.0f, 12.0f);
    ui.widget(panel, {280.0f, 180.0f, 100.0f, 100.0f});
    ui.end();
    ui.touchEvent(TouchPhase::Down, 1, 300.0f, 200.0f);
    frame(ui, 16);
    CHECK_FALSE(ui.widgetCircle(round, 300.0f, 200.0f, 40.0f, 12.0f).pressed);
    CHECK(ui.widget(panel, {280.0f, 180.0f, 100.0f, 100.0f}).pressed);
    ui.end();
}

TEST_CASE("touch widgets: the ring and round icon quads") {
    ui::DrawList draw;
    draw.init(nullptr, 16);
    draw.reset();
    draw.ring(100.0f, 50.0f, 20.0f, 0.5f, ui::kWhite, 0.25f, true);
    draw.iconCircle({0.0f, 0.0f, 64.0f, 64.0f}, 3, ui::kWhite);
    draw.ring(10.0f, 10.0f, 5.0f, 0.0f, ui::kWhite, 0.0f); // nothing left of the turn: no quad
    REQUIRE(draw.quadCount() == 2);
    const std::span<const UiVertex> v = draw.vertices();
    const u32 ring = v[0].params;
    CHECK(((ring >> 12) & 15u) == static_cast<u32>(UiMode::Ring));
    CHECK(((ring >> 16) & 255u) == 64u);  // a quarter of the turn
    CHECK(((ring >> 25) & 127u) == 64u);  // a hole of half the radius
    CHECK(((ring >> 24) & 1u) == 1u);     // additive
    CHECK(v[0].x == doctest::Approx(80.0f));
    CHECK(v[2].y == doctest::Approx(70.0f));
    const u32 icon = v[4].params;
    CHECK(((icon >> 12) & 15u) == static_cast<u32>(UiMode::IconCircle));
    CHECK((icon & 4095u) == 3u);
    CHECK(((icon >> 16) & 255u) == 214u); // the 0.42 radius of a 0.08 crop
    CHECK(v[4].u == doctest::Approx(0.08f));
}
