#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "render/vk/Texture.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>

namespace vd {

namespace vk {
class Device;
class Uploader;
} // namespace vk
class ModelRenderer;

// Touch UI (internal to render/, phase 7): one pipeline and one descriptor set (sprite atlas, icon cache, font
// distance fields) for every quad; a draw command only changes the scissor or the external texture.
class UiRenderer {
public:
    static constexpr u32 kMaxQuads = 16384; // 4 vertices each: fits 16-bit indices
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout textureLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    Result<void> createPipeline(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipeline(const vk::Device& device);

    // Waits for the GPU to go idle (loading only).
    Result<void> setTextures(const vk::Device& device, const u8* atlas, usize atlasSize, const u8* fonts,
                             usize fontsSize);
    // Rewrites one icon slot from a pack blob (see Renderer::setUiIcon) without stalling.
    bool setIcon(const vk::Device& device, vk::Uploader& uploader, u32 slot, VkBuffer src, u64 srcOffset,
                 const u8* blob, usize size);

    // extent: the framebuffer; logical: as the player sees it; preRotate: the swapchain pre-rotation (2x2).
    void draw(const vk::Device& device, VkCommandBuffer cmd, u32 frameSlot, std::span<const UiVertex> vertices,
              std::span<const UiDrawCmd> commands, VkExtent2D extent, VkExtent2D logical, const f32 preRotate[4],
              const ModelRenderer& models, RenderStats& stats);

private:
    void writeTextureDescriptors(const vk::Device& device);

    struct FrameBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
    };

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE; // linear, clamped, with mips
    vk::Texture m_atlas;
    vk::Texture m_icons;
    vk::Texture m_fonts;
    std::array<FrameBuffer, kMaxFrames> m_vertices{};
    FrameBuffer m_indices;
    u32 m_frames = 0;
};

} // namespace vd
