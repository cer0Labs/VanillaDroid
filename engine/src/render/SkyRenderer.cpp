#include "render/SkyRenderer.h"

#include "core/Profile.h"
#include "pack/ModelFormat.h"
#include "pack/SkyFormat.h"
#include "render/ModelRenderer.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"

#include <cstddef>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kSkyVertSpv[] = {
#include "sky.vert.spv.inc"
};
constexpr u32 kSkyFragSpv[] = {
#include "sky.frag.spv.inc"
};
constexpr u32 kSkyCloudsVertSpv[] = {
#include "sky_clouds.vert.spv.inc"
};
constexpr u32 kSkyCloudsFragSpv[] = {
#include "sky_clouds.frag.spv.inc"
};
constexpr u32 kSkySpriteVertSpv[] = {
#include "sky_sprite.vert.spv.inc"
};
constexpr u32 kSkySpriteFragSpv[] = {
#include "sky_sprite.frag.spv.inc"
};
constexpr u32 kSkyModelVertSpv[] = {
#include "sky_model.vert.spv.inc"
};
constexpr u32 kSkyModelFragSpv[] = {
#include "sky_model.frag.spv.inc"
};

// Must match the push_constant blocks of shaders/sky_clouds.*, sky_sprite.* and sky_model.*: vec4s whose
// meaning depends on the draw (see each shader; the sprites and the clouds read the first four).
struct SkyPush {
    f32 a[4];
    f32 b[4];
    f32 c[4];
    f32 d[4];
    f32 e[4];
    f32 f[4];
};
static_assert(sizeof(SkyPush) == 96, "SkyPush layout");

void set(f32 (&out)[4], const glm::vec3& v, f32 w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

void set(f32 (&out)[4], const glm::vec4& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = v.w;
}

} // namespace

Result<void> SkyRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                               VkDescriptorSetLayout textureLayout) {
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &frameLayout;
    VD_VK_TRY(vkCreatePipelineLayout(device.device(), &info, nullptr, &m_layout));

    const std::array<VkDescriptorSetLayout, 2> setLayouts{frameLayout, textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(SkyPush)};
    info.setLayoutCount = static_cast<u32>(setLayouts.size());
    info.pSetLayouts = setLayouts.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(device.device(), &info, nullptr, &m_texturedLayout));
    return {};
}

