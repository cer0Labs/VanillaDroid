#pragma once

#include "core/Types.h"
#include "pack/ParticleFormat.h"

#include <algorithm>
#include <cmath>

namespace vd::fx {

// The 1.12 particle's look over its life (t = age / lifespan, 0..1): three points, at birth, at `mid` and at
// death.
template <typename T> T lifeRamp(f32 t, f32 mid, const T& start, const T& middle, const T& end) {
    if (t <= mid) {
        return mid > 0.0f ? start + (middle - start) * (t / mid) : middle;
    }
    return mid < 1.0f ? middle + (end - middle) * ((t - mid) / (1.0f - mid)) : middle;
}

// Texture cell of a particle's head at `t` of its life: in each half (before and after midPoint), from the
// half's start cell towards its end cell, `repeat` times over the half.
inline u32 headCell(const pack::ParticleEmitter& d, f32 t, u32 cellCount) {
    const bool first = t < d.midPoint;
    const u16* cells = first ? d.headCells : d.headCells + 3;
    const f32 f = first ? (d.midPoint > 0.0f ? t / d.midPoint : 0.0f)
                        : (d.midPoint < 1.0f ? (t - d.midPoint) / (1.0f - d.midPoint) : 0.0f);
    const f32 cycles = f * static_cast<f32>(std::max<u16>(cells[2], 1));
    const f32 k = cycles - std::floor(cycles);
    const i32 span = static_cast<i32>(cells[1]) - static_cast<i32>(cells[0]);
    const i32 cell = static_cast<i32>(cells[0]) + static_cast<i32>(std::floor(static_cast<f32>(span) * k));
    return static_cast<u32>(std::clamp(cell, 0, static_cast<i32>(cellCount) - 1));
}

// The same for the tail (no repeat).
inline u32 tailCell(const pack::ParticleEmitter& d, f32 t, u32 cellCount) {
    const bool first = t < d.midPoint;
    const i16* cells = first ? d.tailCells : d.tailCells + 2;
    const f32 f = first ? (d.midPoint > 0.0f ? t / d.midPoint : 0.0f)
                        : (d.midPoint < 1.0f ? (t - d.midPoint) / (1.0f - d.midPoint) : 0.0f);
    const i32 cell = cells[0] + static_cast<i32>(std::floor(static_cast<f32>(cells[1] - cells[0]) * f));
    return static_cast<u32>(std::clamp(cell, 0, static_cast<i32>(cellCount) - 1));
}

} // namespace vd::fx
