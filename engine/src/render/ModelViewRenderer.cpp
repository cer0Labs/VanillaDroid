#include "render/ModelViewRenderer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/FrameUniforms.h"
#include "render/ModelRenderer.h"
#include "render/vk/Device.h"
#include "render/vk/WorldPass.h"

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

VkImageMemoryBarrier layerBarrier(VkImage image, u32 layer, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                                  VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    return barrier;
}

} // namespace

Result<void> ModelViewRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                                     u32 framesInFlight) {
    VkDevice dev = device.device();
    m_frames = std::min(framesInFlight, kMaxFrames);
    const u32 sets = m_frames * kModelViewSlots;
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>(16, device.properties().limits.minUniformBufferOffsetAlignment);
    m_uniformStride = (sizeof(FrameUniforms) + alignment - 1) / alignment * alignment;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = m_uniformStride * sets;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(
        vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_uniforms, &m_uniformsAllocation, &info));
    m_uniformsMapped = static_cast<u8*>(info.pMappedData);

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = sets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));
    std::array<VkDescriptorSetLayout, kMaxFrames * kModelViewSlots> layouts;
    layouts.fill(frameLayout);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = sets;
    setInfo.pSetLayouts = layouts.data();
    VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, m_sets.data()));
    for (u32 i = 0; i < sets; ++i) {
        const VkDescriptorBufferInfo buffer{m_uniforms, m_uniformStride * i, sizeof(FrameUniforms)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_sets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }

    if (device.timestampValidBits() > 0) {
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * m_frames;
        VD_VK_TRY(vkCreateQueryPool(dev, &queryInfo, nullptr, &m_queries));
        m_timestampPeriodNs = device.properties().limits.timestampPeriod;
        const u32 bits = device.timestampValidBits();
        m_timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    }
    return {};
}

void ModelViewRenderer::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    destroyTargets(device);
    if (m_queries)
        vkDestroyQueryPool(dev, m_queries, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_uniforms)
        vmaDestroyBuffer(device.allocator(), m_uniforms, m_uniformsAllocation);
    const u32 generation = m_generation;
    *this = ModelViewRenderer{};
    m_generation = generation;
}

Result<void> ModelViewRenderer::createTargets(const vk::Device& device, VkFormat colorFormat, VkFormat depthFormat,
                                              const VkSubpassDependency& mainDependency,
                                              VkSampleCountFlagBits samples) {
    destroyTargets(device);
    VkDevice dev = device.device();

    // Compatible with the main render pass (as the portraits' pass): same attachments, subpass and dependency.
    Result<VkRenderPass> pass =
        vk::createWorldPass(device, colorFormat, depthFormat, samples, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, mainDependency); // phase 18 (world)
    if (!pass) {
        return pass.error();
    }
    m_renderPass = pass.value();

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = colorFormat;
    imageInfo.extent = {kModelViewWidth, kModelViewHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = kModelViewSlots;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VD_VK_TRY(vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &m_image, &m_imageAllocation, nullptr));

    imageInfo.samples = samples; // phase 18 (world): the main pass's
    imageInfo.format = depthFormat;
    imageInfo.arrayLayers = 1;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    if (vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &m_depth, &m_depthAllocation, nullptr) !=
        VK_SUCCESS) {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(
            vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &m_depth, &m_depthAllocation, nullptr));
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_depth;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    const bool stencil = depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u), 0, 1, 0,
                                 1};
    VD_VK_TRY(vkCreateImageView(dev, &viewInfo, nullptr, &m_depthView));

    if (samples != VK_SAMPLE_COUNT_1_BIT) { // phase 18 (world): drawn here, resolved into each slot on the tile
        Result<vk::TransientImage> color =
            vk::createTransientImage(device, colorFormat, {kModelViewWidth, kModelViewHeight}, samples, false);
        if (!color) {
            return color.error();
        }
        m_multisampled = color.value().image;
        m_multisampledAllocation = color.value().allocation;
        m_multisampledView = color.value().view;
    }
    for (u32 slot = 0; slot < kModelViewSlots; ++slot) {
        viewInfo.image = m_image;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot, 1};
        VD_VK_TRY(vkCreateImageView(dev, &viewInfo, nullptr, &m_targetViews[slot]));
        VD_VK_TRY(vkCreateImageView(dev, &viewInfo, nullptr, &m_sampledViews[slot]));
        std::array<VkImageView, 3> views{};
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount =
            vk::worldPassViews(samples, m_multisampledView, m_depthView, m_targetViews[slot], views);
        framebufferInfo.pAttachments = views.data();
        framebufferInfo.width = kModelViewWidth;
        framebufferInfo.height = kModelViewHeight;
        framebufferInfo.layers = 1;
        VD_VK_TRY(vkCreateFramebuffer(dev, &framebufferInfo, nullptr, &m_framebuffers[slot]));
    }
    ++m_generation;
    VD_LOG_INFO("Model views: %u slots of %ux%u px (%.1f MB)", kModelViewSlots, kModelViewWidth, kModelViewHeight,
                static_cast<f64>(kModelViewSlots) * kModelViewWidth * kModelViewHeight * 4.0 / 1e6);
    return {};
}

