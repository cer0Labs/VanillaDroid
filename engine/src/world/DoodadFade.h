#pragma once

#include "core/Types.h"

#include <algorithm>

namespace vd {

// Phase 14 (14.10a): how far 1.12 draws an M2 doodad and how it fades in, from WoW.exe 1.12.1 (the pass over
// the visible doodads at 0x683F80, every frame after culling, terrain and WMO doodads alike). `distance` is
// the horizontal one from the camera to the doodad's bounding sphere centre minus its radius (the model
// file's radius times the placement's scale); the radius picks one of three ranges, each ending in a linear
// fade:
//   radius <= 0.5 yd: drawn up to 50 yd, fading over the last 10;
//   radius <= 2.5 yd: up to 125 yd, over the last 25;
//   radius <= 7 yd:   up to 200 yd (or the far clip, if nearer), over the last 50;
//   larger:           always opaque, only the far clip and the fog end them.
// The opacity goes to the model (CM2Model::SetAlpha) and an opacity of 0 leaves it out of the scene. 1.12 has
// no fade in time: a model that finishes loading inside its range appears at once. `scale` stretches the
// ranges (the quality settings' environment detail times the dynamic quality's distance factor; 1 = 1.12).
struct DoodadRange {
    f32 end = 0.0f;  // yards; <= 0: no range of its own (the far clip)
    f32 band = 0.0f; // the fade over the last `band` yards
};

inline DoodadRange doodadRange(f32 radius, f32 scale, f32 farClip) {
    DoodadRange r;
    if (radius <= 0.5f) {
        r = {50.0f, 10.0f};
    } else if (radius <= 2.5f) {
        r = {125.0f, 25.0f};
    } else if (radius <= 7.0f) {
        r = {200.0f, 50.0f};
    } else {
        return r;
    }
    r.end *= scale;
    r.band *= scale;
    if (r.end > farClip) { // the client: min(200, far clip) for the largest range (the others never reach it)
        r.end = farClip;
    }
    return r;
}

// The opacity at `distance` (see above): 1 short of the band, 0 beyond the range (not drawn).
inline f32 doodadFade(const DoodadRange& range, f32 distance) {
    if (range.end <= 0.0f) {
        return 1.0f;
    }
    if (range.band <= 0.0f) {
        return distance <= range.end ? 1.0f : 0.0f;
    }
    return std::clamp(1.0f - (distance - (range.end - range.band)) / range.band, 0.0f, 1.0f);
}

} // namespace vd
