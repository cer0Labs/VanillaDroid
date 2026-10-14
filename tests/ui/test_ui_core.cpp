#include "ui/Context.h"
#include "ui/DrawList.h"

#include <doctest/doctest.h>

using namespace vd;

namespace {

const ui::Rect kScreen{0.0f, 0.0f, 1000.0f, 500.0f};

void frame(ui::Context& ui, u64 nowMs) {
    ui.begin(kScreen.w, kScreen.h, 1.0f, kScreen, nowMs);
}

} // namespace

TEST_CASE("ui context: a finger belongs to the topmost widget under it for its whole life") {
    ui::Context ui;
    const ui::WidgetId below = ui::widgetId("below");
    const ui::WidgetId above = ui::widgetId("above");
    const ui::Rect big{100, 100, 200, 200};
    const ui::Rect small{150, 150, 50, 50};

    frame(ui, 0); // the widgets exist before the finger lands
    ui.widget(below, big);
    ui.widget(above, small);
    ui.end();

    ui.touchEvent(TouchPhase::Down, 7, 160.0f, 160.0f);
    frame(ui, 16);
    CHECK_FALSE(ui.widget(below, big).pressed);
    const ui::WidgetState pressed = ui.widget(above, small);
    CHECK(pressed.pressed);
    REQUIRE(pressed.touch);
    CHECK(pressed.touch->began);
    ui.end();

    // Dragging out of it keeps it on the same widget, and releasing far away is not a tap.
    ui.touchEvent(TouchPhase::Move, 7, 400.0f, 400.0f);
    ui.touchEvent(TouchPhase::Up, 7, 400.0f, 400.0f);
    frame(ui, 32);
    ui.widget(below, big);
    const ui::WidgetState released = ui.widget(above, small);
    CHECK(released.released);
    CHECK_FALSE(released.clicked);
    ui.end();

    frame(ui, 48);
    CHECK(ui.touches().empty()); // gone the frame after it lifted
    ui.end();
}

TEST_CASE("ui context: taps, long presses and fingers on the world") {
    ui::Context ui;
    const ui::WidgetId button = ui::widgetId("button");
    const ui::Rect r{10, 10, 100, 100};
    frame(ui, 0);
    ui.widget(button, r);
    ui.end();

    // A tap: down and up within the same frame still clicks.
    ui.touchEvent(TouchPhase::Down, 1, 50.0f, 50.0f);
    ui.touchEvent(TouchPhase::Up, 1, 52.0f, 51.0f);
    frame(ui, 16);
    CHECK(ui.widget(button, r).clicked);
    ui.end();

    // A long press fires once and then is not a click.
    ui.touchEvent(TouchPhase::Down, 2, 50.0f, 50.0f);
    frame(ui, 100);
    CHECK_FALSE(ui.widget(button, r).longPress);
    ui.end();
    frame(ui, 100 + ui::Context::kLongPressMs + 1);
    CHECK(ui.widget(button, r).longPress);
    ui.end();
    frame(ui, 100 + ui::Context::kLongPressMs + 20);
    CHECK_FALSE(ui.widget(button, r).longPress);
    ui.end();
    ui.touchEvent(TouchPhase::Up, 2, 50.0f, 50.0f);
    frame(ui, 1000);
    CHECK_FALSE(ui.widget(button, r).clicked);
    ui.end();

    // A finger elsewhere is the world's (owner 0), with its motion per frame.
    ui.touchEvent(TouchPhase::Down, 3, 600.0f, 300.0f);
    frame(ui, 1016);
    ui.widget(button, r);
    REQUIRE(ui.touches().size() == 1);
    CHECK(ui.touches()[0].owner == 0);
    ui.end();
    ui.touchEvent(TouchPhase::Move, 3, 630.0f, 290.0f);
    frame(ui, 1032);
    ui.widget(button, r);
    CHECK(ui.touches()[0].dx == doctest::Approx(30.0f));
    CHECK(ui.touches()[0].dy == doctest::Approx(-10.0f));
    CHECK(ui.touches()[0].moved);
    ui.end();
}

TEST_CASE("ui draw list: quads share draw commands until the clip or the texture changes") {
    ui::DrawList draw;
    draw.init(nullptr, 64);
    draw.solid({0, 0, 10, 10}, ui::kWhite);
    draw.icon({0, 0, 10, 10}, 3);
    draw.texture({0, 0, 10, 10}, TextureHandle{5});
    draw.solid({0, 0, 10, 10}, ui::kWhite); // untextured quads join whatever texture the command has
    REQUIRE(draw.commands().size() == 1);
    CHECK(draw.commands()[0].quadCount == 4);
    CHECK(draw.commands()[0].texture.index == 5);
    draw.texture({0, 0, 10, 10}, TextureHandle{6}); // another texture: a new command
    CHECK(draw.commands().size() == 2);
    draw.pushClip({5, 5, 20, 20});
    draw.solid({0, 0, 10, 10}, ui::kWhite);
    draw.popClip();
    draw.solid({0, 0, 10, 10}, ui::kWhite);
    REQUIRE(draw.commands().size() == 4);
    CHECK(draw.commands()[2].clipWidth == doctest::Approx(20.0f));
    CHECK(draw.commands()[3].clipWidth == 0.0f);
    CHECK(draw.quadCount() == 7);
    // The icon quad carries its slot and mode.
    CHECK((draw.vertices()[4].params & 4095u) == 3u);
    CHECK(((draw.vertices()[4].params >> 12) & 15u) == static_cast<u32>(UiMode::Icon));
    // Full: further quads are dropped, never overflowing.
    for (u32 i = 0; i < 100; ++i) {
        draw.solid({0, 0, 1, 1}, ui::kWhite);
    }
    CHECK(draw.quadCount() == 64);
    CHECK(draw.full());
}

TEST_CASE("ui text: UTF-8 decoding") {
    const std::string_view text = "a\xC3\xB1" "\xE2\x82\xAC" "\xF0\x9F\x98\x80" "\xC3"; // a ñ € 😀 + truncated
    usize i = 0;
    CHECK(ui::decodeUtf8(text, i) == 'a');
    CHECK(ui::decodeUtf8(text, i) == 0xF1);
    CHECK(ui::decodeUtf8(text, i) == 0x20AC);
    CHECK(ui::decodeUtf8(text, i) == 0x1F600);
    CHECK(ui::decodeUtf8(text, i) == 0xFFFD);
    CHECK(i == text.size());
}
