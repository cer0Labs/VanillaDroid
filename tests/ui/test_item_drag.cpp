// Phase 8: holding an item picks it up; the finger then drags the item, never the scroll view under it.
#include "ui/Context.h"
#include "ui/Widgets.h"

#include <doctest/doctest.h>

using namespace vd;
using namespace vd::ui;

TEST_CASE("Scroll view: a finger held until its long press is a drag of what it holds, not a scroll") {
    ScrollState s;
    s.content = 1000.0f;
    const Rect viewport{0.0f, 100.0f, 300.0f, 200.0f};
    Touch t;
    t.id = 3;
    t.startX = 50.0f;
    t.startY = 150.0f;
    t.x = 50.0f;
    t.y = 150.0f;
    t.downMs = 1000;
    t.longPressed = true; // Context marks it once held still for kLongPressMs
    // Then it moves up and down a long way: the view stays put and never takes it.
    for (int i = 0; i < 6; ++i) {
        t.y -= 40.0f;
        t.dy = -40.0f;
        scrollStep(s, {&t, 1}, viewport, 2.0f, 1500 + 16 * i);
    }
    CHECK(s.touch == -1);
    CHECK(s.offset == 0.0f);
    CHECK_FALSE(scrollClaimed(&t));
}

TEST_CASE("Context: a held finger long-presses its widget once and then never taps it") {
    Context ui;
    const Rect cell{10.0f, 10.0f, 40.0f, 40.0f};
    const WidgetId id = widgetId("cell");
    ui.begin(400.0f, 300.0f, 1.0f, {0.0f, 0.0f, 400.0f, 300.0f}, 1000);
    ui.widget(id, cell);
    ui.end();
    ui.touchEvent(TouchPhase::Down, 7, 20.0f, 20.0f);
    ui.begin(400.0f, 300.0f, 1.0f, {0.0f, 0.0f, 400.0f, 300.0f}, 1016);
    CHECK_FALSE(ui.widget(id, cell).longPress);
    ui.end();
    ui.begin(400.0f, 300.0f, 1.0f, {0.0f, 0.0f, 400.0f, 300.0f}, 1016 + Context::kLongPressMs);
    const WidgetState held = ui.widget(id, cell);
    CHECK(held.longPress);
    REQUIRE(held.touch);
    CHECK(held.touch->longPressed);
    ui.end();
    // Dragged away and released over another place: no tap (the drop belongs to the item drag).
    ui.touchEvent(TouchPhase::Move, 7, 200.0f, 200.0f);
    ui.touchEvent(TouchPhase::Up, 7, 200.0f, 200.0f);
    ui.begin(400.0f, 300.0f, 1.0f, {0.0f, 0.0f, 400.0f, 300.0f}, 2000);
    const WidgetState up = ui.widget(id, cell);
    CHECK(up.released);
    CHECK_FALSE(up.clicked);
    ui.end();
}
