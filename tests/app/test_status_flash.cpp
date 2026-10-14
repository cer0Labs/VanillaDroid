// Phase 13 (combat): the full-screen status flashes (app/StatusFlash) with UIParent.lua 1.12's rules:
// PlayerStatus_OnUpdate's branches and UIFrameFlash's (0.5 s in, 0.5 s out, 100 s; a call on a frame already
// flashing does nothing). Phase 20 (retouches): the stops work as PlayerStatus_OnUpdate means them (in the 1.12 they
// were ignored and a flash ran its 100 s), the low health one above 20 %.
#include "app/StatusFlash.h"

#include <doctest/doctest.h>

namespace {

using namespace vd;

// `seconds` of 30 fps frames.
void run(StatusFlash& s, f32 seconds, f32 health, bool dead, bool lost) {
    for (f32 t = 0.0f; t < seconds; t += 1.0f / 30.0f) {
        s.update(1.0f / 30.0f, health, dead, lost);
    }
}

// The brightest the low health (or out of control) flash gets over the next second.
f32 brightest(StatusFlash& s, f32 health, bool lost, bool outOfControl) {
    f32 most = 0.0f;
    for (u32 i = 0; i < 30; ++i) {
        s.update(1.0f / 30.0f, health, false, lost);
        const f32 a = outOfControl ? s.outOfControlAlpha() : s.lowHealthAlpha();
        most = most > a ? most : a;
    }
    return most;
}

} // namespace

TEST_CASE("Out of control: OutOfControlFrame pulses while control is lost and goes when it comes back") {
    StatusFlash s;
    run(s, 1.0f, 1.0f, false, false);
    CHECK(s.outOfControlAlpha() == 0.0f);
    s.update(0.25f, 1.0f, false, true); // PLAYER_CONTROL_LOST: UIFrameFlash(OutOfControlFrame, 0.5, 0.5, 100)
    CHECK(s.outOfControlAlpha() == doctest::Approx(0.5f));
    s.update(0.25f, 1.0f, false, true);
    CHECK(s.outOfControlAlpha() == doctest::Approx(1.0f));
    s.update(0.25f, 1.0f, false, true);
    CHECK(s.outOfControlAlpha() == doctest::Approx(0.5f)); // fading out
    // PLAYER_CONTROL_GAINED: hidden at once (UIFrameFlash(OutOfControlFrame, 0.5, 0.5, 0) as meant).
    s.update(1.0f / 30.0f, 1.0f, false, false);
    CHECK(s.outOfControlAlpha() == 0.0f);
    CHECK(brightest(s, 1.0f, false, true) == 0.0f);
    // The next loss starts it again.
    s.update(0.25f, 1.0f, false, true);
    CHECK(s.outOfControlAlpha() == doctest::Approx(0.5f));
    // A loss longer than the flash's 100 s: it starts over.
    run(s, 101.0f, 1.0f, false, true);
    CHECK(brightest(s, 1.0f, true, true) > 0.9f);
}

TEST_CASE("Low health: LowHealthFrame from 20 %, gone once the health is back over it") {
    StatusFlash s;
    s.update(0.25f, 0.5f, false, false);
    CHECK(s.lowHealthAlpha() == 0.0f);
    s.update(0.25f, 0.2f, false, false);
    CHECK(s.lowHealthAlpha() == doctest::Approx(0.5f));
    // It keeps pulsing between 10 and 20 % (the 1.12's stop above 10 % would end it the next frame).
    run(s, 2.0f, 0.15f, false, false);
    CHECK(brightest(s, 0.15f, false, false) > 0.9f);
    // A potion: hidden at once, not 100 s later.
    s.update(1.0f / 30.0f, 0.6f, false, false);
    CHECK(s.lowHealthAlpha() == 0.0f);
    CHECK(brightest(s, 0.6f, false, false) == 0.0f);
    // Low again, then dead: it stops.
    s.update(0.25f, 0.1f, false, false);
    CHECK(s.lowHealthAlpha() == doctest::Approx(0.5f));
    s.update(1.0f / 30.0f, 0.0f, true, false);
    CHECK(s.lowHealthAlpha() == 0.0f);
}

TEST_CASE("Low health goes first: out of control waits a frame for it") {
    StatusFlash s;
    s.update(0.25f, 0.2f, false, true); // both: the elseif chain starts the low health flash only
    CHECK(s.lowHealthAlpha() == doctest::Approx(0.5f));
    CHECK(s.outOfControlAlpha() == 0.0f);
    s.update(0.25f, 0.2f, false, true);
    CHECK(s.lowHealthAlpha() == doctest::Approx(1.0f)); // both pulse, each on its own clock
    CHECK(s.outOfControlAlpha() == doctest::Approx(0.5f));
    // Without the full-screen status option, neither starts.
    StatusFlash off;
    off.update(0.25f, 0.1f, false, true, false);
    off.update(0.25f, 0.1f, false, true, false);
    CHECK(off.lowHealthAlpha() == 0.0f);
    CHECK(off.outOfControlAlpha() == 0.0f);
}
