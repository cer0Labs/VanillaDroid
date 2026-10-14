#pragma once

#include "core/Types.h"

namespace vd::pack {

// Phase 14: the cameras of the cinematics (Cameras\*.m2: each race's intro flyby, the Palantir of Azora, the scrying
// orb...), in cinematics.vdpk under their original paths (".m2"). A camera flies along its position track looking at
// its target track, in the model's space; CinematicCamera.dbc places it in the world: origin + the point rotated by
// the row's facing about +Z (the flybys end right behind the race's starting spot, looking north).
//
// Layout: CinematicCameraHeader, then its keys (16-byte aligned).

constexpr u32 kCinematicFormatVersion = 1;

// How a track goes from one key to the next (the M2 interpolation types).
enum class CinematicInterpolation : u32 { Step = 0, Linear = 1, Bezier = 2, Hermite = 3 };

// A key of a track: its value and the two control points around it (bezier: absolute handles; hermite: tangents).
// Scalar tracks (roll) use x only.
struct CinematicKey {
    u32 time; // ms from the camera's start
    f32 value[3];
    f32 in[3];
    f32 out[3];
};
static_assert(sizeof(CinematicKey) == 40, "CinematicKey layout");

struct CinematicTrack {
    u32 interpolation; // CinematicInterpolation
    u32 firstKey;      // into the keys
    u32 keyCount;      // 0: the base value (or 0) throughout
};
static_assert(sizeof(CinematicTrack) == 12, "CinematicTrack layout");

struct CinematicCameraHeader {
    u32 version;    // kCinematicFormatVersion
    u32 durationMs; // the flight: its sequence's length
    f32 fov;        // diagonal field of view, radians (as the portraits' cameras)
    f32 nearClip;
    f32 farClip;
    f32 positionBase[3]; // added to the position track
    f32 targetBase[3];   // added to the target track
    CinematicTrack position;
    CinematicTrack target;
    CinematicTrack roll; // radians about the view direction
    u32 keyCount;
    u32 keysOffset;
    u32 reserved[2];
};
static_assert(sizeof(CinematicCameraHeader) == 96, "CinematicCameraHeader layout");

} // namespace vd::pack