void SkyRenderer::shutdown(const vk::Device& device) {
    destroyPipeline(device);
    for (Model& model : m_skyModels) {
        if (model.buffer) {
            vmaDestroyBuffer(device.allocator(), model.buffer, model.allocation);
        }
    }
    m_skyModels.assign(1, Model{});
    if (m_texturedLayout) {
        vkDestroyPipelineLayout(device.device(), m_texturedLayout, nullptr);
        m_texturedLayout = VK_NULL_HANDLE;
    }
    if (m_layout) {
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

Result<void> SkyRenderer::createPipeline(const vk::Device& device, VkRenderPass renderPass) {
    auto create = [&](const vk::GraphicsPipelineDesc& desc, VkPipeline& out) -> Result<void> {
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        out = pipeline.value();
        return {};
    };
    vk::GraphicsPipelineDesc desc;
    desc.renderPass = renderPass;
    desc.cullMode = VK_CULL_MODE_NONE; // seen from inside
    desc.depthWrite = false;

    desc.vertexSpv = kSkyVertSpv;
    desc.fragmentSpv = kSkyFragSpv;
    desc.layout = m_layout;
    desc.name = "sky pipeline";
    VD_TRY(create(desc, m_pipeline));

    // Phase 11: the clouds over the dome, the sprites (sun, moons) added at the far plane and over the scene
    // (the glares), and the sky models.
    desc.layout = m_texturedLayout;
    desc.vertexSpv = kSkyCloudsVertSpv;
    desc.fragmentSpv = kSkyCloudsFragSpv;
    desc.blend = vk::BlendMode::Alpha;
    desc.name = "sky clouds pipeline";
    VD_TRY(create(desc, m_clouds));

    desc.vertexSpv = kSkySpriteVertSpv;
    desc.fragmentSpv = kSkySpriteFragSpv;
    desc.blend = vk::BlendMode::Additive;
    desc.name = "sky sprite pipeline";
    VD_TRY(create(desc, m_sprite));
    desc.depthTest = false;
    desc.name = "sky glare pipeline";
    VD_TRY(create(desc, m_glare));
    desc.depthTest = true;

    const VkVertexInputBindingDescription binding{0, sizeof(pack::SkyVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(pack::SkyVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pack::SkyVertex, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(pack::SkyVertex, color)},
    }};
    desc.bindings = {&binding, 1};
    desc.attributes = attributes;
    desc.vertexSpv = kSkyModelVertSpv;
    desc.fragmentSpv = kSkyModelFragSpv;
    const vk::BlendMode modes[kModelPipelineCount] = {vk::BlendMode::Opaque, vk::BlendMode::Alpha,
                                                      vk::BlendMode::Additive};
    for (u32 i = 0; i < kModelPipelineCount; ++i) {
        desc.blend = modes[i];
        desc.name = "sky model pipeline";
        VD_TRY(create(desc, m_models[i]));
    }
    return {};
}

void SkyRenderer::destroyPipeline(const vk::Device& device) {
    auto destroy = [&](VkPipeline& pipeline) {
        if (pipeline) {
            vkDestroyPipeline(device.device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    };
    destroy(m_pipeline);
    destroy(m_clouds);
    destroy(m_sprite);
    destroy(m_glare);
    for (VkPipeline& pipeline : m_models) {
        destroy(pipeline);
    }
}

Result<u32> SkyRenderer::createModel(const vk::Device& device, const u8* blob, usize size,
                                     std::span<const u32> partTextures) {
    if (size < sizeof(pack::SkyModelHeader)) {
        return Error{"Sky model blob too small"};
    }
    pack::SkyModelHeader header;
    std::memcpy(&header, blob, sizeof(header));
    const u64 vertexBytes = u64{header.vertexCount} * sizeof(pack::SkyVertex);
    const u64 indexBytes = u64{header.indexCount} * sizeof(u16);
    if (header.version != pack::kSkyModelFormatVersion || header.verticesOffset + vertexBytes > size ||
        header.indicesOffset + indexBytes > size ||
        header.partsOffset + u64{header.partCount} * sizeof(pack::SkyPart) > size || vertexBytes == 0 ||
        indexBytes == 0) {
        return Error{"Bad sky model blob"};
    }
    // A few KB written once into mapped memory (device-local where the GPU has it, as on phones): no transfer
    // to schedule, so nothing a map change drops (Uploader::discardPending) can lose it.
    Model model;
    model.indexOffset = (vertexBytes + 15) / 16 * 16;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = model.indexOffset + indexBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &model.buffer, &model.allocation,
                              &info));
    auto* mapped = static_cast<u8*>(info.pMappedData);
    std::memcpy(mapped, blob + header.verticesOffset, vertexBytes);
    std::memcpy(mapped + model.indexOffset, blob + header.indicesOffset, indexBytes);
    vmaFlushAllocation(device.allocator(), model.allocation, 0, VK_WHOLE_SIZE);

    for (u32 i = 0; i < header.partCount; ++i) {
        pack::SkyPart part;
        std::memcpy(&part, blob + header.partsOffset + i * sizeof(part), sizeof(part));
        if (u64{part.firstIndex} + part.indexCount > header.indexCount) {
            continue;
        }
        model.parts.push_back({part.firstIndex, part.indexCount,
                               i < partTextures.size() ? partTextures[i] : 0u, part.blend,
                               (part.flags & pack::kSkyPartClamp) != 0});
    }
    m_skyModels.push_back(std::move(model));
    return static_cast<u32>(m_skyModels.size() - 1);
}

void SkyRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, const SkyFrame& sky,
                       const ModelRenderer& textures, RenderStats& stats) const {
    if (!m_pipeline) {
        return;
    }
    VD_PROFILE_ZONE("SkyRenderer::draw");
    if (sky.dome) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet, 0, nullptr);
        vkCmdDraw(cmd, kVertexCount, 1, 0, 0);
        ++stats.drawCalls;
        stats.triangles += kVertexCount / 3;
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedLayout, 0, 1, &frameSet, 0,
                            nullptr);
    if (sky.dome) {
        // In 1.12 the stars, the sun and the moons go on a black screen and the dome is added over them: here
        // the dome is already there, so they are added over it, which comes to the same.
        if (sky.stars.model.valid() && sky.stars.alpha > 0.0f) {
            drawModel(cmd, sky.stars, sky.starMasks.data(), textures, stats);
        }
        if (sky.bodyCount > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sprite);
            for (u32 i = 0; i < sky.bodyCount && i < kMaxSkyBodies; ++i) {
                drawSprite(cmd, sky.bodies[i], true, textures, stats);
            }
        }
        if (sky.clouds.valid()) {
            SkyPush push{};
            set(push.a, sky.cloudDrift);
            set(push.b, sky.cloudShape);
            set(push.c, sky.cloudLight, 0.0f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_clouds);
            const VkDescriptorSet set = textures.textureSet(sky.clouds.index, false);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedLayout, 1, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, m_texturedLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                               &push);
            vkCmdDraw(cmd, kCloudVertexCount, 1, 0, 0);
            ++stats.drawCalls;
            stats.triangles += kCloudVertexCount / 3;
        }
    }
    if (sky.skybox.model.valid() && sky.skybox.alpha > 0.0f) {
        drawModel(cmd, sky.skybox, nullptr, textures, stats);
    }
}

