#include "core/Log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

// Logcat on Android, but for a command-line program there (phase 22: the baker on a phone, VD_LOG_STDERR).
#if defined(__ANDROID__) && !defined(VD_LOG_STDERR)
#define VD_LOG_LOGCAT 1
#endif

#if defined(VD_LOG_LOGCAT)
#include <android/log.h>
#endif

namespace vd::log {
namespace {

#if defined(VD_LOG_LOGCAT)
constexpr const char* kTag = "VanillaDroid";

int toAndroidPriority(Level level) {
    switch (level) {
    case Level::Debug: return ANDROID_LOG_DEBUG;
    case Level::Info: return ANDROID_LOG_INFO;
    case Level::Warn: return ANDROID_LOG_WARN;
    case Level::Error: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_INFO;
}
#else
const char* levelName(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "INFO";
}
#endif

std::atomic<u8> g_minimumLevel{0}; // phase 22 (setup)

} // namespace

void setMinimumLevel(Level level) {
    g_minimumLevel.store(static_cast<u8>(level), std::memory_order_relaxed);
}

void write(Level level, const char* fmt, ...) {
    if (static_cast<u8>(level) < g_minimumLevel.load(std::memory_order_relaxed)) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

#if defined(VD_LOG_LOGCAT)
    __android_log_write(toAndroidPriority(level), kTag, buffer);
#else
    // stderr: keeps stdout clean for tool output (e.g. `vd_baker list > files.txt`) and is unbuffered,
    // so nothing is lost if the process is killed or crashes.
    std::fprintf(stderr, "[%s] %s\n", levelName(level), buffer);
#endif
}

} // namespace vd::log
