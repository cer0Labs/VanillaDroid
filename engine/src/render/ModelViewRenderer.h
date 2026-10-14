#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>

namespace vd {

namespace vk {
class Device;
}
class ModelRenderer;

// Phase 15: the interface's 3D model frames (internal to render/): kModelViewSlots images (layers of one 2D array)
// that a small render pass each fills every frame the game shows them (FrameParams::modelViews), before the scene.
// Like the portraits' pass it is compatible with the main one, so the actors' pipelines draw in it as they are; unlike
// them the images keep a real alpha: cleared to transparent, the opaque and alpha-tested surfaces write 1 (the frame
// uniforms' coverage flag, conditions.z) and the blended ones leave it as it was, so the windows draw the model over
// their own art. One level, no mips: the frames show them at about their size. Depth stays on tile.
class ModelViewRenderer {
public:
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    // With the main render pass's formats and subpass dependency (render pass compatibility). Destroying them loses
    // the images: generation() changes.
    // Phase 18 (world): `samples` as the main pass's (multisampling: a transient color resolved into the slot).
    Result<void> createTargets(const vk::Device& device, VkFormat colorFormat, VkFormat depthFormat,
                               const VkSubpassDependency& mainDependency,
                               VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void destroyTargets(const vk::Device& device);
    VkImageView sampledView(u32 slot) const {
        return slot < kModelViewSlots ? m_sampledViews[slot] : VK_NULL_HANDLE;
    }
    u32 generation() const { return m_generation; }

    // After the frame's fence: the GPU time of the model views that frame slot rendered last time.
    void readTimestamps(const vk::Device& device, u32 frameSlot, RenderStats& stats);
    // Renders `views` (at most kModelViewSlots) with the frame's actor data. Outside a render pass, after the bone
    // palette copy; the images are ready for the fragment shaders of what follows.
    void record(const vk::Device& device, VkCommandBuffer cmd, u32 frameSlot, std::span<const PortraitDraw> views,
                std::span<const ActorDraw> actors, std::span<const u16> geosets, std::span<const glm::vec4> colors,
                const ModelRenderer& models, RenderStats& stats);

private:
    VkBuffer m_uniforms = VK_NULL_HANDLE;
    VmaAllocation m_uniformsAllocation = VK_NULL_HANDLE;
    u8* m_uniformsMapped = nullptr;
    VkDeviceSize m_uniformStride = 0;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFrames * kModelViewSlots> m_sets{}; // [frame slot][view]
    u32 m_frames = 0;

    VkQueryPool m_queries = VK_NULL_HANDLE; // two timestamps per frame slot
    f64 m_timestampPeriodNs = 0.0;
    u64 m_timestampMask = 0;
    std::array<bool, kMaxFrames> m_queriesPending{};

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkImage m_multisampled = VK_NULL_HANDLE; // phase 18 (world): with multisampling, transient
    VmaAllocation m_multisampledAllocation = VK_NULL_HANDLE;
    VkImageView m_multisampledView = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE; // kModelViewSlots layers
    VmaAllocation m_imageAllocation = VK_NULL_HANDLE;
    VkImage m_depth = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    std::array<VkImageView, kModelViewSlots> m_targetViews{};
    std::array<VkImageView, kModelViewSlots> m_sampledViews{};
    std::array<VkFramebuffer, kModelViewSlots> m_framebuffers{};
    u32 m_generation = 0;
};

} // namespace vd
