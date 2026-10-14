#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace vd::audio {

// 3D positioning of the client's sounds (SoundEntries: MinDistance, DistanceCutoff). The original client drives
// FMOD 3 with SoundRolloffFactor 4 (phase 14, WoW.exe: the CVar's default "4" goes to FSOUND_3D_SetRolloffFactor):
// the DirectSound3D rolloff, full volume within the minimum distance and min / (min + 4 (d - min)) beyond, and
// sounds past the cutoff are not heard; here the volume also fades to 0 over the last quarter before the cutoff,
// so a sound walking out of range never stops abruptly. Its listener is the player's character (WoW.exe
// 0x483118: at the unit's head, facing where it faces), the camera only without one.
constexpr f32 kRolloffFactor = 4.0f;

// Volume (0..1) of a sound `distance` yards away.
inline f32 distanceGain(f32 distance, f32 minDistance, f32 maxDistance, f32 rolloff = kRolloffFactor) {
    minDistance = std::max(minDistance, 0.1f);
    maxDistance = std::max(maxDistance, minDistance);
    if (distance >= maxDistance) {
        return 0.0f;
    }
    f32 gain = distance <= minDistance ? 1.0f : minDistance / (minDistance + rolloff * (distance - minDistance));
    const f32 fadeStart = maxDistance - 0.25f * (maxDistance - minDistance);
    if (distance > fadeStart) {
        gain *= (maxDistance - distance) / (maxDistance - fadeStart);
    }
    return gain;
}

// Left / right gains for a pan in [-1 (left), 1 (right)]: equal power, scaled so that the center is full volume in
// both channels (a centered positional sound is as loud as a 2D one).
struct StereoGains {
    f32 left = 1.0f;
    f32 right = 1.0f;
};
inline StereoGains panGains(f32 pan) {
    const f32 angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.78539816f; // 0..pi/2
    constexpr f32 kSqrt2 = 1.41421356f;
    return {std::min(1.0f, std::cos(angle) * kSqrt2), std::min(1.0f, std::sin(angle) * kSqrt2)};
}

// Pan of a sound from the listener: `side` = dot(direction to the sound, the listener's right vector) over the
// distance (the sine of its bearing). Within `nearDistance` yards it moves towards the center: a sound on top of
// the listener (the player's own footsteps) is heard from both sides.
inline f32 panFor(f32 side, f32 distance, f32 nearDistance = 2.0f) {
    if (distance <= 1e-3f) {
        return 0.0f;
    }
    const f32 pan = std::clamp(side / distance, -1.0f, 1.0f);
    return pan * std::min(1.0f, distance / nearDistance);
}

} // namespace vd::audio
