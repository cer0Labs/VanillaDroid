#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/vk/VkCommon.h"

#include <array>

namespace vd::vk {

class Device;

// Phase 18 (world): the render passes the world's pipelines draw in (the main pass, the scene pass below full
// resolution, the portraits' and the model views'): compatible with each other, so with the same attachments and
// samples. With one sample: the target color and the depth. With several (multisampling, an option): a transient
// multisampled color and depth that never leave the tile, and the target as the color they resolve into on the tile
// (PERFORMANCE.md §3: no full-screen pass, no multisampled memory written out).
//   target: the single-sample color the pass leaves (the swapchain image, the scene image, a portrait layer), with
//           the layouts it comes in and goes out with; cleared on load when it is the only color.
Result<VkRenderPass> createWorldPass(const Device& device, VkFormat colorFormat, VkFormat depthFormat,
                                     VkSampleCountFlagBits samples, VkImageLayout targetInitial,
                                     VkImageLayout targetFinal, const VkSubpassDependency& dependency);

// A framebuffer's views in the order createWorldPass() lays its attachments out: {target, depth} with one sample,
// {multisampled color, depth, target} with several. Returns how many.
u32 worldPassViews(VkSampleCountFlagBits samples, VkImageView multisampled, VkImageView depth, VkImageView target,
                   std::array<VkImageView, 3>& out);

// A transient attachment (lazily allocated where the GPU has such memory, as the depth always was): the multisampled
// color, or a depth of `samples`. `aspect`: color or depth (with stencil when the format has it).
struct TransientImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
Result<TransientImage> createTransientImage(const Device& device, VkFormat format, VkExtent2D extent,
                                            VkSampleCountFlagBits samples, bool depth);
void destroyTransientImage(const Device& device, TransientImage& image);

} // namespace vd::vk
