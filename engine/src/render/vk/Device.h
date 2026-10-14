#pragma once

#include "core/Types.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <string>

namespace vd::vk {

struct DeviceOptions {
    bool enableValidation = false;
    // VkPipelineCache file: loaded at creation (if it matches this GPU/driver), saved on destroy().
    std::string pipelineCachePath;
};

// Instance, physical device, logical device, graphics+present queue and VMA allocator.
// Created once per process and kept alive across surface loss (fold/unfold, background).
class Device {
public:
    Result<void> create(const DeviceOptions& options);
    void destroy();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    u32 queueFamily() const { return m_queueFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    VkPipelineCache pipelineCache() const { return m_pipelineCache; }
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    u32 timestampValidBits() const { return m_timestampValidBits; }
    // ASTC LDR sampling (all mobile GPUs; most desktop GPUs lack it -> CPU decode fallback).
    bool astcSupported() const { return m_astcSupported; }
    bool anisotropySupported() const { return m_anisotropySupported; }
    // Phase 18 (world): the largest anisotropy a sampler may ask for (1 without the feature), and the multisample counts
    // a color + depth framebuffer may have (VkSampleCountFlags).
    f32 maxAnisotropy() const { return m_anisotropySupported ? m_properties.limits.maxSamplerAnisotropy : 1.0f; }
    VkSampleCountFlags framebufferSampleCounts() const {
        return m_properties.limits.framebufferColorSampleCounts & m_properties.limits.framebufferDepthSampleCounts;
    }
    // The samples of the main (and scene) render pass: what createGraphicsPipeline() builds for unless told otherwise.
    // Set by the Renderer before it creates its passes and pipelines.
    VkSampleCountFlagBits mainPassSamples() const { return m_mainPassSamples; }
    void setMainPassSamples(VkSampleCountFlagBits samples) { m_mainPassSamples = samples; }

    // GPU-only buffers and images created while playing (terrain tiles, models, textures) come from pools
    // whose blocks are reserved at start-up: a new VMA block mid-game took up to 62 ms on Adreno 650 (a single
    // long frame). Falls back to VMA's default path when the pool can't serve the request (too large, another
    // memory type); the pools grow like the default ones once the reserve is used up.
    VkResult createStreamedBuffer(const VkBufferCreateInfo& info, VkBuffer* buffer, VmaAllocation* allocation) const;
    VkResult createStreamedImage(const VkImageCreateInfo& info, VkImage* image, VmaAllocation* allocation) const;

private:
    Result<void> createInstance(const DeviceOptions& options);
    Result<void> pickPhysicalDevice();
    Result<void> createLogicalDevice();
    void createPipelineCache();
    void savePipelineCache() const;
    void createStreamPools();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    u32 m_queueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VmaPool m_bufferPool = VK_NULL_HANDLE; // may be the same pool as m_imagePool
    VmaPool m_imagePool = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    std::string m_pipelineCachePath;
    VkPhysicalDeviceProperties m_properties{};
    u32 m_timestampValidBits = 0;
    bool m_astcSupported = false;
    bool m_anisotropySupported = false;
    bool m_debugUtils = false;
    VkSampleCountFlagBits m_mainPassSamples = VK_SAMPLE_COUNT_1_BIT;
};

} // namespace vd::vk
