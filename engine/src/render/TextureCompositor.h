#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/vk/Texture.h"
#include "render/vk/VkCommon.h"

#include <span>
#include <vector>

namespace vd {

namespace vk {
class Device;
}

// Builds textures on the GPU from layers (internal to render/): a character's skin with its face, hair and clothes
// on the 256x256 atlas the race models map. Each composition is a tiny render pass recorded at the start of the next
// frame (after the uploads, before the scene), followed by blits for the mip chain. The pipelines are created at
// init; per composition only an image, two views and a framebuffer are made.
class TextureCompositor {
public:
    struct Layer {
        VkDescriptorSet texture; // combined image sampler (the model renderer's clamp set of that texture)
        f32 rect[4];             // x, y, width, height in target pixels
    };

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout textureLayout);
    void shutdown(const vk::Device& device);

    // The composed texture, ready once the next frame's commands run (the caller may sample it in that frame).
    Result<vk::Texture> compose(const vk::Device& device, u32 width, u32 height, std::span<const Layer> layers);
    // Records every queued composition. Outside a render pass.
    void record(VkCommandBuffer cmd, u64 frame);
    // Frees the framebuffers of compositions whose frame has completed.
    void collect(const vk::Device& device, u64 completedFrames, bool everything);

private:
    struct Pending {
        VkImage image;
        VkImageView targetView; // mip 0, for the framebuffer
        VkFramebuffer framebuffer;
        u32 width;
        u32 height;
        u32 mips;
        u32 firstLayer;
        u32 layerCount;
    };
    struct Retired {
        VkImageView view;
        VkFramebuffer framebuffer;
        u64 frame;
    };

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_copy = VK_NULL_HANDLE;  // first layer
    VkPipeline m_blend = VK_NULL_HANDLE; // the others, alpha blended (the target keeps the base alpha)
    std::vector<Pending> m_pending;
    std::vector<Layer> m_layers;
    std::vector<Retired> m_retired;
};

} // namespace vd
