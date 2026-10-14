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

// Portraits of the unit frames (internal to render/): kPortraitSlots square images (layers of one 2D array)
// that a tiny render pass each fills when the game asks (FrameParams::portraits), before the scene. The pass
// is compatible with the main one (same formats, same subpass dependency), so the actors' pipelines draw in
// it as they are: no pipeline of its own. Its depth stays on tile (transient, cleared, not stored); its color
// is cleared to black and stored. Each layer has two views: the framebuffer's, and the one the UI samples,
// which reads alpha as 1 (the actor pipelines write their texels' alpha, meaningless in a portrait).
class PortraitRenderer {
public:
    static constexpr u32 kMaxFrames = 4;

    // Uniform buffers and descriptor sets (set 0 of the actor pipelines, `frameLayout`), timestamp queries.
    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    // With the main render pass, in its formats and with its subpass dependency (render pass compatibility).
    // Destroying them loses the images: generation() changes.
    // Phase 18 (world): `samples` as the main pass's (multisampling: a transient color resolved into the slot).
    Result<void> createTargets(const vk::Device& device, VkFormat colorFormat, VkFormat depthFormat,
                               const VkSubpassDependency& mainDependency,
                               VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void destroyTargets(const vk::Device& device);
    // The view the UI samples (alpha read as 1), VK_NULL_HANDLE without targets.
    VkImageView sampledView(u32 slot) const {
        return slot < kPortraitSlots ? m_sampledViews[slot] : VK_NULL_HANDLE;
    }
    u32 generation() const { return m_generation; }

    // After the frame's fence: the GPU time of the portraits that frame slot rendered last time.
    void readTimestamps(const vk::Device& device, u32 frameSlot, RenderStats& stats);
    // Renders `portraits` (at most kMaxPortraitRenders) with the frame's actor data. Outside a render pass,
    // after the bone palette copy; the images are ready for the fragment shaders of what follows.
    void record(const vk::Device& device, VkCommandBuffer cmd, u32 frameSlot,
                std::span<const PortraitDraw> portraits, std::span<const ActorDraw> actors,
                std::span<const u16> geosets, std::span<const glm::vec4> colors, const ModelRenderer& models,
                RenderStats& stats);

private:
    VkBuffer m_uniforms = VK_NULL_HANDLE;
    VmaAllocation m_uniformsAllocation = VK_NULL_HANDLE;
    u8* m_uniformsMapped = nullptr;
    VkDeviceSize m_uniformStride = 0;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFrames * kMaxPortraitRenders> m_sets{}; // [frame slot][portrait]
    u32 m_frames = 0;

    VkQueryPool m_queries = VK_NULL_HANDLE; // two timestamps per frame slot
    f64 m_timestampPeriodNs = 0.0;
    u64 m_timestampMask = 0;
    std::array<bool, kMaxFrames> m_queriesPending{};

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkImage m_multisampled = VK_NULL_HANDLE; // phase 18 (world): with multisampling, transient
    VmaAllocation m_multisampledAllocation = VK_NULL_HANDLE;
    VkImageView m_multisampledView = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE; // kPortraitSlots layers
    VmaAllocation m_imageAllocation = VK_NULL_HANDLE;
    VkImage m_depth = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    std::array<VkImageView, kPortraitSlots> m_targetViews{};
    std::array<VkImageView, kPortraitSlots> m_sampledViews{};
    std::array<VkFramebuffer, kPortraitSlots> m_framebuffers{};
    u32 m_generation = 0;
};

} // namespace vd
