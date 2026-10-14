#pragma once

#include "core/Types.h"

#if defined(__clang__) || defined(__GNUC__)
#define VD_PRINTF_FORMAT(fmtIndex, argsIndex) __attribute__((format(printf, fmtIndex, argsIndex)))
#else
#define VD_PRINTF_FORMAT(fmtIndex, argsIndex)
#endif

namespace vd::log {

enum class Level : u8 { Debug, Info, Warn, Error };

// printf-style logging. Goes to logcat (tag "VanillaDroid") on Android, stdout/stderr elsewhere.
void write(Level level, const char* fmt, ...) VD_PRINTF_FORMAT(2, 3);

// Phase 22 (setup): lines below `level` are dropped (VanillaDroid Setup's console shows the player its own texts and
// keeps these developer lines for --verbose). Everything is written by default.
void setMinimumLevel(Level level);

} // namespace vd::log

#define VD_LOG_DEBUG(...) ::vd::log::write(::vd::log::Level::Debug, __VA_ARGS__)
#define VD_LOG_INFO(...) ::vd::log::write(::vd::log::Level::Info, __VA_ARGS__)
#define VD_LOG_WARN(...) ::vd::log::write(::vd::log::Level::Warn, __VA_ARGS__)
#define VD_LOG_ERROR(...) ::vd::log::write(::vd::log::Level::Error, __VA_ARGS__)
