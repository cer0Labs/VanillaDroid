#include "render/ParticleRenderer.h"

#include "core/Profile.h"
#include "pack/ModelFormat.h"
#include "render/ModelRenderer.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kParticleVertSpv[] = {
#include "particle.vert.spv.inc"
};
constexpr u32 kParticleFragSpv[] = {
#include "particle.frag.spv.inc"
};

// Must match the push_constant block of shaders/particle.vert and particle.frag.
struct ParticlePush {
    u32 params[4]; // blend, flags
};

Result<void> createMappedBuffer(const vk::Device& device, u64 size, VkBufferUsageFlags usage,
                                VkBuffer& buffer, VmaAllocation& allocation, u8*& mapped) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &info));
    mapped = static_cast<u8*>(info.pMappedData);
    return {};
}

} // namespace

Result<void> ParticleRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                                    VkDescriptorSetLayout textureLayout) {
    const std::array<VkDescriptorSetLayout, 2> setLayouts{frameLayout, textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(ParticlePush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &m_layout));

    for (Buffer& buffer : m_vertices) {
        VD_TRY(createMappedBuffer(device, u64{kMaxQuads} * 4 * sizeof(ParticleVertex),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buffer.buffer, buffer.allocation,
                                  buffer.mapped));
    }
    // Quads as two triangles over their corners (-x +y), (-x -y), (+x +y), (+x -y).
    VD_TRY(createMappedBuffer(device, u64{kMaxQuads} * 6 * sizeof(u16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              m_indices.buffer, m_indices.allocation, m_indices.mapped));
    auto* indices = reinterpret_cast<u16*>(m_indices.mapped);
    for (u32 q = 0; q < kMaxQuads; ++q) {
        const u16 base = static_cast<u16>(q * 4);
        const u16 quad[6] = {base,
                             static_cast<u16>(base + 1),
                             static_cast<u16>(base + 2),
                             static_cast<u16>(base + 2),
                             static_cast<u16>(base + 1),
                             static_cast<u16>(base + 3)};
        std::memcpy(indices + q * 6, quad, sizeof(quad));
    }
    vmaFlushAllocation(device.allocator(), m_indices.allocation, 0, VK_WHOLE_SIZE);
    return {};
}

void ParticleRenderer::shutdown(const vk::Device& device) {
    if (device.device() == VK_NULL_HANDLE) {
        return;
    }
    destroyPipelines(device);
    for (Buffer& buffer : m_vertices) {
        if (buffer.buffer) {
            vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
        }
        buffer = {};
    }
    if (m_indices.buffer) {
        vmaDestroyBuffer(device.allocator(), m_indices.buffer, m_indices.allocation);
    }
    m_indices = {};
    if (m_layout) {
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
    }
    m_layout = VK_NULL_HANDLE;
}

Result<void> ParticleRenderer::createPipelines(const vk::Device& device, VkRenderPass renderPass) {
    const VkVertexInputBindingDescription binding{0, sizeof(ParticleVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleVertex, position)},
        {1, 0, VK_FORMAT_R16G16_UNORM, offsetof(ParticleVertex, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ParticleVertex, color)},
    }};
    const vk::BlendMode modes[kPipelineCount] = {vk::BlendMode::Alpha, vk::BlendMode::Additive,
                                                 vk::BlendMode::Modulate, vk::BlendMode::Modulate2x};
    for (u32 i = 0; i < kPipelineCount; ++i) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kParticleVertSpv;
        desc.fragmentSpv = kParticleFragSpv;
        desc.layout = m_layout;
        desc.renderPass = renderPass;
        desc.bindings = {&binding, 1};
        desc.attributes = attributes;
        desc.cullMode = VK_CULL_MODE_NONE;
        desc.depthWrite = false;
        desc.blend = modes[i];
        desc.name = "particle pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[i] = pipeline.value();
    }
    return {};
}

void ParticleRenderer::destroyPipelines(const vk::Device& device) {
    for (VkPipeline& pipeline : m_pipelines) {
        if (pipeline) {
            vkDestroyPipeline(device.device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}

std::span<ParticleVertex> ParticleRenderer::vertices(u32 buffer) const {
    if (buffer >= kBuffers || !m_vertices[buffer].mapped) {
        return {};
    }
    return {reinterpret_cast<ParticleVertex*>(m_vertices[buffer].mapped), u64{kMaxQuads} * 4};
}

void ParticleRenderer::prepare(const vk::Device& device, u32 slot, u32 buffer, u32 quads) {
    if (slot >= kMaxFrames || buffer >= kBuffers) {
        return;
    }
    quads = std::min(quads, kMaxQuads);
    if (quads > 0) {
        vmaFlushAllocation(device.allocator(), m_vertices[buffer].allocation, 0,
                           u64{quads} * 4 * sizeof(ParticleVertex));
    }
    m_bufferOf[slot] = buffer;
    m_quads[slot] = quads;
}

void ParticleRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot,
                            std::span<const ParticleDraw> draws, const ModelRenderer& textures,
                            RenderStats& stats) const {
    if (draws.empty() || slot >= kMaxFrames || m_quads[slot] == 0 || !m_pipelines[kAlpha]) {
        return;
    }
    VD_PROFILE_ZONE("ParticleRenderer::draw");
    const u32 limit = m_quads[slot];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet, 0, nullptr);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertices[m_bufferOf[slot]].buffer, &zero);
    vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    ParticlePush pushed{};
    bool anyPushed = false;
    for (const ParticleDraw& draw : draws) {
        if (draw.quadCount == 0 || draw.firstQuad + draw.quadCount > limit) {
            continue;
        }
        Pipeline id = kAlpha;
        switch (static_cast<pack::ModelBlend>(draw.blend)) {
        case pack::ModelBlend::Add: id = kAdd; break;
        case pack::ModelBlend::Mod: id = kMod; break;
        case pack::ModelBlend::Mod2x: id = kMod2x; break;
        default: id = kAlpha; break;
        }
        if (m_pipelines[id] != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[id]);
            boundPipeline = m_pipelines[id];
        }
        const VkDescriptorSet set = textures.textureSet(draw.texture.index, true);
        if (set != boundTexture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &set, 0, nullptr);
            boundTexture = set;
        }
        const ParticlePush push{{draw.blend, draw.flags, 0, 0}};
        if (!anyPushed || std::memcmp(&push, &pushed, sizeof(push)) != 0) {
            vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            pushed = push;
            anyPushed = true;
        }
        vkCmdDrawIndexed(cmd, draw.quadCount * 6, 1, draw.firstQuad * 6, 0, 0);
        ++stats.drawCalls;
        ++stats.particleDrawCalls;
        stats.triangles += draw.quadCount * 2;
        stats.particleQuads += draw.quadCount;
    }
}

} // namespace vd
