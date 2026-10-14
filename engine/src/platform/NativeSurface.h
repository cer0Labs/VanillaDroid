#pragma once

#include "core/Types.h"

namespace vd {

// OS window handed to the renderer to create a VkSurfaceKHR.
//   Android: window = ANativeWindow*, instance = nullptr
//   Win32:   window = HWND,           instance = HINSTANCE
struct NativeSurface {
    void* window = nullptr;
    void* instance = nullptr;
    u32 width = 0;
    u32 height = 0;
};

} // namespace vd
