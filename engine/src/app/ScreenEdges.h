#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <algorithm>
#include <array>

namespace vd {

// The screen beyond the safe area (logical pixels, the window's coordinates). The safe area keeps everything clear of
// the display cutout along its whole side; a part lying along an edge where no cutout reaches (the chat and the
// experience bar at the bottom, while a phone's camera hole is in the middle of its left side in landscape) can use the
// screen up to the system bars and curved edges instead. Without cutouts it is the safe area.
struct ScreenEdges {
    static constexpr u32 kMaxCutouts = 4;
    ui::Rect screen;                  // the whole window
    f32 insets[4] = {};               // left, top, right, bottom: the system bars' and curved edges', not the cutout's
    ui::Rect safe;                    // the safe area (with the cutout's insets): the limit when nothing else is known
    std::array<ui::Rect, kMaxCutouts> cutouts{};
    u32 cutoutCount = 0;
    bool known = false; // the platform said where its cutouts are (else left() and right() keep to the safe area)

    // The left limit for something spanning [top, bottom] vertically: the screen's edge plus its inset, past the
    // cutouts on the left half that reach into that span.
    f32 left(f32 top, f32 bottom) const {
        if (!known) {
            return safe.x;
        }
        f32 x = screen.x + insets[0];
        for (u32 i = 0; i < cutoutCount; ++i) {
            const ui::Rect& c = cutouts[i];
            if (c.centerX() < screen.centerX() && c.y < bottom && c.bottom() > top) {
                x = std::max(x, c.right());
            }
        }
        return x;
    }
    // The right limit, the same way.
    f32 right(f32 top, f32 bottom) const {
        if (!known) {
            return safe.right();
        }
        f32 x = screen.right() - insets[2];
        for (u32 i = 0; i < cutoutCount; ++i) {
            const ui::Rect& c = cutouts[i];
            if (c.centerX() >= screen.centerX() && c.y < bottom && c.bottom() > top) {
                x = std::min(x, c.x);
            }
        }
        return x;
    }
};

} // namespace vd
