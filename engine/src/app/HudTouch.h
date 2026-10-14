#pragma once

// Phase 20: shared by the touch cluster's translation units (app/HudTouchBar.cpp, app/HudTouchEdit.cpp).

#include "app/Hud.h"
#include "game/TouchLayout.h"

namespace vd {

// Where the touch cluster is this frame: Main's center and the logical pixels of one millimetre of its geometry (its
// scale and the fit to the screen included).
struct Hud::TouchFrame {
    const TouchBarView* view = nullptr;
    const game::TouchGeometry* geometry = nullptr;
    f32 mainX = 0.0f;
    f32 mainY = 0.0f;
    f32 scale = 1.0f;
    bool mirrored = false;

    const game::TouchPlacement& place(u32 index) const { return geometry->places[index]; }
    f32 x(u32 index) const { return mainX + (mirrored ? -place(index).x : place(index).x) * scale; }
    f32 y(u32 index) const { return mainY - place(index).y * scale; }
    f32 radius(u32 index) const { return place(index).size * 0.5f * scale; }
    // A screen point in the geometry's millimetres (relative to Main's center, y up): the edit mode's moves.
    f32 toMmX(f32 sx) const { return (mirrored ? mainX - sx : sx - mainX) / scale; }
    f32 toMmY(f32 sy) const { return (mainY - sy) / scale; }
};

} // namespace vd
