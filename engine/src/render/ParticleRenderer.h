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
} // namespace vk
class ModelRenderer;

// Particles and ribbons (internal to render/, phase 9): quads the game wrote into a mapped vertex ring, drawn
// after every other transparent surface with one pipeline per blend mode (created with the others, never in
// game). Depth tested, not written; textures are the models' (ModelRenderer's descriptor sets, clamped).
class ParticleRenderer {
public:
    static constexpr u32 kMaxQuads = 16384; // per frame: 4 vertices each, 16-bit indices
    // The game fills a buffer before renderFrame() waits for its frame's fence (like the bone palettes): one
    // more buffer than frames in flight.
    static constexpr u32 kBuffers = 3;
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                      VkDescriptorSetLayout textureLayout);
    void shutdown(const vk::Device& device);
    Result<void> createPipelines(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipelines(const vk::Device& device);

    // Mapped vertices of buffer `buffer` (0..kBuffers): write-only.
    std::span<ParticleVertex> vertices(u32 buffer) const;
    // Once per frame, after the frame's fence: the quads of `buffer` the frame in `slot` draws.
    void prepare(const vk::Device& device, u32 slot, u32 buffer, u32 quads);
    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, std::span<const ParticleDraw> draws,
              const ModelRenderer& textures, RenderStats& stats) const;

private:
    enum Pipeline : u8 { kAlpha, kAdd, kMod, kMod2x, kPipelineCount };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
    };

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::array<VkPipeline, kPipelineCount> m_pipelines{};
    std::array<Buffer, kBuffers> m_vertices{};
    Buffer m_indices;
    std::array<u32, kMaxFrames> m_bufferOf{}; // vertex buffer each frame slot draws from
    std::array<u32, kMaxFrames> m_quads{};
};

} // namespace vd
