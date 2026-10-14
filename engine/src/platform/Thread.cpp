#include "platform/Thread.h"

#include "core/Profile.h"

#include <cwchar>

#if defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace vd {

void setCurrentThreadName(const char* name) {
#if defined(__ANDROID__)
    pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
    wchar_t wide[32];
    std::swprintf(wide, 32, L"%hs", name);
    SetThreadDescription(GetCurrentThread(), wide);
#endif
    VD_PROFILE_THREAD_NAME(name);
}

#if defined(__ANDROID__)

i32 currentThreadId() {
    return static_cast<i32>(gettid());
}

u32 pinThreadToFastCores(i32 tid) {
    // The cores whose maximum frequency beats the slowest cluster's.
    constexpr u32 kMaxCores = 16;
    u32 frequency[kMaxCores] = {};
    u32 count = 0;
    u32 slowest = ~0u;
    for (; count < kMaxCores; ++count) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", count);
        FILE* file = std::fopen(path, "r");
        if (!file) {
            break;
        }
        if (std::fscanf(file, "%u", &frequency[count]) != 1) {
            frequency[count] = 0;
        }
        std::fclose(file);
        slowest = std::min(slowest, frequency[count]);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    u32 allowed = 0;
    for (u32 core = 0; core < count; ++core) {
        if (frequency[core] > slowest) {
            CPU_SET(core, &set);
            ++allowed;
        }
    }
    if (allowed == 0 || sched_setaffinity(tid, sizeof(set), &set) != 0) {
        return 0;
    }
    return allowed;
}

#else

i32 currentThreadId() {
    return 0;
}

u32 pinThreadToFastCores(i32) {
    return 0;
}

#endif

} // namespace vd
