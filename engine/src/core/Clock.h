#pragma once

#include "core/Types.h"

#include <chrono>

namespace vd {

using Clock = std::chrono::steady_clock;

inline f64 secondsBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<f64>(to - from).count();
}

} // namespace vd
