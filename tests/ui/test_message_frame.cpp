// Parity (messages): ui::MessageFrame as UIErrorsFrame.xml configures the client's MessageFrame: three lines,
// the newest on top, each shown 5 s and then fading out over 3, stamped the first time it is updated.
#include "ui/MessageFrame.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <string>
#include <string_view>

namespace {

using namespace vd;

std::string_view textOf(const ui::MessageFrame::Shown& s) {
    return s.line->text.data();
}

} // namespace

TEST_CASE("MessageFrame: UIErrorsFrame's shape (three lines, newest on top, 5 s and a 3 s fade)") {
    ui::MessageFrame frame;
    std::array<ui::MessageFrame::Shown, 4> shown;
    CHECK(frame.visible(0, shown) == 0);

    frame.add("Out of range.", 1);
    frame.add("You are too far away!", 2);
    frame.update(1000);
    REQUIRE(frame.visible(1000, shown) == 2);
    CHECK(textOf(shown[0]) == "You are too far away!"); // insertMode TOP: the newest first
    CHECK(shown[0].line->color == 2);
    CHECK(textOf(shown[1]) == "Out of range.");
    CHECK(shown[0].alpha == 1.0f);

    // Two more: only the three newest fit.
    frame.add("Kobold Vermin slain: 3/10", 3);
    std::span<char> out = frame.add(4, true);
    out[0] = 'X';
    out[1] = 0;
    frame.update(2000);
    REQUIRE(frame.visible(2000, shown) == 3);
    CHECK(textOf(shown[0]) == "X");
    CHECK(shown[0].line->big);
    CHECK(textOf(shown[2]) == "You are too far away!");

    // Shown 5 s from their first update, then fading out over 3.
    CHECK(frame.visible(6000, shown) == 3);
    CHECK(shown[2].alpha == 1.0f);
    REQUIRE(frame.visible(7500, shown) == 3);
    CHECK(shown[2].alpha == doctest::Approx(0.5f));        // stamped at 1000: 1.5 s into its fade
    CHECK(shown[0].alpha == doctest::Approx(2.5f / 3.0f)); // stamped at 2000: 0.5 s into it
    // The first two are gone.
    frame.update(9000);
    REQUIRE(frame.visible(9000, shown) == 2);
    CHECK(textOf(shown[1]) == "Kobold Vermin slain: 3/10");
    frame.update(10000);
    CHECK(frame.visible(10000, shown) == 0);

    // Lines never updated keep showing (a gallery, a frame not drawn yet); empty ones are not lines.
    frame.add("Kobold Camp Cleanup (Complete)", 5);
    frame.add("", 6);
    REQUIRE(frame.visible(50000, shown) == 1);
    CHECK(shown[0].alpha == 1.0f);
    frame.clear();
    CHECK(frame.visible(50000, shown) == 0);

    // Long lines are cut to the buffer, NUL-terminated; the ring wraps.
    const std::string longText(400, 'a');
    for (u32 i = 0; i < 20; ++i) {
        frame.add(longText, i);
    }
    REQUIRE(frame.visible(50000, shown) == 3);
    CHECK(textOf(shown[0]).size() == ui::MessageFrame::kTextSize - 1);
    CHECK(shown[0].line->color == 19);
    CHECK(shown[2].line->color == 17);
}

TEST_CASE("MessageFrame: insertMode BOTTOM lists the oldest first") {
    ui::MessageFrame frame({2, 1000, 500, false});
    frame.add("a", 1);
    frame.add("b", 2);
    frame.add("c", 3);
    std::array<ui::MessageFrame::Shown, 4> shown;
    REQUIRE(frame.visible(0, shown) == 2);
    CHECK(textOf(shown[0]) == "b");
    CHECK(textOf(shown[1]) == "c");
}
