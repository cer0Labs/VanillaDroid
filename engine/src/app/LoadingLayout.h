#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

namespace vd {

// Phase 20 (retouches): the loading screen as WoW.exe 1.12 lays it out (LoadingScreen.cpp), as a pure function (unit
// tested). Everything lives in the largest 4:3 rectangle centred on the screen, black around it (its viewport at
// 0x406A60), in fractions of that rectangle measured from its bottom left: the bar's two quads at 0x7FFD34 (centre x,
// centre y, width, height) and the tip's block at 0x406640.
struct LoadingLayout {
    ui::Rect art;    // LoadingScreens.dbc's art, 4:3
    ui::Rect border; // Loading-BarBorder: 0.6 x 0.05, centred 0.075 above the bottom
    ui::Rect fill;   // Loading-BarFill when full: 0.525 x 0.025 with the same centre; it grows from its left edge
    // GameTips.dbc's line: a block 515 / 1024 of the width wide, centred, left-aligned text whose last line ends on
    // its bottom edge (the border's top: the tip grows upwards).
    ui::Rect tip;
    f32 tipSize = 0.0f;   // the tip's font, 0.018 of the height
    f32 tipShadow = 0.0f; // its black shadow, 0.001 of the height down and right
};

// `safe`: the part of the screen nothing covers; the bar and the tip go up out of what is under it. `minTipSize`: the
// smallest legible font (a short screen); a larger font widens the tip's block by as much, so its lines break where
// the 1.12's do.
LoadingLayout loadingLayout(const ui::Rect& screen, const ui::Rect& safe, f32 minTipSize);

} // namespace vd
