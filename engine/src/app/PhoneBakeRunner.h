#pragma once

#include "app/PhoneBakePlan.h"
#include "platform/BakeLauncher.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (phonebake, ROADMAP 22.7): runs a bake plan step by step, as the Android service does
// (apps/android/.../BakeService.java follows this file rule by rule: keep them alike). For each step from the
// last finished one: wait while the phone is too hot, empty the staging folder (its .cache stays: textures'
// resume), run `vd_baker <bake...> --jobs N` with its output as the progress, stop it and start it again with
// fewer threads when the phone heats (a step with a resume cache only), then `vd_baker <install...>`;
// status.json after every change and every 2 s; every line of the baker into bake.log. On the desktop it is
// the platform's runner (DesktopBakeLauncher: vd_baker.exe on a thread of its own), so the first-run screens
// can be tried with an empty packs folder; the tests drive it with a fake baker.
namespace vd {

struct PhoneBakeRunnerHooks {
    // Runs vd_baker with `args` to its end: every output line to `onLine`; `tick` about every second, which
    // returns true to stop the program. The exit code, or -1 when it could not start or was stopped.
    std::function<i32(const std::vector<std::string>& args,
                      const std::function<void(std::string_view)>& onLine, const std::function<bool()>& tick)>
        runBaker;
    // The thermal reading: headroom (NaN: unknown) and status (-1: unknown). Optional.
    std::function<void(f32& headroom, i32& status)> thermal;
    std::function<i64()> nowMs; // wall clock, ms since 1970
    // Waits while the phone cools down; tests pass one that returns at once. Optional.
    std::function<void(u32 ms)> sleep;
    const std::atomic<bool>* cancel = nullptr;
    u32 heartbeatMs = 2000;
    u32 thermalMs = 15000;
    u32 coolingMs = 30000;
};

// Runs (or goes on with) the plan in `places.bakeDir`; returns how it ended (Done, Failed, Cancelled; None
// when there is no readable plan).
PhoneBakeState runPhoneBakePlan(const PhoneBakePlaces& places, const PhoneBakeRunnerHooks& hooks);

// The milliseconds since 1970 (the service's System.currentTimeMillis()).
i64 phoneBakeNowMs();

#if defined(_WIN32)
// The desktop's runner: vd_baker.exe (`baker`) as a child process, on a thread of its own.
std::unique_ptr<BakeLauncher> makeDesktopBakeLauncher(std::filesystem::path baker);
// vd_baker.exe next to the program or in the development build's layout
// (build/<preset>/tools/baker/<config>/); VD_BAKER overrides it. Empty when there is none.
std::filesystem::path findDesktopBaker();
#endif

} // namespace vd
