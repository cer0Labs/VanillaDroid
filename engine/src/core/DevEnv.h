#pragma once

#include <cstdlib>

// Phase 22 (distribution, ADR-0005 §9): the VD_* development switches (environment variables; on Android the launch
// intent's extras, apps/android/native/main_android.cpp). The distribution build (VD_DIST) has none: VD_DEV_ENV is
// always null there, so neither the switches' names nor the code they turn on reach the published binary (the
// optimizer drops the branches, the linker the functions only they reached).
#if defined(VD_DIST)
#define VD_DEV_ENV(name) (static_cast<const char*>(nullptr))
#else
#define VD_DEV_ENV(name) (std::getenv(name))
#endif
