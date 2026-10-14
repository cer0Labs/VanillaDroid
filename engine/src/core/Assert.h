#pragma once

namespace vd::detail {
[[noreturn]] void assertFailed(const char* expression, const char* file, int line);
} // namespace vd::detail

// VD_VERIFY is always active; VD_ASSERT compiles out in release builds (NDEBUG).
#define VD_VERIFY(cond)                                                                                  \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            ::vd::detail::assertFailed(#cond, __FILE__, __LINE__);                                       \
        }                                                                                                \
    } while (0)

#if defined(NDEBUG)
#define VD_ASSERT(cond) ((void)0)
#else
#define VD_ASSERT(cond) VD_VERIFY(cond)
#endif
