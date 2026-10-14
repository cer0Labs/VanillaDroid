#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>
#include <vector>

namespace vd {

namespace vk {
class Device;
class Uploader;
} // namespace vk

// Distant horizon (internal to render/): the map's WDL heights in one storage buffer and a per-frame
// instance list, drawn with a single instanced draw in the fog color.
class HorizonRenderer {
public:
    static constexpr u32 kMaxInstances = 1024;
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    Result<void> createPipeline(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipeline(const vk::Device& device);

    // Map data, copied through the upload ring into the next frame. heights: i16 pairs as u32 words. Phase 18 (world):
    // the data it replaces is retired, never freed while a frame in flight may draw it.
    Result<void> setData(const vk::Device& device, vk::Uploader& uploader, std::span<const u32> heights,
                         std::span<const u16> indices);
    // Phase 18 (world): the map's data goes to the retired list, freed by collect() once submittedFrames >= retireFrame.
    void retire(u64 retireFrame);
    void collect(const vk::Device& device, u64 submittedFrames, bool everything);
    // Everything at once (the GPU must be idle): shutdown.
    void clear(const vk::Device& device);

    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 frameSlot, std::span<const HorizonDraw> draws,
              const vk::Device& device, RenderStats& stats);
    u64 gpuBytes() const { return m_gpuBytes; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
    };

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFrames> m_sets{};
    std::array<Buffer, kMaxFrames> m_instances{};
    u32 m_frames = 0;
    Buffer m_heights;
    Buffer m_indices;
    u32 m_indexCount = 0;
    u64 m_gpuBytes = 0;
    // Phase 18 (world): maps' data waiting for the frames that may still draw it.
    struct Retired {
        Buffer heights;
        Buffer indices;
        std::array<VkDescriptorSet, kMaxFrames> sets{};
        u64 retireFrame = 0;
    };
    static constexpr u32 kMaxRetired = 2; // the descriptor pool holds the current sets and these
    static constexpr u64 kRetireFrames = 3;
    std::vector<Retired> m_retired;
    u64 m_lastSubmitted = 0; // what collect() last saw
    void release(const vk::Device& device, Retired& retired);
};

} // namespace vd
