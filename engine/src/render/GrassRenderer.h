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
class Uploader;
} // namespace vk
class ModelRenderer;

// The terrain's detail doodads (internal to render/, phase 14): every grass model's vertices and indices in one
// buffer (uploaded once), the frame's instances in a mapped ring, one instanced draw per model with its texture (the
// models' descriptor sets). One pipeline, created with the others: alpha blended and tested at 128 like WoW.exe's
// detail doodads, two-sided, writing depth; lit per vertex like the terrain with the normal the game gives each
// instance, fogged, and faded out with the view depth (shaders/grass.vert, grass.frag).
class GrassRenderer {
public:
    static constexpr u32 kMaxInstances = 16384; // per frame
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                      VkDescriptorSetLayout textureLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    Result<void> createPipelines(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipelines(const vk::Device& device);

    // The grass models blob (pack::GrassModelsHeader) at `srcOffset` in `src`; `blob` is its CPU copy. Once.
    Result<void> setModels(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 srcOffset,
                           const u8* blob, usize size);

    // Once per frame, after the frame's fence: copies the instances `slot` draws.
    void prepare(const vk::Device& device, u32 slot, std::span<const GrassInstance> instances);
    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, const FrameParams& params,
              const ModelRenderer& textures, RenderStats& stats) const;

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
    };

    u32 m_frames = 0;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::array<Buffer, kMaxFrames> m_instances{};
    std::array<u32, kMaxFrames> m_instanceCount{};
    Buffer m_geometry; // vertices, then indices from m_indexOffset
    u64 m_indexOffset = 0;
    u32 m_indexCount = 0;
    u32 m_vertexCount = 0;
};

} // namespace vd
