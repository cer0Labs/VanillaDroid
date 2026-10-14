#pragma once

// CPU profiling zones. Backed by Tracy when the build enables VD_ENABLE_TRACY (Android `profile`
// variant); otherwise they compile to nothing. Desktop Tracy needs Windows SDK >= 10.0.22000
// (Tracy 0.14 uses Windows 11 APIs); this PC has 10.0.19041, so it is Android-only for now.
//
//   void update() {
//       VD_PROFILE_ZONE("World::update");
//       ...
//   }

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#define VD_PROFILE_ZONE(name) ZoneScopedN(name)
#define VD_PROFILE_FRAME() FrameMark
#define VD_PROFILE_THREAD_NAME(name) ::tracy::SetThreadName(name)
#else
#define VD_PROFILE_ZONE(name) static_cast<void>(0)
#define VD_PROFILE_FRAME() static_cast<void>(0)
#define VD_PROFILE_THREAD_NAME(name) static_cast<void>(0)
#endif
