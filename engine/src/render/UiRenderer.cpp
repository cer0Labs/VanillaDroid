#include "render/UiRenderer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "pack/PackFormat.h"
#include "render/ModelRenderer.h"
#include "render/vk/AstcSoftware.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vd {
namespace {

constexpr u32 kUiVertSpv[] = {
#include "ui.vert.spv.inc"
};
constexpr u32 kUiFragSpv[] = {
#include "ui.frag.spv.inc"
};

struct UiPush {
    f32 scaleOffset[4];
    f32 rotation[4];
    f32 circle[4];
};

constexpr u32 kIconMips = 7; // 64 .. 1

Result<void> createMappedBuffer(const vk::Device& device, u64 size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                VmaAllocation& allocation, u8*& mapped) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &info));
    mapped = static_cast<u8*>(info.pMappedData);
    return {};
}

// A pack texture blob of `layers` identical layers of one texel: a placeholder until ui.vdpk is loaded.
std::vector<u8> solidArrayBlob(pack::TextureFormat format, u32 layers, u8 value) {
    const u32 texel = format == pack::TextureFormat::R8 ? 1u : 4u;
    pack::TextureHeader header{};
    header.format = static_cast<u16>(format);
    header.width = 1;
    header.height = 1;
    header.mipCount = 1;
    header.layers = static_cast<u16>(layers);
    header.mipOffsets[0] = 144; // after the 140-byte header, 16-byte aligned
    header.mipSizes[0] = texel * layers;
    std::vector<u8> blob(header.mipOffsets[0] + header.mipSizes[0], value);
    std::memcpy(blob.data(), &header, sizeof(header));
    return blob;
}

// The icon cache before any icon arrives: kUiIconSlots transparent 64x64 ASTC 4x4 layers with their mips (every
// block a constant-color "void extent" block of transparent black).
std::vector<u8> emptyIconArrayBlob() {
    pack::TextureHeader header{};
    header.format = static_cast<u16>(pack::TextureFormat::Astc4x4);
    header.width = kUiIconSize;
    header.height = kUiIconSize;
    header.mipCount = kIconMips;
    header.layers = kUiIconSlots;
    u32 offset = 144;
    for (u32 level = 0; level < kIconMips; ++level) {
        const u32 side = std::max(1u, kUiIconSize >> level);
        const u32 blocks = (side + 3) / 4;
        header.mipOffsets[level] = offset;
        header.mipSizes[level] = blocks * blocks * 16 * kUiIconSlots;
        offset += header.mipSizes[level];
    }
    std::vector<u8> blob(offset, u8{0});
    std::memcpy(blob.data(), &header, sizeof(header));
    static constexpr u8 kVoidExtent[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    for (u32 at = 144; at + 16 <= offset; at += 16) {
        std::memcpy(blob.data() + at, kVoidExtent, 16);
    }
    return blob;
}

} // namespace

Result<void> UiRenderer::init(const vk::Device& device, VkDescriptorSetLayout textureLayout, u32 framesInFlight) {
    VkDevice dev = device.device();
    m_frames = std::min(framesInFlight, kMaxFrames);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &m_sampler));

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_setLayout));
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_setLayout;
    VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, &m_set));

    const std::array<VkDescriptorSetLayout, 2> setLayouts{m_setLayout, textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UiPush)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_layout));

    // Placeholders until ui.vdpk is loaded, and the (empty) icon cache.
    const std::vector<u8> white = solidArrayBlob(pack::TextureFormat::Rgba8, 1, 255);
    const std::vector<u8> blank = solidArrayBlob(pack::TextureFormat::R8, 1, 0);
    const std::vector<u8> icons = emptyIconArrayBlob();
    Result<vk::Texture> atlas = vk::createTextureFromBlob(device, white.data(), white.size());
    Result<vk::Texture> fonts = vk::createTextureFromBlob(device, blank.data(), blank.size());
    Result<vk::Texture> iconArray = vk::createTextureFromBlob(device, icons.data(), icons.size());
    if (!atlas || !fonts || !iconArray) {
        return Error{"UI: cannot create its textures"};
    }
    m_atlas = atlas.value();
    m_fonts = fonts.value();
    m_icons = iconArray.value();
    writeTextureDescriptors(device);

    for (u32 i = 0; i < m_frames; ++i) {
        VD_TRY(createMappedBuffer(device, u64{kMaxQuads} * 4 * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  m_vertices[i].buffer, m_vertices[i].allocation, m_vertices[i].mapped));
    }
    VD_TRY(createMappedBuffer(device, u64{kMaxQuads} * 6 * sizeof(u16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              m_indices.buffer, m_indices.allocation, m_indices.mapped));
    auto* indices = reinterpret_cast<u16*>(m_indices.mapped);
    for (u32 q = 0; q < kMaxQuads; ++q) {
        const u16 base = static_cast<u16>(q * 4);
        const u16 quad[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                             base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
        std::memcpy(indices + q * 6, quad, sizeof(quad));
    }
    vmaFlushAllocation(device.allocator(), m_indices.allocation, 0, VK_WHOLE_SIZE);
    return {};
}

void UiRenderer::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    destroyPipeline(device);
    for (FrameBuffer& frame : m_vertices) {
        if (frame.buffer) {
            vmaDestroyBuffer(device.allocator(), frame.buffer, frame.allocation);
        }
        frame = {};
    }
    if (m_indices.buffer) {
        vmaDestroyBuffer(device.allocator(), m_indices.buffer, m_indices.allocation);
    }
    m_indices = {};
    vk::destroyTexture(device, m_atlas);
    vk::destroyTexture(device, m_icons);
    vk::destroyTexture(device, m_fonts);
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_layout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
}