void SkyRenderer::drawGlare(VkCommandBuffer cmd, VkDescriptorSet frameSet, const SkyFrame& sky,
                            const ModelRenderer& textures, RenderStats& stats) const {
    if (!m_glare || sky.glareCount == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glare);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedLayout, 0, 1, &frameSet, 0,
                            nullptr);
    for (u32 i = 0; i < sky.glareCount && i < kMaxSkyGlares; ++i) {
        drawSprite(cmd, sky.glares[i], false, textures, stats);
    }
}

void SkyRenderer::drawSprite(VkCommandBuffer cmd, const SkySprite& sprite, bool horizonFade,
                             const ModelRenderer& textures, RenderStats& stats) const {
    if (sprite.color.w <= 0.0f) {
        return;
    }
    SkyPush push{};
    set(push.a, sprite.center, horizonFade ? 1.0f : 0.0f);
    set(push.b, sprite.right, 0.0f);
    set(push.c, sprite.up, 0.0f);
    set(push.d, sprite.color);
    const VkDescriptorSet set = textures.textureSet(sprite.texture.index, true);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedLayout, 1, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_texturedLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    ++stats.drawCalls;
    stats.triangles += 2;
}

void SkyRenderer::drawModel(VkCommandBuffer cmd, const SkyModelDraw& draw, const glm::vec4* starMasks,
                            const ModelRenderer& textures, RenderStats& stats) const {
    const bool additive = starMasks != nullptr;
    if (draw.model.index >= m_skyModels.size()) {
        return;
    }
    const Model& model = m_skyModels[draw.model.index];
    // Model -> world rotation (the root bone's), as three rows.
    const f32 x = draw.rotation.x, y = draw.rotation.y, z = draw.rotation.z, w = draw.rotation.w;
    SkyPush push{};
    set(push.a, glm::vec3(1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y)),
        0.0f);
    set(push.b, glm::vec3(2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x)),
        0.0f);
    set(push.c, glm::vec3(2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y)),
        0.0f);
    push.d[1] = additive ? 1.0f : 0.0f;
    if (additive) {
        set(push.e, starMasks[0]);
        set(push.f, starMasks[1]);
    }
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &model.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, model.buffer, model.indexOffset, VK_INDEX_TYPE_UINT16);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Part& part : model.parts) {
        u32 pipeline = kModelAdditive;
        if (!additive) {
            const auto blend = static_cast<pack::ModelBlend>(part.blend);
            pipeline = blend == pack::ModelBlend::Opaque ? kModelOpaque
                       : blend == pack::ModelBlend::Add  ? kModelAdditive
                                                         : kModelAlpha; // alpha key, alpha, mod: blended
        }
        if (m_models[pipeline] != bound) {
            bound = m_models[pipeline];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        }
        push.d[0] = draw.alpha;
        const VkDescriptorSet set = textures.textureSet(part.texture, part.clamp);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedLayout, 1, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, m_texturedLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, part.indexCount, 1, part.firstIndex, 0, 0);
        ++stats.drawCalls;
        stats.triangles += part.indexCount / 3;
    }
}

} // namespace vd
