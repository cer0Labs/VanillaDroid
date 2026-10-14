#include "render/vk/WorldPass.h"

#include "render/vk/Device.h"

namespace vd::vk {

Result<VkRenderPass> createWorldPass(const Device& device, VkFormat colorFormat, VkFormat depthFormat,
                                     VkSampleCountFlagBits samples, VkImageLayout targetInitial,
                                     VkImageLayout targetFinal, const VkSubpassDependency& dependency) {
    const bool multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
    std::array<VkAttachmentDescription, 3> attachments{};
    // The color drawn into: the target itself, or the multisampled one resolved into it at the end.
    attachments[0].format = colorFormat;
    attachments[0].samples = samples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = multisampled ? VK_IMAGE_LAYOUT_UNDEFINED : targetInitial;
    attachments[0].finalLayout = multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : targetFinal;
    attachments[1].format = depthFormat;
    attachments[1].samples = samples;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].format = colorFormat;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel resolved over
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = targetInitial;
    attachments[2].finalLayout = targetFinal;

    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = multisampled ? &resolveRef : nullptr;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = multisampled ? 3u : 2u;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VkRenderPass pass = VK_NULL_HANDLE;
    VD_VK_TRY(vkCreateRenderPass(device.device(), &info, nullptr, &pass));
    return pass;
}

u32 worldPassViews(VkSampleCountFlagBits samples, VkImageView multisampled, VkImageView depth, VkImageView target,
                   std::array<VkImageView, 3>& out) {
    if (samples == VK_SAMPLE_COUNT_1_BIT) {
        out = {target, depth, VK_NULL_HANDLE};
        return 2;
    }
    out = {multisampled, depth, target};
    return 3;
}

Result<TransientImage> createTransientImage(const Device& device, VkFormat format, VkExtent2D extent,
                                            VkSampleCountFlagBits samples, bool depth) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                      VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    TransientImage out;
    // Tiled GPUs back transient attachments with lazily allocated memory: no DRAM is ever committed.
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    if (vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) !=
        VK_SUCCESS) {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr));
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    viewInfo.subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u)
                                       : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 1, 0, 1};
    if (VkResult r = vkCreateImageView(device.device(), &viewInfo, nullptr, &out.view); r != VK_SUCCESS) {
        vmaDestroyImage(device.allocator(), out.image, out.allocation);
        return makeError("vkCreateImageView(transient)", r);
    }
    return out;
}

void destroyTransientImage(const Device& device, TransientImage& image) {
    if (image.view) {
        vkDestroyImageView(device.device(), image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(device.allocator(), image.image, image.allocation);
    }
    image = {};
}

} // namespace vd::vk
