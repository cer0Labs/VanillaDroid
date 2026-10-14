// Single translation unit holding the VMA implementation. volk.h must come first so VMA sees
// VK_NO_PROTOTYPES and exposes vmaImportVulkanFunctionsFromVolk().
#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
