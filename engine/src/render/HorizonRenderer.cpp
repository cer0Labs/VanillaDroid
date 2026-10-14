#include "render/HorizonRenderer.h"

#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kHorizonVertSpv[] = {
#include "horizon.vert.spv.inc"
};
constexpr u32 kHorizonFragSpv[] = {
#include "horizon.frag.spv.inc"
};

static_assert(sizeof(HorizonDraw) == 16, "HorizonDraw must match the shader's Instance (std430)");

} // namespace

Result<void> HorizonRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout, u32 framesInFlight) {
    VkDevice dev = device.device();
    m_frames = std::min(framesInFlight, kMaxFrames);

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_setLayout));

    const std::array<VkDescriptorSetLayout, 2> setLayouts{frameLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

    // Phase 18 (world): the sets of maps being retired live next to the current ones for a frame or two.
    const u32 sets = m_frames * (1 + kMaxRetired);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * sets};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = sets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));

    for (u32 i = 0; i < m_frames; ++i) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = kMaxInstances * sizeof(HorizonDraw);
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_instances[i].buffer,
                                  &m_instances[i].allocation, &info));
        m_instances[i].mapped = static_cast<u8*>(info.pMappedData);
    }
    m_retired.reserve(kMaxRetired + 1);
    return {};
}

void HorizonRenderer::shutdown(const vk::Device& device) {
    if (device.device() == VK_NULL_HANDLE) {
        return;
    }
    clear(device);
    destroyPipeline(device);
    for (Buffer& instances : m_instances) {
        if (instances.buffer) {
            vmaDestroyBuffer(device.allocator(), instances.buffer, instances.allocation);
        }
    }
    if (m_pool) vkDestroyDescriptorPool(device.device(), m_pool, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device.device(), m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(device.device(), m_setLayout, nullptr);
    *this = HorizonRenderer{};
}

Result<void> HorizonRenderer::createPipeline(const vk::Device& device, VkRenderPass renderPass) {
    vk::GraphicsPipelineDesc desc;
    desc.vertexSpv = kHorizonVertSpv;
    desc.fragmentSpv = kHorizonFragSpv;
    desc.layout = m_pipelineLayout;
    desc.renderPass = renderPass;
    desc.name = "horizon pipeline";
    Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
    if (!pipeline) {
        return pipeline.error();
    }
    m_pipeline = pipeline.value();
    return {};
}

void HorizonRenderer::destroyPipeline(const vk::Device& device) {
    if (m_pipeline) {
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
}

Result<void> HorizonRenderer::setData(const vk::Device& device, vk::Uploader& uploader, std::span<const u32> heights,
                                      std::span<const u16> indices) {
    retire(m_lastSubmitted + kRetireFrames);
    if (m_retired.size() > kMaxRetired) { // map after map within a frame or two: wait for the oldest (rare)
        vkDeviceWaitIdle(device.device());
        collect(device, m_lastSubmitted, true);
    }
    if (heights.empty() || indices.empty()) {
        return {};
    }
    const u64 heightBytes = heights.size_bytes();
    const u64 indexBytes = indices.size_bytes();
    vk::Uploader::Staging staging;
    if (!uploader.reserve(heightBytes + 16 + indexBytes, staging)) {
        return Error{"Upload ring full (horizon)"};
    }
    auto create = [&](u64 size, VkBufferUsageFlags usage, Buffer& out) -> Result<void> {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &out.buffer, &out.allocation, nullptr));
        return {};
    };
    VD_TRY(create(heightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_heights));
    VD_TRY(create(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_indices));
    const u64 indexStaging = (heightBytes + 15) / 16 * 16;
    std::memcpy(staging.data, heights.data(), heightBytes);
    std::memcpy(staging.data + indexStaging, indices.data(), indexBytes);
    uploader.commit(device, staging);
    uploader.copyBuffer(staging, 0, m_heights.buffer, 0, heightBytes);
    uploader.copyBuffer(staging, indexStaging, m_indices.buffer, 0, indexBytes);
    m_indexCount = static_cast<u32>(indices.size());
    m_gpuBytes = heightBytes + indexBytes;

    std::array<VkDescriptorSetLayout, kMaxFrames> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = m_frames;
    setInfo.pSetLayouts = layouts.data();
    VD_VK_TRY(vkAllocateDescriptorSets(device.device(), &setInfo, m_sets.data()));
    for (u32 i = 0; i < m_frames; ++i) {
        const std::array<VkDescriptorBufferInfo, 2> buffers{{
            {m_heights.buffer, 0, VK_WHOLE_SIZE},
            {m_instances[i].buffer, 0, VK_WHOLE_SIZE},
        }};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (u32 b = 0; b < writes.size(); ++b) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = m_sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(device.device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
    return {};
}

void HorizonRenderer::retire(u64 retireFrame) {
    if (!m_heights.buffer && !m_indices.buffer && !m_sets[0]) {
        return;
    }
    Retired retired;
    retired.heights = m_heights;
    retired.indices = m_indices;
    retired.sets = m_sets;
    retired.retireFrame = retireFrame;
    m_retired.push_back(retired);
    m_heights = {};
    m_indices = {};
    m_sets = {};
    m_indexCount = 0;
    m_gpuBytes = 0;
}

void HorizonRenderer::release(const vk::Device& device, Retired& retired) {
    for (VkDescriptorSet& set : retired.sets) {
        if (set) {
            vkFreeDescriptorSets(device.device(), m_pool, 1, &set);
        }
    }
    for (Buffer* buffer : {&retired.heights, &retired.indices}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(device.allocator(), buffer->buffer, buffer->allocation);
        }
    }
    retired = {};
}

void HorizonRenderer::collect(const vk::Device& device, u64 submittedFrames, bool everything) {
    m_lastSubmitted = submittedFrames;
    usize kept = 0;
    for (Retired& retired : m_retired) {
        if (everything || submittedFrames >= retired.retireFrame) {
            release(device, retired);
        } else {
            m_retired[kept++] = retired;
        }
    }
    m_retired.resize(kept);
}

void HorizonRenderer::clear(const vk::Device& device) {
    retire(0);
    collect(device, m_lastSubmitted, true);
}

void HorizonRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 frameSlot,
                           std::span<const HorizonDraw> draws, const vk::Device& device, RenderStats& stats) {
    if (!m_pipeline || m_indexCount == 0 || draws.empty() || frameSlot >= m_frames) {
        return;
    }
    const u32 count = std::min<u32>(static_cast<u32>(draws.size()), kMaxInstances);
    const Buffer& instances = m_instances[frameSlot];
    std::memcpy(instances.mapped, draws.data(), count * sizeof(HorizonDraw));
    vmaFlushAllocation(device.allocator(), instances.allocation, 0, count * sizeof(HorizonDraw));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const std::array<VkDescriptorSet, 2> sets{frameSet, m_sets[frameSlot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, static_cast<u32>(sets.size()),
                            sets.data(), 0, nullptr);
    vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, m_indexCount, count, 0, 0, 0);
    ++stats.drawCalls;
    stats.triangles += m_indexCount / 3 * count;
}

} // namespace vd
