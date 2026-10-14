#pragma once

#include "core/Types.h"

namespace vd {

struct PlatformInfo;

// Phase 20 (assists, docs/MOBILE_CONTROLS.md §4 / §8 "Vibración"): short vibrations for the touch controls. Android:
// the system's Vibrator with VibrationEffect (API 26+; predefined effects API 29+, minSdk is 30; the manifest asks for
// android.permission.VIBRATE, a normal permission granted at install), called from a thread of its own ("vd-haptics")
// so the game thread never waits on a binder call. Elsewhere nothing vibrates; VD_TRACE_HAPTICS=1 writes each one to
// the log.
namespace haptics {

enum class Pattern : u8 {
    Tap,    // a spell cast from the cluster (EFFECT_TICK: light)
    Double, // a reactive ability appeared (EFFECT_DOUBLE_CLICK)
    Long,   // a refusal (a 160 ms buzz)
};

// The platform's vibrator (Android: its thread starts; elsewhere only the trace). Once, before the first play().
void start(const PlatformInfo& platform);
// Its thread stops (at exit).
void stop();
// From any thread, never blocks: played as soon as the vibrator's thread wakes (a newer pattern replaces one not
// played yet).
void play(Pattern pattern);

} // namespace haptics
} // namespace vd
