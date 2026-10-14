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
} // namespace vk
class ModelRenderer;

// The sky (internal to render/), drawn after the opaque world where nothing covered the far plane: the dome
// of the Light.dbc sky bands (procedural, on 1.12's rings), the stars model and the sun and the moons added
// over it, the clouds blended over those (procedural on the dome, from a noise texture), then a skybox model;
// and, after every transparent surface, the glares over the whole scene (phase 11). Every pipeline is created
// at load.
class SkyRenderer {
public:
    static constexpr u32 kSegments = 24;
    static constexpr u32 kVertexCount = 6 * kSegments * 6;      // ring gaps x segments x 2 triangles
    static constexpr u32 kCloudVertexCount = 5 * kSegments * 6; // the gaps above the horizon

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                      VkDescriptorSetLayout textureLayout);
    void shutdown(const vk::Device& device);
    Result<void> createPipeline(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipeline(const vk::Device& device);

    // A pack::SkyModelHeader blob; `partTextures`: Renderer texture slots per part (0 = the white
    // placeholder).
    Result<u32> createModel(const vk::Device& device, const u8* blob, usize size,
                            std::span<const u32> partTextures);

    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, const SkyFrame& sky,
              const ModelRenderer& textures, RenderStats& stats) const;
    void drawGlare(VkCommandBuffer cmd, VkDescriptorSet frameSet, const SkyFrame& sky,
                   const ModelRenderer& textures, RenderStats& stats) const;

private:
    enum ModelPipeline : u32 { kModelOpaque, kModelAlpha, kModelAdditive, kModelPipelineCount };
    struct Part {
        u32 firstIndex;
        u32 indexCount;
        u32 texture; // Renderer texture slot
        u8 blend;    // pack::ModelBlend
        bool clamp;
    };
    struct Model {
        VkBuffer buffer = VK_NULL_HANDLE; // vertices, then indices
        VmaAllocation allocation = VK_NULL_HANDLE;
        u64 indexOffset = 0;
        std::vector<Part> parts;
    };

    void drawSprite(VkCommandBuffer cmd, const SkySprite& sprite, bool horizonFade,
                    const ModelRenderer& textures, RenderStats& stats) const;
    // starMasks (the stars, SkyFrame::starMasks): every part added over what is there, but not behind the
    // moons; nullptr (a skybox): each part with its own blend.
    void drawModel(VkCommandBuffer cmd, const SkyModelDraw& draw, const glm::vec4* starMasks,
                   const ModelRenderer& textures, RenderStats& stats) const;

    VkPipelineLayout m_layout = VK_NULL_HANDLE;         // set 0: the frame
    VkPipelineLayout m_texturedLayout = VK_NULL_HANDLE; // + set 1: one texture, + SkyPush
    VkPipeline m_pipeline = VK_NULL_HANDLE;             // the dome
    VkPipeline m_clouds = VK_NULL_HANDLE;
    VkPipeline m_sprite = VK_NULL_HANDLE; // added at the far plane
    VkPipeline m_glare = VK_NULL_HANDLE;  // added over the scene, no depth test
    std::array<VkPipeline, kModelPipelineCount> m_models{};
    std::vector<Model> m_skyModels{1}; // slot 0 reserved: handle 0 is invalid
};

} // namespace vd
