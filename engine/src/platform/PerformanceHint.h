#pragma once

#include "core/Types.h"

#include <span>

namespace vd {

// Android ADPF performance hint session (API 33+): tells the scheduler how long the game thread's work took
// against its per-frame target, so the thread runs on (and clocks up) the cores it needs instead of idling
// on the little ones. Without it the game thread was measured on the Cortex-A55 cores at 1.8 GHz while the
// big cores sat at ~800 MHz. Where Android offers no session (before 13, or a device without the power
// HAL support, like the Xiaomi Mi 11X) the thread is pinned off the slowest cluster instead. A no-op on
// other platforms.
class PerformanceHint {
public:
    PerformanceHint() = default;
    ~PerformanceHint();
    PerformanceHint(const PerformanceHint&) = delete;
    PerformanceHint& operator=(const PerformanceHint&) = delete;

    // Opens the session for the calling thread. Returns false where ADPF is unavailable.
    bool startForCurrentThread(i64 targetNanos);
    // The same for the calling thread plus `helpers` (the job workers: they do part of the frame's work, and the
    // scheduler otherwise parks those short bursts on the little cores). Without ADPF only the calling thread is
    // pinned off the slowest cluster.
    bool startForThreads(std::span<const i32> helpers, i64 targetNanos);
    // Work time of the frame just finished (without the waits on the GPU/display).
    void reportActual(i64 nanos);
    // Phase 10: the same with the GPU's time of the frame (Android 15+: APerformanceHint_reportActualWorkDuration2,
    // so the system can also clock the GPU for the target); the CPU's alone where that API is missing.
    void reportActual(i64 cpuNanos, i64 gpuNanos);
    // Phase 10: a new target per frame (the 30 fps limit doubles it).
    void updateTarget(i64 targetNanos);
    // Phase 10 (Android 15+): rather save energy than finish early (the battery saver). False where unsupported.
    bool setPreferPowerEfficiency(bool enabled);
    void stop();
    bool active() const { return m_session != nullptr; }
    bool reportsGpu() const; // reportActual(cpu, gpu) passes the GPU's time on

private:
    void* m_session = nullptr; // APerformanceHintSession*
    void* m_work = nullptr;    // AWorkDuration*, reused every frame
    i64 m_target = 0;
};

} // namespace vd
