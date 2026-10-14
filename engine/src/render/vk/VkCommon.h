#pragma once

// Include this instead of <vulkan/vulkan.h>: volk provides the function pointers.
#include <volk.h>

#include <vk_mem_alloc.h>

#include "core/Result.h"

namespace vd::vk {

const char* resultToString(VkResult result);

inline Error makeError(const char* what, VkResult result) {
    return Error{std::string(what) + " failed: " + resultToString(result)};
}

} // namespace vd::vk

// Returns a vd::Error from the enclosing function when a Vulkan call does not return VK_SUCCESS.
#define VD_VK_TRY(call)                                                                                  \
    do {                                                                                                 \
        const VkResult vdVkResult_ = (call);                                                             \
        if (vdVkResult_ != VK_SUCCESS) {                                                                 \
            return ::vd::vk::makeError(#call, vdVkResult_);                                              \
        }                                                                                                \
    } while (0)
