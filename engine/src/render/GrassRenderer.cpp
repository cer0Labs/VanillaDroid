#include "render/GrassRenderer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "pack/GrassFormat.h"
#include "render/ModelRenderer.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kGrassVertSpv[] = {
#include "grass.vert.spv.inc"
};
constexpr u32 kGrassFragSpv[] = {
#include "grass.frag.spv.inc"
};

// Must match the push_constant block of shaders/grass.vert and grass.frag.
struct GrassPush {
    f32 fade[4]; // x: fade end (yards of view depth), y: 1 / (end - start)
};

} // namespace

Result<void> GrassRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                                 VkDescriptorSetLayout textureLayout, u32 framesInFlight) {
    m_frames = std::min(framesInFlight, kMaxFrames);
    const std::array<VkDescriptorSetLayout, 2> setLayouts{frameLayout, textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GrassPush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &m_layout));

    for (u32 i = 0; i < m_frames; ++i) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = u64{kMaxInstances} * sizeof(GrassInstance);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_instances[i].buffer,
                                  &m_instances[i].allocation, &info));
        m_instances[i].mapped = static_cast<u8*>(info.pMappedData);
    }
    return {};
}

void GrassRenderer::shutdown(const vk::Device& device) {
    if (device.device() == VK_NULL_HANDLE) {
        return;
    }
    destroyPipelines(device);
    for (Buffer& buffer : m_instances) {
        if (buffer.buffer) {
            vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
        }
        buffer = {};
    }
    if (m_geometry.buffer) {
        vmaDestroyBuffer(device.allocator(), m_geometry.buffer, m_geometry.allocation);
    }
    m_geometry = {};
    m_indexCount = 0;
    m_vertexCount = 0;
    if (m_layout) {
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
    }
    m_layout = VK_NULL_HANDLE;
}

Result<void> GrassRenderer::createPipelines(const vk::Device& device, VkRenderPass renderPass) {
    const std::array<VkVertexInputBindingDescription, 2> bindings{{
        {0, sizeof(pack::GrassVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(GrassInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 5> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(pack::GrassVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pack::GrassVertex, uv)},
        {2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GrassInstance, position)},
        {3, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(GrassInstance, rotation)},
        {4, 1, VK_FORMAT_R8G8B8A8_SNORM, offsetof(GrassInstance, normal)},
    }};
    vk::GraphicsPipelineDesc desc;
    desc.vertexSpv = kGrassVertSpv;
    desc.fragmentSpv = kGrassFragSpv;
    desc.layout = m_layout;
    desc.renderPass = renderPass;
    desc.bindings = bindings;
    desc.attributes = attributes;
    desc.cullMode = VK_CULL_MODE_NONE; // the client draws them two-sided (crossed quads seen from both sides)
    desc.blend = vk::BlendMode::Alpha; // blended and alpha tested at 128, writing depth, like the client
    desc.name = "grass pipeline";
    Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
    if (!pipeline) {
        return pipeline.error();
    }
    m_pipeline = pipeline.value();
    return {};
}

void GrassRenderer::destroyPipelines(const vk::Device& device) {
    if (m_pipeline) {
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
}

Result<void> GrassRenderer::setModels(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 srcOffset,
                                      const u8* blob, usize size) {
    if (m_geometry.buffer) {
        return Error{"Grass models are already set"};
    }
    pack::GrassModelsHeader header;
    if (!blob || size < sizeof(header)) {
        return Error{"Grass models blob too small"};
    }
    std::memcpy(&header, blob, sizeof(header));
    const u64 vertexBytes = u64{header.vertexCount} * sizeof(pack::GrassVertex);
    const u64 indexBytes = u64{header.indexCount} * sizeof(u16);
    if (header.version != pack::kGrassFormatVersion || header.vertexCount == 0 || header.indexCount == 0 ||
        header.verticesOffset + vertexBytes > size || header.indicesOffset + indexBytes > size) {
        return Error{"Corrupt grass models blob (re-bake grass)"};
    }
    m_indexOffset = (vertexBytes + 15) / 16 * 16;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = m_indexOffset + indexBytes;
    bufferInfo.usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VD_VK_TRY(device.createStreamedBuffer(bufferInfo, &m_geometry.buffer, &m_geometry.allocation));
    uploader.copyBufferFrom(src, srcOffset + header.verticesOffset, m_geometry.buffer, 0, vertexBytes);
    uploader.copyBufferFrom(src, srcOffset + header.indicesOffset, m_geometry.buffer, m_indexOffset, indexBytes);
    m_vertexCount = header.vertexCount;
    m_indexCount = header.indexCount;
    VD_LOG_INFO("Grass: %u models, %u vertices, %u indices on the GPU", header.modelCount, header.vertexCount,
                header.indexCount);
    return {};
}

void GrassRenderer::prepare(const vk::Device& device, u32 slot, std::span<const GrassInstance> instances) {
    if (slot >= m_frames) {
        return;
    }
    const u32 count = std::min<u32>(static_cast<u32>(instances.size()), kMaxInstances);
    if (count > 0) {
        std::memcpy(m_instances[slot].mapped, instances.data(), count * sizeof(GrassInstance));
        vmaFlushAllocation(device.allocator(), m_instances[slot].allocation, 0, count * sizeof(GrassInstance));
    }
    m_instanceCount[slot] = count;
}

void GrassRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, const FrameParams& params,
                         const ModelRenderer& textures, RenderStats& stats) const {
    if (params.grass.empty() || slot >= m_frames || m_instanceCount[slot] == 0 || !m_pipeline || !m_geometry.buffer) {
        return;
    }
    VD_PROFILE_ZONE("GrassRenderer::draw");
    const u32 limit = m_instanceCount[slot];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet, 0, nullptr);
    const std::array<VkBuffer, 2> buffers{m_geometry.buffer, m_instances[slot].buffer};
    const std::array<VkDeviceSize, 2> offsets{0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
    vkCmdBindIndexBuffer(cmd, m_geometry.buffer, m_indexOffset, VK_INDEX_TYPE_UINT16);
    const f32 range = std::max(params.grassFadeEnd - params.grassFadeStart, 0.01f);
    const GrassPush push{{params.grassFadeEnd, 1.0f / range, 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    for (const GrassDraw& draw : params.grass) {
        if (draw.instanceCount == 0 || !draw.texture.valid() || draw.firstInstance + draw.instanceCount > limit ||
            draw.firstIndex + draw.indexCount > m_indexCount || draw.vertexOffset >= m_vertexCount) {
            continue;
        }
        const VkDescriptorSet set = textures.textureSet(draw.texture.index, draw.clamp);
        if (set != boundTexture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &set, 0, nullptr);
            boundTexture = set;
        }
        vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, draw.firstIndex,
                         static_cast<i32>(draw.vertexOffset), draw.firstInstance);
        const u32 triangles = draw.indexCount / 3 * draw.instanceCount;
        ++stats.drawCalls;
        ++stats.grassDrawCalls;
        stats.triangles += triangles;
        stats.grassTriangles += triangles;
        stats.grassInstances += draw.instanceCount;
    }
}

} // namespace vd
