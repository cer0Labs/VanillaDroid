#include "platform/PerformanceHint.h"

#include "core/Log.h"
#include "platform/Thread.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <time.h>

#include <algorithm>
#include <cstdio>
#endif

namespace vd {

#if defined(__ANDROID__)
namespace {

// Looked up at run time: minSdk is 30 and the API appeared in 33 (android/performance_hint.h).
struct HintApi {
    void* (*getManager)() = nullptr;
    void* (*createSession)(void* manager, const i32* tids, usize count, i64 targetNanos) = nullptr;
    int (*reportActual)(void* session, i64 actualNanos) = nullptr;
    void (*closeSession)(void* session) = nullptr;
    int (*updateTarget)(void* session, i64 targetNanos) = nullptr; // API 33
    // API 35: work durations with the GPU's share, and the power efficiency preference.
    void* (*createWork)() = nullptr;
    void (*releaseWork)(void* work) = nullptr;
    void (*setWorkStart)(void* work, i64 nanos) = nullptr;
    void (*setWorkTotal)(void* work, i64 nanos) = nullptr;
    void (*setWorkCpu)(void* work, i64 nanos) = nullptr;
    void (*setWorkGpu)(void* work, i64 nanos) = nullptr;
    int (*reportWork)(void* session, void* work) = nullptr;
    int (*setPreferPowerEfficiency)(void* session, bool enabled) = nullptr;
    bool loaded = false;
    bool work = false;
};

const HintApi& hintApi() {
    static const HintApi api = [] {
        HintApi a;
        void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return a;
        }
        a.getManager = reinterpret_cast<decltype(a.getManager)>(dlsym(library, "APerformanceHint_getManager"));
        a.createSession =
            reinterpret_cast<decltype(a.createSession)>(dlsym(library, "APerformanceHint_createSession"));
        a.reportActual = reinterpret_cast<decltype(a.reportActual)>(
            dlsym(library, "APerformanceHint_reportActualWorkDuration"));
        a.closeSession = reinterpret_cast<decltype(a.closeSession)>(dlsym(library, "APerformanceHint_closeSession"));
        a.updateTarget = reinterpret_cast<decltype(a.updateTarget)>(
            dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
        a.createWork = reinterpret_cast<decltype(a.createWork)>(dlsym(library, "AWorkDuration_create"));
        a.releaseWork = reinterpret_cast<decltype(a.releaseWork)>(dlsym(library, "AWorkDuration_release"));
        a.setWorkStart = reinterpret_cast<decltype(a.setWorkStart)>(
            dlsym(library, "AWorkDuration_setWorkPeriodStartTimestampNanos"));
        a.setWorkTotal = reinterpret_cast<decltype(a.setWorkTotal)>(
            dlsym(library, "AWorkDuration_setActualTotalDurationNanos"));
        a.setWorkCpu = reinterpret_cast<decltype(a.setWorkCpu)>(
            dlsym(library, "AWorkDuration_setActualCpuDurationNanos"));
        a.setWorkGpu = reinterpret_cast<decltype(a.setWorkGpu)>(
            dlsym(library, "AWorkDuration_setActualGpuDurationNanos"));
        a.reportWork = reinterpret_cast<decltype(a.reportWork)>(
            dlsym(library, "APerformanceHint_reportActualWorkDuration2"));
        a.setPreferPowerEfficiency = reinterpret_cast<decltype(a.setPreferPowerEfficiency)>(
            dlsym(library, "APerformanceHint_setPreferPowerEfficiency"));
        a.loaded = a.getManager && a.createSession && a.reportActual && a.closeSession;
        a.work = a.createWork && a.releaseWork && a.setWorkStart && a.setWorkTotal && a.setWorkCpu && a.setWorkGpu &&
                 a.reportWork;
        return a;
    }();
    return api;
}

} // namespace

bool PerformanceHint::startForCurrentThread(i64 targetNanos) {
    return startForThreads({}, targetNanos);
}

bool PerformanceHint::startForThreads(std::span<const i32> helpers, i64 targetNanos) {
    stop();
    const HintApi& api = hintApi();
    void* manager = api.loaded ? api.getManager() : nullptr;
    i32 tids[32];
    u32 count = 0;
    tids[count++] = currentThreadId();
    for (i32 tid : helpers) {
        if (tid != 0 && count < 32) {
            tids[count++] = tid;
        }
    }
    m_session = manager ? api.createSession(manager, tids, count, targetNanos) : nullptr;
    if (!m_session) {
        // Fallback: only the game thread goes off the slowest cluster (the scheduler otherwise parks a thread that
        // is busy ~35% of the time on the little cores). Pinning the workers there too was measured worse on the
        // Mi 11X: seven threads on four cores, and waking them took the game thread's core for ~2 ms a frame.
        if (const u32 cores = pinThreadToFastCores(tids[0]); cores > 0) {
            VD_LOG_WARN("ADPF performance hint unavailable: thread %d pinned to the %u non-little cores", tids[0], cores);
        } else {
            VD_LOG_WARN("ADPF performance hint unavailable: threads left to the scheduler");
        }
        return false;
    }
    m_target = targetNanos;
    if (api.work) {
        m_work = api.createWork();
    }
    VD_LOG_INFO("ADPF performance hint: %u threads, target %.2f ms%s", count, static_cast<f64>(targetNanos) / 1.0e6,
                m_work ? ", CPU + GPU durations" : "");
    return true;
}

void PerformanceHint::reportActual(i64 nanos) {
    if (m_session && nanos > 0) {
        hintApi().reportActual(m_session, nanos);
    }
}

void PerformanceHint::reportActual(i64 cpuNanos, i64 gpuNanos) {
    if (!m_session || cpuNanos <= 0) {
        return;
    }
    const HintApi& api = hintApi();
    if (!m_work || gpuNanos <= 0) {
        api.reportActual(m_session, cpuNanos);
        return;
    }
    // The frame's period started cpuNanos ago (its work ends now); the total is what the longer of the two took.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const i64 nowNanos = static_cast<i64>(now.tv_sec) * 1000000000 + now.tv_nsec;
    api.setWorkStart(m_work, nowNanos - cpuNanos);
    api.setWorkTotal(m_work, std::max(cpuNanos, gpuNanos));
    api.setWorkCpu(m_work, cpuNanos);
    api.setWorkGpu(m_work, gpuNanos);
    api.reportWork(m_session, m_work);
}

void PerformanceHint::updateTarget(i64 targetNanos) {
    if (m_session && targetNanos > 0 && targetNanos != m_target && hintApi().updateTarget) {
        hintApi().updateTarget(m_session, targetNanos);
        m_target = targetNanos;
        VD_LOG_INFO("ADPF target %.2f ms", static_cast<f64>(targetNanos) / 1.0e6);
    }
}

bool PerformanceHint::setPreferPowerEfficiency(bool enabled) {
    if (m_session && hintApi().setPreferPowerEfficiency) {
        return hintApi().setPreferPowerEfficiency(m_session, enabled) == 0;
    }
    return false;
}

bool PerformanceHint::reportsGpu() const {
    return m_work != nullptr;
}

void PerformanceHint::stop() {
    if (m_work) {
        hintApi().releaseWork(m_work);
        m_work = nullptr;
    }
    if (m_session) {
        hintApi().closeSession(m_session);
        m_session = nullptr;
    }
}

#else

bool PerformanceHint::startForCurrentThread(i64) {
    return false;
}

bool PerformanceHint::startForThreads(std::span<const i32>, i64) {
    return false;
}

void PerformanceHint::reportActual(i64) {}

void PerformanceHint::reportActual(i64, i64) {}

void PerformanceHint::updateTarget(i64) {}

bool PerformanceHint::setPreferPowerEfficiency(bool) {
    return false;
}

bool PerformanceHint::reportsGpu() const {
    return false;
}

void PerformanceHint::stop() {}

#endif

PerformanceHint::~PerformanceHint() {
    stop();
}

} // namespace vd
