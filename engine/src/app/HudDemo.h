#pragma once

#include "app/Hud.h"

#include <array>

namespace vd {

// VD_HUD_DEMO's action bars (parity pass, development): the twelve buttons of the main bar, the bottom left bar and
// a warrior's stance bar showing every state a button has (cooldown, short of power, unusable, out of range, the
// auto attack on, a queued swing, an equipped item, counts), to look at the layout offline.
struct HudDemo {
    std::array<HudActionSlot, 12> actions{};
    std::array<HudActionSlot, 12> extraActions{};
    std::array<HudActionSlot, 3> stances{};
    // Phase 15 (options): the other three extra bars, empty (VD_MULTIBARS and Always Show ActionBars show their grids).
    std::array<std::array<HudActionSlot, 12>, 3> moreBars{};
};
const HudDemo& hudDemo();

} // namespace vd
