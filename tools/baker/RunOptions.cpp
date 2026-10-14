// Phase 22 (22.7, ADR-0005 §8): the options every command takes and what a run cost, to bake on a phone.
#include "BakerCommon.h"

#include "core/Log.h"
#include "jobs/JobSystem.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
// After windows.h.
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace vd::baker {
namespace {

// --cache: the folder that takes the place of <out>/.cache (empty: that one), or no cache at all.
std::filesystem::path g_cacheRoot;
bool g_cacheEnabled = true;

struct ProcessCost {
    f64 cpuSeconds = 0.0; // user + system, every thread
    u64 peakBytes = 0;    // peak resident memory (Windows: peak working set)
};

ProcessCost processCost() {
    ProcessCost cost;
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& t) { return (u64{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
        cost.cpuSeconds = static_cast<f64>(ticks(kernel) + ticks(user)) * 1e-7; // 100 ns units
    }
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        cost.peakBytes = memory.PeakWorkingSetSize;
    }
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const auto seconds = [](const timeval& t) { return static_cast<f64>(t.tv_sec) + t.tv_usec * 1e-6; };
        cost.cpuSeconds = seconds(usage.ru_utime) + seconds(usage.ru_stime);
#if defined(__APPLE__)
        cost.peakBytes = static_cast<u64>(usage.ru_maxrss); // bytes on macOS
#else
        cost.peakBytes = static_cast<u64>(usage.ru_maxrss) * 1024; // KiB on Linux and Android
#endif
    }
#endif
    return cost;
}

} // namespace

bool applyRunOptions(const Options& options) {
    if (const std::string jobs = option(options, "jobs"); !jobs.empty()) {
        char* end = nullptr;
        const unsigned long threads = std::strtoul(jobs.c_str(), &end, 10);
        if (*end != '\0' || threads == 0 || threads > 64) {
            VD_LOG_ERROR("--jobs takes the threads that bake, 1 to 64 (not '%s')", jobs.c_str());
            return false;
        }
        JobSystem::setDefaultWorkerCount(static_cast<i32>(threads) - 1); // the calling thread works too
    }
    if (const std::string cache = option(options, "cache"); cache == "off") {
        g_cacheEnabled = false;
    } else if (!cache.empty()) {
        g_cacheRoot = cache;
    }
    return true;
}

std::filesystem::path bakeCacheDir(const std::filesystem::path& dir) {
    if (!g_cacheEnabled) {
        return {};
    }
    return g_cacheRoot.empty() ? dir : g_cacheRoot / dir.filename();
}

void logRunCost(const char* what, Clock::time_point start) {
    const f64 seconds = secondsBetween(start, Clock::now());
    const ProcessCost cost = processCost();
    VD_LOG_INFO("%s: %.1f s, CPU %.1f s (%.1f threads busy), peak memory %.0f MB", what, seconds,
                cost.cpuSeconds, seconds > 0.0 ? cost.cpuSeconds / seconds : 0.0,
                static_cast<f64>(cost.peakBytes) / 1e6);
}

} // namespace vd::baker