Result<void> UiRenderer::createPipeline(const vk::Device& device, VkRenderPass renderPass) {
    const VkVertexInputBindingDescription binding{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> attributes{{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UiVertex, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(UiVertex, params)},
    }};
    vk::GraphicsPipelineDesc desc;
    desc.vertexSpv = kUiVertSpv;
    desc.fragmentSpv = kUiFragSpv;
    desc.layout = m_layout;
    desc.renderPass = renderPass;
    desc.bindings = {&binding, 1};
    desc.attributes = attributes;
    desc.cullMode = VK_CULL_MODE_NONE;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blend = vk::BlendMode::Premultiplied; // ui.frag premultiplies; additive quads write alpha 0
    desc.name = "ui pipeline";
    Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
    if (!pipeline) {
        return pipeline.error();
    }
    m_pipeline = pipeline.value();
    return {};
}

void UiRenderer::destroyPipeline(const vk::Device& device) {
    if (m_pipeline) {
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
}

void UiRenderer::writeTextureDescriptors(const vk::Device& device) {
    const std::array<VkDescriptorImageInfo, 3> images{{
        {m_sampler, m_atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_sampler, m_icons.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_sampler, m_fonts.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = m_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device.device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

Result<void> UiRenderer::setTextures(const vk::Device& device, const u8* atlas, usize atlasSize, const u8* fonts,
                                     usize fontsSize) {
    Result<vk::Texture> newAtlas = vk::createTextureFromBlob(device, atlas, atlasSize);
    if (!newAtlas) {
        return newAtlas.error();
    }
    Result<vk::Texture> newFonts = vk::createTextureFromBlob(device, fonts, fontsSize);
    if (!newFonts) {
        vk::Texture failed = newAtlas.value();
        vk::destroyTexture(device, failed);
        return newFonts.error();
    }
    if (!newAtlas.value().isArray || !newFonts.value().isArray) {
        vk::Texture a = newAtlas.value();
        vk::Texture f = newFonts.value();
        vk::destroyTexture(device, a);
        vk::destroyTexture(device, f);
        return Error{"UI: atlas and fonts must be texture arrays"};
    }
    vkDeviceWaitIdle(device.device()); // the old ones may still be in use
    vk::destroyTexture(device, m_atlas);
    vk::destroyTexture(device, m_fonts);
    m_atlas = newAtlas.value();
    m_fonts = newFonts.value();
    writeTextureDescriptors(device);
    return {};
}

bool UiRenderer::setIcon(const vk::Device& device, vk::Uploader& uploader, u32 slot, VkBuffer src, u64 srcOffset,
                         const u8* blob, usize size) {
    if (slot >= kUiIconSlots || size < sizeof(pack::TextureHeader)) {
        return false;
    }
    pack::TextureHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.format != static_cast<u16>(pack::TextureFormat::Astc4x4) || header.width != kUiIconSize ||
        header.height != kUiIconSize || header.layers != 0 || header.mipCount == 0) {
        return false;
    }
    const u32 mips = std::min<u32>(header.mipCount, kIconMips);
    for (u32 level = 0; level < mips; ++level) {
        if (header.mipOffsets[level] + u64{header.mipSizes[level]} > size) {
            return false;
        }
    }
    std::array<VkBufferImageCopy, kIconMips> regions{};
    if (!m_icons.decodedOnCpu) {
        for (u32 level = 0; level < mips; ++level) {
            const u32 side = std::max(1u, kUiIconSize >> level);
            regions[level].bufferOffset = srcOffset + header.mipOffsets[level];
            regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, slot, 1};
            regions[level].imageExtent = {side, side, 1};
        }
        uploader.updateImageLayer(src, m_icons.image, {regions.data(), mips}, mips, slot);
        return true;
    }
    // Desktop without ASTC: decoded here into the uploader's own ring.
    u64 total = 0;
    for (u32 level = 0; level < mips; ++level) {
        const u32 side = std::max(1u, kUiIconSize >> level);
        total += (u64{side} * side * 4 + 15) & ~u64{15};
    }
    vk::Uploader::Staging staging;
    if (!uploader.reserve(total, staging)) {
        return false;
    }
    u64 at = 0;
    for (u32 level = 0; level < mips; ++level) {
        const u32 side = std::max(1u, kUiIconSize >> level);
        Result<std::vector<u8>> rgba =
            vk::decodeAstcSoftware(blob + header.mipOffsets[level], header.mipSizes[level], side, side, 4);
        if (!rgba) {
            return false;
        }
        std::memcpy(staging.data + at, rgba.value().data(), rgba.value().size());
        regions[level].bufferOffset = staging.offset + at;
        regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, slot, 1};
        regions[level].imageExtent = {side, side, 1};
        at += (u64{side} * side * 4 + 15) & ~u64{15};
    }
    uploader.commit(device, staging);
    uploader.updateImageLayer(uploader.buffer(), m_icons.image, {regions.data(), mips}, mips, slot);
    return true;
}

void UiRenderer::draw(const vk::Device& device, VkCommandBuffer cmd, u32 frameSlot, std::span<const UiVertex> vertices,
                      std::span<const UiDrawCmd> commands, VkExtent2D extent, VkExtent2D logical,
                      const f32 preRotate[4], const ModelRenderer& models, RenderStats& stats) {
    VD_PROFILE_ZONE("UiRenderer::draw");
    if (!m_pipeline || vertices.empty() || commands.empty() || frameSlot >= m_frames || logical.width == 0 ||
        logical.height == 0) {
        return;
    }
    const usize quads = std::min<usize>(vertices.size() / 4, kMaxQuads);
    std::memcpy(m_vertices[frameSlot].mapped, vertices.data(), quads * 4 * sizeof(UiVertex));
    vmaFlushAllocation(device.allocator(), m_vertices[frameSlot].allocation, 0, quads * 4 * sizeof(UiVertex));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertices[frameSlot].buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_set, 0, nullptr);

    UiPush push{};
    push.scaleOffset[0] = 2.0f / static_cast<f32>(logical.width);
    push.scaleOffset[1] = 2.0f / static_cast<f32>(logical.height);
    push.scaleOffset[2] = -1.0f;
    push.scaleOffset[3] = -1.0f;
    std::memcpy(push.rotation, preRotate, sizeof(push.rotation));

    // A logical rectangle -> the framebuffer rectangle it covers once pre-rotated.
    auto scissorOf = [&](const UiDrawCmd& command) {
        if (command.clipWidth <= 0.0f || command.clipHeight <= 0.0f) {
            return VkRect2D{{0, 0}, extent};
        }
        f32 minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        const f32 xs[2] = {command.clipX, command.clipX + command.clipWidth};
        const f32 ys[2] = {command.clipY, command.clipY + command.clipHeight};
        for (f32 x : xs) {
            for (f32 y : ys) {
                const f32 nx = x * push.scaleOffset[0] - 1.0f;
                const f32 ny = y * push.scaleOffset[1] - 1.0f;
                const f32 rx = preRotate[0] * nx + preRotate[2] * ny;
                const f32 ry = preRotate[1] * nx + preRotate[3] * ny;
                const f32 px = (rx + 1.0f) * 0.5f * static_cast<f32>(extent.width);
                const f32 py = (ry + 1.0f) * 0.5f * static_cast<f32>(extent.height);
                minX = std::min(minX, px);
                minY = std::min(minY, py);
                maxX = std::max(maxX, px);
                maxY = std::max(maxY, py);
            }
        }
        const i32 x0 = std::clamp(static_cast<i32>(std::floor(minX)), 0, static_cast<i32>(extent.width));
        const i32 y0 = std::clamp(static_cast<i32>(std::floor(minY)), 0, static_cast<i32>(extent.height));
        const i32 x1 = std::clamp(static_cast<i32>(std::ceil(maxX)), 0, static_cast<i32>(extent.width));
        const i32 y1 = std::clamp(static_cast<i32>(std::ceil(maxY)), 0, static_cast<i32>(extent.height));
        return VkRect2D{{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
    };

    VkRect2D boundScissor{{-1, -1}, {0, 0}};
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    bool pushed = false;
    for (const UiDrawCmd& command : commands) {
        if (command.quadCount == 0 || command.firstQuad >= quads) {
            continue;
        }
        const u32 count = static_cast<u32>(std::min<usize>(command.quadCount, quads - command.firstQuad));
        const VkRect2D scissor = scissorOf(command);
        if (scissor.extent.width == 0 || scissor.extent.height == 0) {
            continue;
        }
        if (std::memcmp(&scissor, &boundScissor, sizeof(scissor)) != 0) {
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            boundScissor = scissor;
        }
        const VkDescriptorSet texture = models.textureSet(command.texture.index, true);
        if (texture != boundTexture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &texture, 0, nullptr);
            boundTexture = texture;
        }
        if (!pushed || command.circleRadius > 0.0f || push.circle[2] > 0.0f) {
            push.circle[0] = command.circleX;
            push.circle[1] = command.circleY;
            push.circle[2] = command.circleRadius;
            vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            pushed = true;
        }
        vkCmdDrawIndexed(cmd, count * 6, 1, command.firstQuad * 6, 0, 0);
        ++stats.drawCalls;
        ++stats.uiDrawCalls;
        stats.triangles += count * 2;
        stats.uiQuads += count;
    }
    const VkRect2D full{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &full);
}

} // namespace vd