void ModelViewRenderer::destroyTargets(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    for (u32 slot = 0; slot < kModelViewSlots; ++slot) {
        if (m_framebuffers[slot])
            vkDestroyFramebuffer(dev, m_framebuffers[slot], nullptr);
        if (m_targetViews[slot])
            vkDestroyImageView(dev, m_targetViews[slot], nullptr);
        if (m_sampledViews[slot])
            vkDestroyImageView(dev, m_sampledViews[slot], nullptr);
        m_framebuffers[slot] = VK_NULL_HANDLE;
        m_targetViews[slot] = VK_NULL_HANDLE;
        m_sampledViews[slot] = VK_NULL_HANDLE;
    }
    vk::TransientImage multisampled{m_multisampled, m_multisampledAllocation, m_multisampledView};
    vk::destroyTransientImage(device, multisampled); // phase 18 (world)
    m_multisampled = VK_NULL_HANDLE;
    m_multisampledAllocation = VK_NULL_HANDLE;
    m_multisampledView = VK_NULL_HANDLE;
    if (m_depthView)
        vkDestroyImageView(dev, m_depthView, nullptr);
    if (m_depth)
        vmaDestroyImage(device.allocator(), m_depth, m_depthAllocation);
    if (m_image)
        vmaDestroyImage(device.allocator(), m_image, m_imageAllocation);
    if (m_renderPass)
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
    m_depthView = VK_NULL_HANDLE;
    m_depth = VK_NULL_HANDLE;
    m_depthAllocation = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_imageAllocation = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
}

void ModelViewRenderer::readTimestamps(const vk::Device& device, u32 frameSlot, RenderStats& stats) {
    if (frameSlot >= m_frames || !m_queriesPending[frameSlot]) {
        return;
    }
    m_queriesPending[frameSlot] = false;
    std::array<u64, 2> ticks{};
    if (vkGetQueryPoolResults(device.device(), m_queries, frameSlot * 2, 2, sizeof(ticks), ticks.data(),
                              sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        stats.modelViewGpuMs = static_cast<f32>(static_cast<f64>((ticks[1] - ticks[0]) & m_timestampMask) *
                                                m_timestampPeriodNs / 1.0e6);
    }
}

void ModelViewRenderer::record(const vk::Device& device, VkCommandBuffer cmd, u32 frameSlot,
                               std::span<const PortraitDraw> views, std::span<const ActorDraw> actors,
                               std::span<const u16> geosets, std::span<const glm::vec4> colors,
                               const ModelRenderer& models, RenderStats& stats) {
    stats.modelViewRenders = 0;
    if (views.empty() || !m_renderPass || frameSlot >= m_frames) {
        return;
    }
    VD_PROFILE_ZONE("ModelViewRenderer::record");
    const u32 count = std::min<u32>(static_cast<u32>(views.size()), kModelViewSlots);
    if (m_queries) {
        vkCmdResetQueryPool(cmd, m_queries, frameSlot * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queries, frameSlot * 2);
    }
    // The layers become attachments once the fragment shaders that sampled them (earlier frames' UI) are done.
    std::array<VkImageMemoryBarrier, kModelViewSlots> barriers{};
    std::array<u32, kModelViewSlots> layers{};
    for (u32 i = 0; i < count; ++i) {
        layers[i] = std::min(views[i].slot, kModelViewSlots - 1);
        barriers[i] = layerBarrier(m_image, layers[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, count, barriers.data());

    for (u32 i = 0; i < count; ++i) {
        const PortraitDraw& v = views[i];
        const u32 set = frameSlot * kModelViewSlots + i;
        FrameUniforms uniforms{};
        uniforms.viewProj = v.viewProj;
        uniforms.lightDirection = glm::vec4(v.lightDirection, 0.0f);
        uniforms.directColor = glm::vec4(v.directColor, 1.0f);
        uniforms.ambientColor = glm::vec4(v.ambientColor, 1.0f);
        uniforms.fogColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        uniforms.fogParams = glm::vec4(1.0e9f, 0.0f, 0.0f, 1.0e9f); // no fog
        uniforms.conditions = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);   // z: opaque surfaces write coverage
        std::memcpy(m_uniformsMapped + m_uniformStride * set, &uniforms, sizeof(uniforms));
        vmaFlushAllocation(device.allocator(), m_uniformsAllocation, m_uniformStride * set, sizeof(uniforms));

        std::array<VkClearValue, 2> clear{};
        clear[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = m_renderPass;
        begin.framebuffer = m_framebuffers[layers[i]];
        begin.renderArea = {{0, 0}, {kModelViewWidth, kModelViewHeight}};
        begin.clearValueCount = static_cast<u32>(clear.size());
        begin.pClearValues = clear.data();
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{
            0.0f, 0.0f, static_cast<f32>(kModelViewWidth), static_cast<f32>(kModelViewHeight), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {kModelViewWidth, kModelViewHeight}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (u64{v.firstActor} + v.actorCount <= actors.size()) {
            const std::span<const ActorDraw> draws = actors.subspan(v.firstActor, v.actorCount);
            models.drawActors(cmd, m_sets[set], frameSlot, draws, geosets, colors, false, stats);
            models.drawActors(cmd, m_sets[set], frameSlot, draws, geosets, colors, true, stats);
        }
        vkCmdEndRenderPass(cmd);
        ++stats.modelViewRenders;
    }

    for (u32 i = 0; i < count; ++i) {
        barriers[i] = layerBarrier(m_image, layers[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT);
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, count, barriers.data());
    if (m_queries) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, frameSlot * 2 + 1);
        m_queriesPending[frameSlot] = true;
    }
}

} // namespace vd
