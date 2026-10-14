#include "render/TextureCompositor.h"

#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"

#include <algorithm>
#include <array>

namespace vd {
namespace {

constexpr u32 kComposeVertSpv[] = {
#include "compose.vert.spv.inc"
};
constexpr u32 kComposeFragSpv[] = {
#include "compose.frag.spv.inc"
};

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Must match the push_constant block in shaders/compose.vert.
struct ComposePush {
    f32 rect[4];
    f32 target[2];
    f32 padding[2];
};

VkImageMemoryBarrier mipBarrier(VkImage image, u32 mip, u32 count, VkImageLayout from, VkImageLayout to,
                                VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
    return barrier;
}

} // namespace

Result<void> TextureCompositor::init(const vk::Device& device, VkDescriptorSetLayout textureLayout) {
    VkDevice dev = device.device();
    VkAttachmentDescription attachment{};
    attachment.format = kFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // then blitted into the mips
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; // layer textures uploaded earlier in the command buffer
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL; // the mip blits
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = static_cast<u32>(dependencies.size());
    passInfo.pDependencies = dependencies.data();
    VD_VK_TRY(vkCreateRenderPass(dev, &passInfo, nullptr, &m_renderPass));

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ComposePush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &textureLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_layout));

    for (const bool blend : {false, true}) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kComposeVertSpv;
        desc.fragmentSpv = kComposeFragSpv;
        desc.layout = m_layout;
        desc.renderPass = m_renderPass;
        desc.cullMode = VK_CULL_MODE_NONE;
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.blend = blend ? vk::BlendMode::Alpha : vk::BlendMode::Opaque;
        desc.name = "compose pipeline";
        desc.samples = VK_SAMPLE_COUNT_1_BIT; // phase 18 (world): its own pass, never multisampled
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        (blend ? m_blend : m_copy) = pipeline.value();
    }
    m_pending.reserve(64);
    m_layers.reserve(1024);
    m_retired.reserve(64);
    return {};
}

void TextureCompositor::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    collect(device, 0, true);
    for (const Pending& p : m_pending) { // never recorded: their images belong to textures destroyed elsewhere
        vkDestroyFramebuffer(dev, p.framebuffer, nullptr);
        vkDestroyImageView(dev, p.targetView, nullptr);
    }
    m_pending.clear();
    if (m_copy) vkDestroyPipeline(dev, m_copy, nullptr);
    if (m_blend) vkDestroyPipeline(dev, m_blend, nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_renderPass) vkDestroyRenderPass(dev, m_renderPass, nullptr);
    *this = TextureCompositor{};
}

Result<vk::Texture> TextureCompositor::compose(const vk::Device& device, u32 width, u32 height,
                                               std::span<const Layer> layers) {
    if (layers.empty() || width == 0 || height == 0 || !m_renderPass) {
        return Error{"Nothing to compose"};
    }
    VkDevice dev = device.device();
    vk::Texture texture;
    texture.format = kFormat;
    texture.width = width;
    texture.height = height;
    texture.mipCount = 1;
    while ((std::max(width, height) >> texture.mipCount) > 0) {
        ++texture.mipCount;
    }
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = texture.mipCount;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VD_VK_TRY(device.createStreamedImage(imageInfo, &texture.image, &texture.allocation));
    texture.gpuBytes = u64{width} * height * 4 * 4 / 3;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipCount, 0, 1};
    Pending pending{};
    VkResult result = vkCreateImageView(dev, &viewInfo, nullptr, &texture.view);
    if (result == VK_SUCCESS) {
        viewInfo.subresourceRange.levelCount = 1;
        result = vkCreateImageView(dev, &viewInfo, nullptr, &pending.targetView);
    }
    if (result == VK_SUCCESS) {
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &pending.targetView;
        framebufferInfo.width = width;
        framebufferInfo.height = height;
        framebufferInfo.layers = 1;
        result = vkCreateFramebuffer(dev, &framebufferInfo, nullptr, &pending.framebuffer);
    }
    if (result != VK_SUCCESS) {
        if (pending.targetView) {
            vkDestroyImageView(dev, pending.targetView, nullptr);
        }
        vk::destroyTexture(device, texture);
        return vk::makeError("compose target", result);
    }
    pending.image = texture.image;
    pending.width = width;
    pending.height = height;
    pending.mips = texture.mipCount;
    pending.firstLayer = static_cast<u32>(m_layers.size());
    pending.layerCount = static_cast<u32>(layers.size());
    m_layers.insert(m_layers.end(), layers.begin(), layers.end());
    m_pending.push_back(pending);
    return texture;
}

void TextureCompositor::record(VkCommandBuffer cmd, u64 frame) {
    for (const Pending& p : m_pending) {
        VkClearValue clear{};
        clear.color = {{0.5f, 0.5f, 0.5f, 1.0f}};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = m_renderPass;
        begin.framebuffer = p.framebuffer;
        begin.renderArea = {{0, 0}, {p.width, p.height}};
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(p.width), static_cast<f32>(p.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {p.width, p.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        for (u32 i = 0; i < p.layerCount; ++i) {
            const Layer& layer = m_layers[p.firstLayer + i];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, i == 0 ? m_copy : m_blend);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &layer.texture, 0, nullptr);
            const ComposePush push{{layer.rect[0], layer.rect[1], layer.rect[2], layer.rect[3]},
                                   {static_cast<f32>(p.width), static_cast<f32>(p.height)},
                                   {0.0f, 0.0f}};
            vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);

        // Mip chain: each level blitted from the one above (mip 0 is already TRANSFER_SRC).
        for (u32 mip = 1; mip < p.mips; ++mip) {
            VkImageMemoryBarrier toDst = mipBarrier(p.image, mip, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &toDst);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
            blit.srcOffsets[1] = {static_cast<i32>(std::max(1u, p.width >> (mip - 1))),
                                  static_cast<i32>(std::max(1u, p.height >> (mip - 1))), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            blit.dstOffsets[1] = {static_cast<i32>(std::max(1u, p.width >> mip)),
                                  static_cast<i32>(std::max(1u, p.height >> mip)), 1};
            vkCmdBlitImage(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            VkImageMemoryBarrier toSrc =
                mipBarrier(p.image, mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &toSrc);
        }
        VkImageMemoryBarrier toShader =
            mipBarrier(p.image, 0, p.mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toShader);
        m_retired.push_back({p.targetView, p.framebuffer, frame});
    }
    m_pending.clear();
    m_layers.clear();
}

void TextureCompositor::collect(const vk::Device& device, u64 completedFrames, bool everything) {
    usize kept = 0;
    for (const Retired& r : m_retired) {
        if (everything || completedFrames >= r.frame) {
            vkDestroyFramebuffer(device.device(), r.framebuffer, nullptr);
            vkDestroyImageView(device.device(), r.view, nullptr);
        } else {
            m_retired[kept++] = r;
        }
    }
    m_retired.resize(kept);
}

} // namespace vd
