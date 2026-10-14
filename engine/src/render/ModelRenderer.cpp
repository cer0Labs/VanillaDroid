#include "render/ModelRenderer.h"

#include "core/Log.h"
#include "pack/ActorFormat.h"
#include "pack/ModelFormat.h"
#include "pack/PackFormat.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kModelVertSpv[] = {
#include "model.vert.spv.inc"
};
constexpr u32 kModelFragSpv[] = {
#include "model.frag.spv.inc"
};
constexpr u32 kModelAlphaKeyFragSpv[] = {
#include "model_alphakey.frag.spv.inc"
};
constexpr u32 kModelAlphaKeyCoverageFragSpv[] = { // phase 18 (world): with alpha to coverage
#include "model_alphakey_a2c.frag.spv.inc"
};
constexpr u32 kModelLiquidVertSpv[] = {
#include "model_liquid.vert.spv.inc"
};
constexpr u32 kLiquidFragSpv[] = {
#include "liquid.frag.spv.inc"
};
constexpr u32 kActorVertSpv[] = {
#include "actor.vert.spv.inc"
};
constexpr u32 kDoodadVertSpv[] = { // phase 14
#include "doodad.vert.spv.inc"
};

// Must match the push_constant block in shaders/model_push.glsl.
struct ModelPush {
    u32 flags[4];
    f32 ambient[4];
    f32 color[4];
    f32 uv[8]; // texture animation rows (identity when static)
    f32 selfLight[8]; // phase 11: the M2's own point light (ModelDraw::selfLight); WMO: color = night glow
};
static_assert(sizeof(ModelPush) <= 128, "push constants: the guaranteed 128 bytes");
static_assert(sizeof(UvTransform) == sizeof(ModelPush::uv), "UV transform rows");

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// The rows of a material's texture animation this frame, or the identity.
void uvRows(std::span<const UvTransform> transforms, u32 first, u16 uvAnimation, f32 (&out)[8]) {
    static constexpr UvTransform kIdentity{};
    const UvTransform* t = &kIdentity;
    if (uvAnimation != 0 && first != ModelDraw::kNoUvTransforms && u64{first} + uvAnimation <= transforms.size()) {
        t = &transforms[first + uvAnimation - 1];
    }
    std::memcpy(out, t->rows, sizeof(out));
}

static_assert(sizeof(ModelInstanceData) == 64, "ModelInstanceData must match the instance attributes");
static_assert(sizeof(ActorInstanceData) == 64, "ActorInstanceData must match the actor instance attributes");
static_assert(kActorTextureSlots == pack::kActorTextureSlots, "actor texture slots");
static_assert(sizeof(anim::BoneMatrix) == 48, "bone matrix: three vec4 rows");

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

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

Result<void> ModelRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                                 VkDescriptorSetLayout liquidLayout, u32 framesInFlight) {
    VkDevice dev = device.device();
    m_frames = std::min(framesInFlight, kMaxFrames);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &m_repeatSampler));
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &m_clampSampler));

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_textureLayout));

    const std::array<VkDescriptorSetLayout, 2> setLayouts{frameLayout, m_textureLayout};
    const VkPushConstantRange push{kPushStages, 0, sizeof(ModelPush)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));
    const std::array<VkDescriptorSetLayout, 2> liquidSetLayouts{frameLayout, liquidLayout};
    pipelineLayoutInfo.pSetLayouts = liquidSetLayouts.data();
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_liquidLayout));

    // Actors: set 2 is the frame's bone palette, read by the vertex shader.
    VkDescriptorSetLayoutBinding boneBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr};
    layoutInfo.pBindings = &boneBinding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_boneLayout));
    const std::array<VkDescriptorSetLayout, 3> actorSetLayouts{frameLayout, m_textureLayout, m_boneLayout};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(actorSetLayouts.size());
    pipelineLayoutInfo.pSetLayouts = actorSetLayouts.data();
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_actorLayout));

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kMaxTextures + 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kMaxFrames}, // the actors' and (phase 14) the doodads' palettes
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 2 * kMaxTextures + 2 + 2 * kMaxFrames;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));
    m_textureSets.resize(kMaxTextures + 1, {VK_NULL_HANDLE, VK_NULL_HANDLE});
    m_textureViews.resize(kMaxTextures + 1, VK_NULL_HANDLE); // phase 18 (world)

    // The game writes the palettes (host-visible, a ring over submitted frames); each frame copies its own into
    // GPU memory, which is what the vertex shader reads.
    for (u32 i = 0; i < kBoneBuffers; ++i) {
        VD_TRY(createMappedBuffer(device, u64{kMaxBones} * sizeof(anim::BoneMatrix), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  m_bones[i].buffer, m_bones[i].allocation, m_bones[i].mapped));
    }
    for (u32 i = 0; i < m_frames; ++i) {
        VkBufferCreateInfo gpuInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        gpuInfo.size = u64{kMaxBones} * sizeof(anim::BoneMatrix);
        gpuInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo gpuAlloc{};
        gpuAlloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &gpuInfo, &gpuAlloc, &m_gpuBones[i], &m_gpuBoneMemory[i], nullptr));
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = m_pool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &m_boneLayout;
        VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, &m_gpuBoneSets[i]));
        const VkDescriptorBufferInfo bufferDescriptor{m_gpuBones[i], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_gpuBoneSets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferDescriptor;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
    // Phase 14: the animated doodads' palette, the same way.
    for (u32 i = 0; i < kBoneBuffers; ++i) {
        VD_TRY(createMappedBuffer(device, u64{kMaxDoodadBones} * sizeof(anim::BoneMatrix),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, m_doodadBones[i].buffer, m_doodadBones[i].allocation,
                                  m_doodadBones[i].mapped));
    }
    for (u32 i = 0; i < m_frames; ++i) {
        VkBufferCreateInfo gpuInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        gpuInfo.size = u64{kMaxDoodadBones} * sizeof(anim::BoneMatrix);
        gpuInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo gpuAlloc{};
        gpuAlloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &gpuInfo, &gpuAlloc, &m_gpuDoodadBones[i],
                                  &m_gpuDoodadBoneMemory[i], nullptr));
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = m_pool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &m_boneLayout;
        VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, &m_gpuDoodadBoneSets[i]));
        const VkDescriptorBufferInfo bufferDescriptor{m_gpuDoodadBones[i], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_gpuDoodadBoneSets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferDescriptor;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
    for (u32 i = 0; i < m_frames; ++i) {
        VD_TRY(createMappedBuffer(device, u64{kMaxActorInstances} * sizeof(ActorInstanceData),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_actorInstances[i].buffer,
                                  m_actorInstances[i].allocation, m_actorInstances[i].mapped));
    }

    for (u32 i = 0; i < m_frames; ++i) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = u64{kMaxInstances} * sizeof(ModelInstanceData);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_instances[i].buffer,
                                  &m_instances[i].allocation, &info));
        m_instances[i].mapped = static_cast<u8*>(info.pMappedData);
    }

    // White 1x1 texture for materials whose texture is replaceable or still streaming.
    struct {
        pack::TextureHeader header;
        u8 pad[4];
        u8 texel[4];
    } white{};
    white.header.format = static_cast<u16>(pack::TextureFormat::Rgba8);
    white.header.width = 1;
    white.header.height = 1;
    white.header.mipCount = 1;
    white.header.mipOffsets[0] = offsetof(decltype(white), texel);
    white.header.mipSizes[0] = 4;
    std::memset(white.texel, 255, sizeof(white.texel));
    Result<vk::Texture> placeholder =
        vk::createTextureFromBlob(device, reinterpret_cast<const u8*>(&white), sizeof(white));
    if (!placeholder) {
        return placeholder.error();
    }
    m_placeholder = placeholder.value();
    registerTexture(device, 0, m_placeholder.view);

    m_models.reserve(1024);
    m_retired.reserve(1024);
    return {};
}

void ModelRenderer::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    collect(device, 0, true);
    for (Mesh& mesh : m_meshes) {
        if (mesh.buffer) {
            vmaDestroyBuffer(device.allocator(), mesh.buffer, mesh.allocation);
        }
    }
    for (u32 id = 1; id < m_models.size(); ++id) {
        if (m_models[id].buffer) {
            release(device, id);
        }
    }
    destroyPipelines(device);
    for (Buffer* buffers : {m_instances.data(), m_actorInstances.data()}) {
        for (u32 i = 0; i < kMaxFrames; ++i) {
            if (buffers[i].buffer) {
                vmaDestroyBuffer(device.allocator(), buffers[i].buffer, buffers[i].allocation);
            }
        }
    }
    for (Buffer& buffer : m_bones) {
        if (buffer.buffer) {
            vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
        }
    }
    for (u32 i = 0; i < kMaxFrames; ++i) {
        if (m_gpuBones[i]) {
            vmaDestroyBuffer(device.allocator(), m_gpuBones[i], m_gpuBoneMemory[i]);
        }
    }
    for (Buffer& buffer : m_doodadBones) { // phase 14
        if (buffer.buffer) {
            vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
        }
    }
    for (u32 i = 0; i < kMaxFrames; ++i) {
        if (m_gpuDoodadBones[i]) {
            vmaDestroyBuffer(device.allocator(), m_gpuDoodadBones[i], m_gpuDoodadBoneMemory[i]);
        }
    }
    vk::destroyTexture(device, m_placeholder);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_liquidLayout) vkDestroyPipelineLayout(dev, m_liquidLayout, nullptr);
    if (m_actorLayout) vkDestroyPipelineLayout(dev, m_actorLayout, nullptr);
    if (m_boneLayout) vkDestroyDescriptorSetLayout(dev, m_boneLayout, nullptr);
    if (m_textureLayout) vkDestroyDescriptorSetLayout(dev, m_textureLayout, nullptr);
    if (m_clampSampler) vkDestroySampler(dev, m_clampSampler, nullptr);
    if (m_repeatSampler) vkDestroySampler(dev, m_repeatSampler, nullptr);
    *this = ModelRenderer{};
}

Result<void> ModelRenderer::createPipelines(const vk::Device& device, VkRenderPass renderPass) {
    const std::array<VkVertexInputBindingDescription, 2> bindings{{
        {0, sizeof(pack::ModelVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(ModelInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 9> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(pack::ModelVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(pack::ModelVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pack::ModelVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(pack::ModelVertex, color)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {7, 1, VK_FORMAT_R32G32_UINT, offsetof(ModelInstanceData, light)}, // phase 11
        {8, 1, VK_FORMAT_R32_SFLOAT, offsetof(ModelInstanceData, fade)},   // phase 14
    }};
    struct Variant {
        Pipeline id;
        bool alphaKey;
        VkCullModeFlags cull;
        vk::BlendMode blend;
    };
    const Variant variants[] = {
        {kOpaque, false, VK_CULL_MODE_BACK_BIT, vk::BlendMode::Opaque},
        {kOpaqueTwoSided, false, VK_CULL_MODE_NONE, vk::BlendMode::Opaque},
        {kAlphaKey, true, VK_CULL_MODE_BACK_BIT, vk::BlendMode::Opaque},
        {kAlphaKeyTwoSided, true, VK_CULL_MODE_NONE, vk::BlendMode::Opaque},
        {kAlpha, false, VK_CULL_MODE_NONE, vk::BlendMode::Alpha},
        {kAdd, false, VK_CULL_MODE_NONE, vk::BlendMode::Additive},
        {kMod, false, VK_CULL_MODE_NONE, vk::BlendMode::Modulate},
        {kMod2x, false, VK_CULL_MODE_NONE, vk::BlendMode::Modulate2x},
    };
    for (const Variant& v : variants) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kModelVertSpv;
        desc.fragmentSpv = v.alphaKey ? std::span<const u32>(kModelAlphaKeyFragSpv) : std::span<const u32>(kModelFragSpv);
        desc.layout = m_pipelineLayout;
        desc.renderPass = renderPass;
        desc.bindings = bindings;
        desc.attributes = attributes;
        desc.cullMode = v.cull;
        desc.blend = v.blend;
        desc.depthWrite = v.blend == vk::BlendMode::Opaque;
        desc.alphaToCoverage = v.alphaKey && alphaToCoverage; // phase 18 (world): foliage's edges with multisampling
        desc.fragmentCoverageSpv = kModelAlphaKeyCoverageFragSpv;
        desc.name = "model pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[v.id] = pipeline.value();
    }
    // Actors: the same eight variants over skinned vertices.
    const std::array<VkVertexInputBindingDescription, 2> actorBindings{{
        {0, sizeof(pack::ActorVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(ActorInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 9> actorAttributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(pack::ActorVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(pack::ActorVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pack::ActorVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(pack::ActorVertex, weights)},
        {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(pack::ActorVertex, bones)},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {8, 1, VK_FORMAT_R32G32B32A32_UINT, 48},
    }};
    for (const Variant& v : variants) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kActorVertSpv;
        desc.fragmentSpv = v.alphaKey ? std::span<const u32>(kModelAlphaKeyFragSpv) : std::span<const u32>(kModelFragSpv);
        desc.layout = m_actorLayout;
        desc.renderPass = renderPass;
        desc.bindings = actorBindings;
        desc.attributes = actorAttributes;
        desc.cullMode = v.cull;
        desc.blend = v.blend;
        desc.depthWrite = v.blend == vk::BlendMode::Opaque;
        desc.alphaToCoverage = v.alphaKey && alphaToCoverage; // phase 18 (world)
        desc.fragmentCoverageSpv = kModelAlphaKeyCoverageFragSpv;
        desc.name = "actor pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[kActorOpaque + v.id] = pipeline.value();
    }
    // Phase 11: the opaque and alpha-tested ones once more for translucent units, alpha blended without writing depth
    // (the client switches a fading M2's opaque batches to its alpha blend mode, which does not write depth either).
    for (const Variant& v : variants) {
        if (v.blend != vk::BlendMode::Opaque) {
            continue;
        }
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kActorVertSpv;
        desc.fragmentSpv = v.alphaKey ? std::span<const u32>(kModelAlphaKeyFragSpv) : std::span<const u32>(kModelFragSpv);
        desc.layout = m_actorLayout;
        desc.renderPass = renderPass;
        desc.bindings = actorBindings;
        desc.attributes = actorAttributes;
        desc.cullMode = v.cull;
        desc.blend = vk::BlendMode::Alpha;
        desc.depthWrite = false;
        desc.name = "actor fade pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[kActorFade + v.id] = pipeline.value();
    }
    // Phase 14: the same eight over animated doodads: the model's vertices, its skin stream (binding 2) and the bones
    // of the doodad palette (set 2, as the actors').
    const std::array<VkVertexInputBindingDescription, 3> doodadBindings{{
        {0, sizeof(pack::ModelVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(ModelInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
        {2, sizeof(pack::ModelSkinVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    }};
    std::array<VkVertexInputAttributeDescription, 12> doodadAttributes{};
    std::copy(attributes.begin(), attributes.end(), doodadAttributes.begin());
    doodadAttributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(ModelInstanceData, boneBase)};
    doodadAttributes[10] = {10, 2, VK_FORMAT_R8G8B8A8_UINT, offsetof(pack::ModelSkinVertex, bones)};
    doodadAttributes[11] = {11, 2, VK_FORMAT_R8G8B8A8_UNORM, offsetof(pack::ModelSkinVertex, weights)};
    for (const Variant& v : variants) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kDoodadVertSpv;
        desc.fragmentSpv = v.alphaKey ? std::span<const u32>(kModelAlphaKeyFragSpv) : std::span<const u32>(kModelFragSpv);
        desc.layout = m_actorLayout;
        desc.renderPass = renderPass;
        desc.bindings = doodadBindings;
        desc.attributes = doodadAttributes;
        desc.cullMode = v.cull;
        desc.blend = v.blend;
        desc.depthWrite = v.blend == vk::BlendMode::Opaque;
        desc.alphaToCoverage = v.alphaKey && alphaToCoverage; // phase 18 (world)
        desc.fragmentCoverageSpv = kModelAlphaKeyCoverageFragSpv;
        desc.name = "doodad pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[kDoodadOpaque + v.id] = pipeline.value();
    }
    // WMO liquids: seen from above and below, like the terrain's.
    for (const Pipeline id : {kLiquidOpaque, kLiquid}) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = kModelLiquidVertSpv;
        desc.fragmentSpv = kLiquidFragSpv;
        desc.layout = m_liquidLayout;
        desc.renderPass = renderPass;
        desc.bindings = bindings;
        desc.attributes = std::span(attributes).first(7); // not the instance light (phase 11): liquids are unlit
        desc.cullMode = VK_CULL_MODE_NONE;
        desc.blend = id == kLiquid ? vk::BlendMode::Alpha : vk::BlendMode::Opaque;
        desc.depthWrite = id == kLiquidOpaque;
        desc.name = "model liquid pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[id] = pipeline.value();
    }
    return {};
}

void ModelRenderer::destroyPipelines(const vk::Device& device) {
    for (VkPipeline& pipeline : m_pipelines) {
        if (pipeline) {
            vkDestroyPipeline(device.device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}

void ModelRenderer::registerTexture(const vk::Device& device, u32 texture, VkImageView view) {
    if (texture >= m_textureSets.size()) {
        return; // beyond kMaxTextures: drawn with the placeholder
    }
    unregisterTexture(device, texture);
    const std::array<VkDescriptorSetLayout, 2> layouts{m_textureLayout, m_textureLayout};
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, 2> sets{};
    if (vkAllocateDescriptorSets(device.device(), &allocInfo, sets.data()) != VK_SUCCESS) {
        VD_LOG_WARN("Model texture descriptors exhausted");
        return;
    }
    const std::array<VkDescriptorImageInfo, 2> images{{
        {m_repeatSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_clampSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = sets[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device.device(), 2, writes.data(), 0, nullptr);
    m_textureSets[texture] = sets;
    m_textureViews[texture] = view; // phase 18 (world): rewritten with other samplers (setAnisotropy)
}

void ModelRenderer::setAnisotropy(const vk::Device& device, u32 level) {
    VkDevice dev = device.device();
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.anisotropyEnable = level > 1 ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = static_cast<f32>(level);
    VkSampler repeat = VK_NULL_HANDLE;
    VkSampler clamp = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &repeat) != VK_SUCCESS) {
        return;
    }
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &clamp) != VK_SUCCESS) {
        vkDestroySampler(dev, repeat, nullptr);
        return;
    }
    vkDestroySampler(dev, m_repeatSampler, nullptr);
    vkDestroySampler(dev, m_clampSampler, nullptr);
    m_repeatSampler = repeat;
    m_clampSampler = clamp;
    for (u32 texture = 0; texture < m_textureSets.size(); ++texture) {
        if (!m_textureSets[texture][0] || !m_textureViews[texture]) {
            continue;
        }
        const std::array<VkDescriptorImageInfo, 2> images{{
            {m_repeatSampler, m_textureViews[texture], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {m_clampSampler, m_textureViews[texture], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < 2; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = m_textureSets[texture][i];
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(dev, 2, writes.data(), 0, nullptr);
    }
}

void ModelRenderer::unregisterTexture(const vk::Device& device, u32 texture) {
    if (texture < m_textureSets.size() && m_textureSets[texture][0]) {
        vkFreeDescriptorSets(device.device(), m_pool, 2, m_textureSets[texture].data());
        m_textureSets[texture] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        m_textureViews[texture] = VK_NULL_HANDLE;
    }
}

Result<u32> ModelRenderer::createModel(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 srcOffset,
                                       const u8* blob, usize size) {
    pack::ModelHeader header;
    if (size < sizeof(header)) {
        return Error{"Model blob too small"};
    }
    std::memcpy(&header, blob, sizeof(header));
    const u64 vertexBytes = u64{header.vertexCount} * sizeof(pack::ModelVertex);
    const bool wideIndices = (header.flags & pack::kModelIndices32) != 0;
    const u64 indexBytes = u64{header.indexCount} * (wideIndices ? 4 : 2);
    if (header.version != pack::kModelFormatVersion || header.verticesOffset + vertexBytes > size ||
        header.indicesOffset + indexBytes > size ||
        header.materialsOffset + u64{header.materialCount} * sizeof(pack::ModelMaterial) > size ||
        header.partsOffset + u64{header.partCount} * sizeof(pack::ModelPart) > size ||
        header.groupsOffset + u64{header.groupCount} * sizeof(pack::ModelGroup) > size) {
        return Error{"Corrupt model blob (re-bake)"};
    }

    Model model;
    model.indexType = wideIndices ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    model.materials.resize(header.materialCount);
    const auto* materials = reinterpret_cast<const pack::ModelMaterial*>(blob + header.materialsOffset);
    for (u32 i = 0; i < header.materialCount; ++i) {
        model.materials[i].flags = materials[i].flags;
        model.materials[i].blend = materials[i].blend;
        model.materials[i].uvAnimation = materials[i].uvAnimation;
        // Phase 11: SIDN materials (windows, lamp glass) glow at night with their emissive color.
        model.materials[i].emissive = (materials[i].flags & pack::kModelMaterialNightGlow) ? materials[i].color : 0u;
        if (materials[i].flags & pack::kModelMaterialTinted) { // an M2 batch's own color and opacity
            const u32 c = materials[i].color;
            for (u32 k = 0; k < 4; ++k) {
                model.materials[i].color[k] = static_cast<f32>((c >> (8 * k)) & 0xFF) / 255.0f;
            }
        }
    }
    const auto* parts = reinterpret_cast<const pack::ModelPart*>(blob + header.partsOffset);
    model.parts.reserve(header.partCount);
    for (u32 i = 0; i < header.partCount; ++i) {
        const pack::ModelPart& p = parts[i];
        if (p.material >= header.materialCount || p.group >= header.groupCount ||
            u64{p.firstIndex} + p.indexCount > header.indexCount) {
            return Error{"Corrupt model part"};
        }
        const Material& m = model.materials[p.material];
        const bool twoSided = (m.flags & pack::kModelMaterialTwoSided) != 0;
        const auto blend = static_cast<pack::ModelBlend>(m.blend);
        const bool transparent = blend != pack::ModelBlend::Opaque && blend != pack::ModelBlend::AlphaKey;
        Pipeline pipeline = kOpaque;
        switch (blend) {
        case pack::ModelBlend::Opaque: pipeline = twoSided ? kOpaqueTwoSided : kOpaque; break;
        case pack::ModelBlend::AlphaKey: pipeline = twoSided ? kAlphaKeyTwoSided : kAlphaKey; break;
        case pack::ModelBlend::Alpha: pipeline = kAlpha; break;
        case pack::ModelBlend::Add: pipeline = kAdd; break;
        case pack::ModelBlend::Mod: pipeline = kMod; break;
        case pack::ModelBlend::Mod2x: pipeline = kMod2x; break;
        }
        if (m.flags & pack::kModelMaterialLiquid) {
            pipeline = transparent ? kLiquid : kLiquidOpaque;
        }
        // Phase 11: every WMO part mixes its vertex colors' light and the outdoor one by the vertex alpha (v6).
        const bool interior = header.kind == static_cast<u32>(pack::ModelKind::Wmo);
        model.parts.push_back({p.firstIndex, p.indexCount, p.vertexOffset, p.material, p.group,
                               static_cast<u8>(pipeline), static_cast<u8>(transparent ? 1 : 0),
                               static_cast<u8>(interior ? 1 : 0)});
    }
    model.groupCount = header.groupCount;
    if (m_groupMask.size() * 64 < header.groupCount) {
        m_groupMask.resize((header.groupCount + 63) / 64);
    }
    const u32 ambient = header.ambientColor;
    model.ambient = {static_cast<f32>(ambient & 0xFF) / 255.0f, static_cast<f32>((ambient >> 8) & 0xFF) / 255.0f,
                     static_cast<f32>((ambient >> 16) & 0xFF) / 255.0f, 1.0f};

    // Phase 14 (v7): an animated doodad's skin stream goes between its vertices and its indices.
    u32 skinSource = 0;
    if (header.skeletonOffset != 0 && header.kind == static_cast<u32>(pack::ModelKind::M2) &&
        header.skeletonOffset % 4 == 0 && u64{header.skeletonOffset} + sizeof(pack::ModelSkeletonHeader) <= size) {
        pack::ModelSkeletonHeader skeleton;
        std::memcpy(&skeleton, blob + header.skeletonOffset, sizeof(skeleton));
        if (skeleton.skinOffset != 0 &&
            u64{skeleton.skinOffset} + u64{header.vertexCount} * sizeof(pack::ModelSkinVertex) <= size) {
            skinSource = skeleton.skinOffset;
        }
    }
    const u64 skinBytes = skinSource != 0 ? u64{header.vertexCount} * sizeof(pack::ModelSkinVertex) : 0;
    if (vertexBytes > 0) {
        model.skinOffset = skinBytes > 0 ? alignUp(vertexBytes, 16) : 0;
        model.indexOffset = alignUp(skinBytes > 0 ? model.skinOffset + skinBytes : vertexBytes, 16);
        const u64 bufferSize = model.indexOffset + std::max<u64>(indexBytes, 4);
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferSize;
        bufferInfo.usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VD_VK_TRY(device.createStreamedBuffer(bufferInfo, &model.buffer, &model.allocation));
        uploader.copyBufferFrom(src, srcOffset + header.verticesOffset, model.buffer, 0, vertexBytes);
        if (skinBytes > 0) {
            uploader.copyBufferFrom(src, srcOffset + skinSource, model.buffer, model.skinOffset, skinBytes);
        }
        if (indexBytes > 0) {
            uploader.copyBufferFrom(src, srcOffset + header.indicesOffset, model.buffer, model.indexOffset, indexBytes);
        }
        model.gpuBytes = bufferSize;
    }
    model.used = true;

    u32 id = 0;
    if (!m_freeModels.empty()) {
        id = m_freeModels.back();
        m_freeModels.pop_back();
    } else {
        id = static_cast<u32>(m_models.size());
        m_models.emplace_back();
    }
    m_gpuBytes += model.gpuBytes;
    m_models[id] = std::move(model);
    ++m_liveModels;
    return id;
}

Result<u32> ModelRenderer::createActorModel(const vk::Device& device, vk::Uploader& uploader, VkBuffer src,
                                            u64 srcOffset, const u8* blob, usize size) {
    pack::ActorModelHeader header;
    if (size < sizeof(header)) {
        return Error{"Actor blob too small"};
    }
    std::memcpy(&header, blob, sizeof(header));
    const u64 vertexBytes = u64{header.vertexCount} * sizeof(pack::ActorVertex);
    const u64 indexBytes = u64{header.indexCount} * 2;
    if (header.version != pack::kActorFormatVersion || header.verticesOffset + vertexBytes > size ||
        header.indicesOffset + indexBytes > size ||
        header.materialsOffset + u64{header.materialCount} * sizeof(pack::ActorMaterial) > size ||
        header.partsOffset + u64{header.partCount} * sizeof(pack::ActorPart) > size) {
        return Error{"Corrupt actor blob (re-bake actors)"};
    }
    Model model;
    model.actor = true;
    model.indexType = VK_INDEX_TYPE_UINT16;
    model.materials.resize(header.materialCount);
    const auto* materials = reinterpret_cast<const pack::ActorMaterial*>(blob + header.materialsOffset);
    for (u32 i = 0; i < header.materialCount; ++i) {
        Material& m = model.materials[i];
        m.flags = materials[i].flags;
        m.blend = materials[i].blend;
        m.uvAnimation = materials[i].uvAnimation;
        m.slot = materials[i].slot < kActorTextureSlots ? materials[i].slot : 0xFF; // 0xFF: never textured
        const u32 c = materials[i].color;
        m.color[0] = static_cast<f32>(c & 0xFF) / 255.0f;
        m.color[1] = static_cast<f32>((c >> 8) & 0xFF) / 255.0f;
        m.color[2] = static_cast<f32>((c >> 16) & 0xFF) / 255.0f;
        m.color[3] = static_cast<f32>(c >> 24) / 255.0f;
    }
    const auto* parts = reinterpret_cast<const pack::ActorPart*>(blob + header.partsOffset);
    model.parts.reserve(header.partCount);
    for (u32 i = 0; i < header.partCount; ++i) {
        const pack::ActorPart& p = parts[i];
        if (p.material >= header.materialCount || p.geosetIndex >= std::max(1u, header.geosetCount) ||
            u64{p.firstIndex} + p.indexCount > header.indexCount) {
            return Error{"Corrupt actor part"};
        }
        const Material& m = model.materials[p.material];
        const bool twoSided = (m.flags & pack::kModelMaterialTwoSided) != 0;
        const auto blend = static_cast<pack::ModelBlend>(m.blend);
        const bool transparent = blend != pack::ModelBlend::Opaque && blend != pack::ModelBlend::AlphaKey;
        u32 pipeline = kActorOpaque;
        switch (blend) {
        case pack::ModelBlend::Opaque: pipeline = twoSided ? kActorOpaqueTwoSided : kActorOpaque; break;
        case pack::ModelBlend::AlphaKey: pipeline = twoSided ? kActorAlphaKeyTwoSided : kActorAlphaKey; break;
        case pack::ModelBlend::Alpha: pipeline = kActorAlpha; break;
        case pack::ModelBlend::Add: pipeline = kActorAdd; break;
        case pack::ModelBlend::Mod: pipeline = kActorMod; break;
        case pack::ModelBlend::Mod2x: pipeline = kActorMod2x; break;
        }
        model.parts.push_back({p.firstIndex, p.indexCount, 0, p.material, p.geosetIndex, static_cast<u8>(pipeline),
                               static_cast<u8>(transparent ? 1 : 0), 0});
    }
    model.groupCount = std::max(1u, header.geosetCount);
    if (m_groupMask.size() * 64 < model.groupCount) {
        m_groupMask.resize((model.groupCount + 63) / 64);
    }
    if (vertexBytes > 0 && indexBytes > 0) {
        model.indexOffset = alignUp(vertexBytes, 16);
        const u64 bufferSize = model.indexOffset + indexBytes;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferSize;
        bufferInfo.usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VD_VK_TRY(device.createStreamedBuffer(bufferInfo, &model.buffer, &model.allocation));
        uploader.copyBufferFrom(src, srcOffset + header.verticesOffset, model.buffer, 0, vertexBytes);
        uploader.copyBufferFrom(src, srcOffset + header.indicesOffset, model.buffer, model.indexOffset, indexBytes);
        model.gpuBytes = bufferSize;
    }
    model.used = true;
    u32 id = 0;
    if (!m_freeModels.empty()) {
        id = m_freeModels.back();
        m_freeModels.pop_back();
    } else {
        id = static_cast<u32>(m_models.size());
        m_models.emplace_back();
    }
    m_gpuBytes += model.gpuBytes;
    m_models[id] = std::move(model);
    ++m_liveModels;
    return id;
}

Result<u32> ModelRenderer::createActorMesh(const vk::Device& device, vk::Uploader& uploader, u32 model,
                                           std::span<const u16> indices, std::span<const ActorMeshRun> runs) {
    if (model == 0 || model >= m_models.size() || !m_models[model].used || !m_models[model].actor || indices.empty()) {
        return Error{"Actor mesh without a loaded actor model"};
    }
    const Model& source = m_models[model];
    Mesh mesh;
    mesh.model = model;
    for (const ActorMeshRun& run : runs) {
        if (run.material >= source.materials.size() || u64{run.firstIndex} + run.indexCount > indices.size()) {
            return Error{"Corrupt actor mesh run"};
        }
        // The pipeline of any part with that material (parts of one material share it).
        const auto part = std::find_if(source.parts.begin(), source.parts.end(),
                                       [&](const Part& p) { return p.material == run.material; });
        if (part == source.parts.end()) {
            return Error{"Actor mesh material without parts"};
        }
        mesh.runs.push_back({run.firstIndex, run.indexCount, static_cast<u16>(run.material), part->pipeline,
                             part->transparent});
    }
    const u64 bytes = indices.size_bytes();
    vk::Uploader::Staging staging;
    if (!uploader.reserve(bytes, staging)) {
        return Error{"Upload ring full (retry)"};
    }
    std::memcpy(staging.data, indices.data(), bytes);
    uploader.commit(device, staging);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VD_VK_TRY(device.createStreamedBuffer(bufferInfo, &mesh.buffer, &mesh.allocation));
    uploader.copyBuffer(staging, 0, mesh.buffer, 0, bytes);
    mesh.used = true;
    u32 id = 0;
    if (!m_freeMeshes.empty()) {
        id = m_freeMeshes.back();
        m_freeMeshes.pop_back();
    } else {
        id = static_cast<u32>(m_meshes.size());
        m_meshes.emplace_back();
    }
    m_meshes[id] = std::move(mesh);
    return id;
}

void ModelRenderer::destroyActorMesh(u32 id, u64 retireFrame) {
    if (id == 0 || id >= m_meshes.size() || !m_meshes[id].used) {
        return;
    }
    m_meshes[id].used = false;
    m_retiredMeshes.push_back({id, retireFrame});
}

void ModelRenderer::setMaterialTexture(u32 model, u32 material, u32 texture) {
    if (model < m_models.size() && m_models[model].used && material < m_models[model].materials.size()) {
        m_models[model].materials[material].texture = texture;
    }
}

void ModelRenderer::destroyModel(u32 id, u64 retireFrame) {
    if (id == 0 || id >= m_models.size() || !m_models[id].used) {
        return;
    }
    m_models[id].used = false;
    m_retired.push_back({id, retireFrame});
}

void ModelRenderer::collect(const vk::Device& device, u64 submittedFrames, bool everything) {
    usize keptMeshes = 0;
    for (const Retired& retired : m_retiredMeshes) {
        if (everything || submittedFrames >= retired.retireFrame) {
            Mesh& mesh = m_meshes[retired.id];
            if (mesh.buffer) {
                vmaDestroyBuffer(device.allocator(), mesh.buffer, mesh.allocation);
            }
            mesh = Mesh{};
            m_freeMeshes.push_back(retired.id);
        } else {
            m_retiredMeshes[keptMeshes++] = retired;
        }
    }
    m_retiredMeshes.resize(keptMeshes);
    usize kept = 0;
    for (const Retired& retired : m_retired) {
        if (everything || submittedFrames >= retired.retireFrame) {
            release(device, retired.id);
        } else {
            m_retired[kept++] = retired;
        }
    }
    m_retired.resize(kept);
}

void ModelRenderer::release(const vk::Device& device, u32 id) {
    Model& model = m_models[id];
    if (model.buffer) {
        vmaDestroyBuffer(device.allocator(), model.buffer, model.allocation);
    }
    m_gpuBytes -= model.gpuBytes;
    --m_liveModels;
    model = Model{};
    m_freeModels.push_back(id);
}

void ModelRenderer::prepare(const vk::Device& device, u32 slot, std::span<const ModelInstanceData> instances) {
    if (slot >= m_frames) {
        return;
    }
    const u32 count = std::min<u32>(static_cast<u32>(instances.size()), kMaxInstances);
    if (count > 0) {
        std::memcpy(m_instances[slot].mapped, instances.data(), count * sizeof(ModelInstanceData));
        vmaFlushAllocation(device.allocator(), m_instances[slot].allocation, 0, count * sizeof(ModelInstanceData));
    }
    m_instanceCount[slot] = count;
}

void ModelRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, VkDescriptorSet liquidSet, u32 slot,
                         std::span<const ModelDraw> draws, std::span<const u16> groups, bool transparent,
                         RenderStats& stats) const {
    if (draws.empty() || slot >= m_frames || !m_pipelines[kOpaque]) {
        return;
    }
    const u32 instanceLimit = m_instanceCount[slot];
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkPipelineLayout setsLayout = VK_NULL_HANDLE; // the layout the bound descriptor sets went through
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    bool instancesBound = false;
    bool doodadBonesBound = false; // phase 14
    ModelPush pushed{};
    bool anyPushed = false;

    // One draw for a run of parts that share state and are contiguous in the index buffer.
    auto drawRun = [&](const Model& model, const ModelDraw& draw, const Part& part, u32 indexCount, bool& modelBound) {
        if (!instancesBound) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 1, 1, &m_instances[slot].buffer, &zero);
            instancesBound = true;
        }
        // Phase 14: an animated doodad skins its vertices (the doodad pipelines, its skin stream and the doodad
        // palette); instances in the fade band dither their opaque parts out through the alpha-key shader.
        const bool skinned = (draw.doodadFlags & ModelDraw::kDoodadSkinned) != 0 && model.skinOffset != 0 &&
                             part.pipeline < kLiquidOpaque;
        const bool fading = (draw.doodadFlags & ModelDraw::kDoodadFading) != 0 && part.pipeline < kLiquidOpaque;
        if (!modelBound) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.buffer, &zero);
            if (model.skinOffset != 0) {
                vkCmdBindVertexBuffers(cmd, 2, 1, &model.buffer, &model.skinOffset);
            }
            vkCmdBindIndexBuffer(cmd, model.buffer, model.indexOffset, model.indexType);
            modelBound = true;
        }
        // Every set goes through the layout of the pipeline that reads it. Sets bound through a compatible layout
        // are valid Vulkan, but the Adreno 650's driver (Vulkan 1.1.128) lost the doodad palette (set 2, bound
        // through the actors' layout) once set 1 was bound after it through the two-set one: every animated doodad
        // vanished on the phone, leaving only its particles (a Teldrassil street lamp's light without the lamp).
        const bool liquid = part.pipeline >= kLiquidOpaque;
        const VkPipelineLayout layout = liquid ? m_liquidLayout : skinned ? m_actorLayout : m_pipelineLayout;
        if (layout != setsLayout) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frameSet, 0, nullptr);
            setsLayout = layout;
            boundTexture = VK_NULL_HANDLE;
            doodadBonesBound = false;
            anyPushed = false;
        }
        if (skinned && !doodadBonesBound) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_actorLayout, 2, 1, &m_gpuDoodadBoneSets[slot],
                                    0, nullptr);
            doodadBonesBound = true;
        }
        u32 pipelineId = part.pipeline;
        u32 doodadFlags = 0;
        if (fading) {
            doodadFlags |= 2u; // model_frag.glsl VD_FLAG_DOODAD_FADE
            if (pipelineId == kOpaque || pipelineId == kOpaqueTwoSided) {
                pipelineId = pipelineId == kOpaque ? kAlphaKey : kAlphaKeyTwoSided;
                doodadFlags |= 4u; // VD_FLAG_NO_ALPHA_TEST: an opaque material, only dithered
            }
        }
        if (skinned) {
            pipelineId += kDoodadOpaque - kOpaque;
        }
        const VkPipeline pipeline = m_pipelines[pipelineId];
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            boundPipeline = pipeline;
        }
        const Material& material = model.materials[part.material];
        VkDescriptorSet set = liquidSet;
        if (!liquid) {
            const u32 texture =
                material.texture < m_textureSets.size() && m_textureSets[material.texture][0] ? material.texture : 0;
            set = m_textureSets[texture][(material.flags & pack::kModelMaterialClamp) ? 1 : 0];
        }
        if (set != boundTexture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set, 0, nullptr);
            boundTexture = set;
        }
        ModelPush push{{material.flags, part.interior, material.blend, doodadFlags},
                       {model.ambient[0], model.ambient[1], model.ambient[2], model.ambient[3]},
                       {material.color[0], material.color[1], material.color[2], material.color[3]},
                       {}};
        uvRows(m_modelUv, draw.firstUvTransform, material.uvAnimation, push.uv);
        std::memcpy(push.selfLight, draw.selfLight.data(), sizeof(push.selfLight));
        if (material.emissive != 0) { // a WMO window's night glow, where an M2 keeps its own light's color
            push.selfLight[4] = static_cast<f32>(material.emissive & 0xFF) / 255.0f;
            push.selfLight[5] = static_cast<f32>((material.emissive >> 8) & 0xFF) / 255.0f;
            push.selfLight[6] = static_cast<f32>((material.emissive >> 16) & 0xFF) / 255.0f;
        }
        // Pushed again through each new layout (above), for the same reason as the sets.
        if (!anyPushed || std::memcmp(&push, &pushed, sizeof(push)) != 0) {
            vkCmdPushConstants(cmd, layout, kPushStages, 0, sizeof(push), &push);
            pushed = push;
            anyPushed = true;
        }
        vkCmdDrawIndexed(cmd, indexCount, draw.instanceCount, part.firstIndex, static_cast<i32>(part.vertexOffset),
                         draw.firstInstance);
        const u32 triangles = indexCount / 3 * draw.instanceCount;
        ++stats.drawCalls;
        stats.triangles += triangles;
        if (draw.groupCount != 0) {
            ++stats.wmoDrawCalls;
            stats.wmoTriangles += triangles;
        } else {
            ++stats.m2DrawCalls;
            stats.m2Triangles += triangles;
        }
    };

    for (const ModelDraw& draw : draws) {
        const u32 id = draw.model.index;
        if (id == 0 || id >= m_models.size() || !m_models[id].used || !m_models[id].buffer ||
            draw.firstInstance + draw.instanceCount > instanceLimit) {
            continue;
        }
        const Model& model = m_models[id];
        const bool masked = draw.groupCount != 0 && u64{draw.firstGroup} + draw.groupCount <= groups.size();
        if (masked) {
            std::fill_n(m_groupMask.begin(), (model.groupCount + 63) / 64, 0ull);
            for (u32 g = draw.firstGroup; g < draw.firstGroup + draw.groupCount; ++g) {
                if (groups[g] < model.groupCount) {
                    m_groupMask[groups[g] >> 6] |= 1ull << (groups[g] & 63);
                }
            }
        }
        bool modelBound = false;
        const Part* run = nullptr;
        u32 runCount = 0;
        for (const Part& part : model.parts) {
            if ((part.transparent != 0) != transparent ||
                (masked && !(m_groupMask[part.group >> 6] & (1ull << (part.group & 63)))) ||
                (part.pipeline >= kLiquidOpaque && !liquidSet)) {
                continue;
            }
            if (run && part.firstIndex == run->firstIndex + runCount && part.vertexOffset == run->vertexOffset &&
                part.material == run->material && part.pipeline == run->pipeline && part.interior == run->interior) {
                runCount += part.indexCount;
                continue;
            }
            if (run) {
                drawRun(model, draw, *run, runCount, modelBound);
            }
            run = &part;
            runCount = part.indexCount;
        }
        if (run) {
            drawRun(model, draw, *run, runCount, modelBound);
        }
    }
}

std::span<anim::BoneMatrix> ModelRenderer::boneMatrices(u32 buffer) const {
    if (buffer >= kBoneBuffers || !m_bones[buffer].mapped) {
        return {};
    }
    return {reinterpret_cast<anim::BoneMatrix*>(m_bones[buffer].mapped), kMaxBones};
}

void ModelRenderer::prepareActors(const vk::Device& device, u32 slot, u32 buffer,
                                  std::span<const ActorInstanceData> instances, u32 boneCount) {
    if (slot >= m_frames || buffer >= kBoneBuffers) {
        return;
    }
    const u32 count = std::min<u32>(static_cast<u32>(instances.size()), kMaxActorInstances);
    if (count > 0) {
        std::memcpy(m_actorInstances[slot].mapped, instances.data(), count * sizeof(ActorInstanceData));
        vmaFlushAllocation(device.allocator(), m_actorInstances[slot].allocation, 0, count * sizeof(ActorInstanceData));
    }
    if (boneCount > 0) {
        vmaFlushAllocation(device.allocator(), m_bones[buffer].allocation, 0,
                           u64{std::min(boneCount, kMaxBones)} * sizeof(anim::BoneMatrix));
    }
    m_actorInstanceCount[slot] = count;
    m_boneBufferOf[slot] = buffer;
    m_boneCount[slot] = std::min(boneCount, kMaxBones);
}

void ModelRenderer::recordBoneCopy(VkCommandBuffer cmd, u32 slot) const {
    if (slot >= m_frames || m_boneCount[slot] == 0) {
        return;
    }
    const VkBufferCopy region{0, 0, u64{m_boneCount[slot]} * sizeof(anim::BoneMatrix)};
    vkCmdCopyBuffer(cmd, m_bones[m_boneBufferOf[slot]].buffer, m_gpuBones[slot], 1, &region);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_gpuBones[slot];
    barrier.size = region.size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 1,
                         &barrier, 0, nullptr);
}

std::span<anim::BoneMatrix> ModelRenderer::doodadBoneMatrices(u32 buffer) const {
    if (buffer >= kBoneBuffers || !m_doodadBones[buffer].mapped) {
        return {};
    }
    return {reinterpret_cast<anim::BoneMatrix*>(m_doodadBones[buffer].mapped), kMaxDoodadBones};
}

void ModelRenderer::prepareDoodadBones(const vk::Device& device, u32 slot, u32 buffer, u32 boneCount) {
    if (slot >= m_frames || buffer >= kBoneBuffers) {
        return;
    }
    m_doodadBoneBufferOf[slot] = buffer;
    m_doodadBoneCount[slot] = std::min(boneCount, kMaxDoodadBones);
    if (m_doodadBoneCount[slot] > 0) {
        vmaFlushAllocation(device.allocator(), m_doodadBones[buffer].allocation, 0,
                           u64{m_doodadBoneCount[slot]} * sizeof(anim::BoneMatrix));
    }
}

void ModelRenderer::recordDoodadBoneCopy(VkCommandBuffer cmd, u32 slot) const {
    if (slot >= m_frames || m_doodadBoneCount[slot] == 0) {
        return;
    }
    const VkBufferCopy region{0, 0, u64{m_doodadBoneCount[slot]} * sizeof(anim::BoneMatrix)};
    vkCmdCopyBuffer(cmd, m_doodadBones[m_doodadBoneBufferOf[slot]].buffer, m_gpuDoodadBones[slot], 1, &region);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_gpuDoodadBones[slot];
    barrier.size = region.size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 1,
                         &barrier, 0, nullptr);
}

VkDescriptorSet ModelRenderer::textureSet(u32 texture, bool clamp) const {
    const u32 index = texture < m_textureSets.size() && m_textureSets[texture][0] ? texture : 0;
    return m_textureSets[index][clamp ? 1 : 0];
}

void ModelRenderer::drawActors(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, std::span<const ActorDraw> draws,
                               std::span<const u16> geosets, std::span<const glm::vec4> colors, bool transparent,
                               RenderStats& stats) const {
    if (draws.empty() || slot >= m_frames || !m_pipelines[kActorOpaque]) {
        return;
    }
    const u32 instanceLimit = m_actorInstanceCount[slot];
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    bool setsBound = false;
    ModelPush pushed{};
    bool anyPushed = false;
    // Opaque pass in two sweeps, plain parts then alpha-tested ones (hair, fur, fringes), so the pipeline changes a
    // handful of times instead of for every actor. Blended parts keep the draw order. Phase 11: translucent actors
    // (ActorDraw::fade) are left out of the opaque pass and drawn whole after everyone's blended parts.
    const bool anyFaded =
        transparent && std::any_of(draws.begin(), draws.end(), [](const ActorDraw& d) { return d.fade.w < 1.0f; });
    const u32 phases = transparent ? (anyFaded ? 2u : 1u) : 2u;
    for (u32 phase = 0; phase < phases; ++phase) {
        const bool fadePhase = transparent && phase == 1;
        auto inPhase = [&](u8 pipeline, u8 partTransparent) {
            if (fadePhase) {
                return true; // every part of a translucent actor
            }
            return (partTransparent != 0) == transparent &&
                   (transparent || ((pipeline == kActorAlphaKey || pipeline == kActorAlphaKeyTwoSided) == (phase == 1)));
        };
        for (const ActorDraw& draw : draws) {
            if ((draw.fade.w < 1.0f) != fadePhase) {
                continue;
            }
            const u32 id = draw.model.index;
            if (id == 0 || id >= m_models.size() || !m_models[id].used || !m_models[id].actor || !m_models[id].buffer ||
                draw.instanceCount == 0 || draw.firstInstance + draw.instanceCount > instanceLimit) {
                continue;
            }
            const Model& model = m_models[id];
            const bool masked = draw.geosetCount != 0 && u64{draw.firstGeoset} + draw.geosetCount <= geosets.size();
            if (masked) {
                std::fill_n(m_groupMask.begin(), (model.groupCount + 63) / 64, 0ull);
                for (u32 g = draw.firstGeoset; g < draw.firstGeoset + draw.geosetCount; ++g) {
                    if (geosets[g] < model.groupCount) {
                        m_groupMask[geosets[g] >> 6] |= 1ull << (geosets[g] & 63);
                    }
                }
            }
            const bool animatedColors = draw.firstColor != ActorDraw::kNoColors &&
                                        u64{draw.firstColor} + model.materials.size() <= colors.size();
            const u32 meshId = draw.mesh.index;
            const Mesh* mesh = meshId != 0 && meshId < m_meshes.size() && m_meshes[meshId].used &&
                                       m_meshes[meshId].model == id
                                   ? &m_meshes[meshId]
                                   : nullptr;
            bool modelBound = false;
            const Part* run = nullptr;
            u32 runCount = 0;
            auto flush = [&]() {
                if (!run) {
                    return;
                }
                const Material& material = model.materials[run->material];
                const f32* base = animatedColors ? &colors[draw.firstColor + run->material].x : material.color;
                const f32 color[4] = {base[0] * draw.fade.x, base[1] * draw.fade.y, base[2] * draw.fade.z,
                                      base[3] * draw.fade.w};
                if (color[3] < 0.004f) {
                    return; // faded out this frame
                }
                if (!setsBound) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_actorLayout, 0, 1, &frameSet, 0,
                                            nullptr);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_actorLayout, 2, 1,
                                            &m_gpuBoneSets[slot], 0, nullptr);
                    const VkDeviceSize zero = 0;
                    vkCmdBindVertexBuffers(cmd, 1, 1, &m_actorInstances[slot].buffer, &zero);
                    setsBound = true;
                }
                if (!modelBound) {
                    const VkDeviceSize zero = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &model.buffer, &zero);
                    if (mesh) {
                        vkCmdBindIndexBuffer(cmd, mesh->buffer, 0, VK_INDEX_TYPE_UINT16);
                    } else {
                        vkCmdBindIndexBuffer(cmd, model.buffer, model.indexOffset, model.indexType);
                    }
                    modelBound = true;
                }
                // Phase 11: a translucent actor's opaque and alpha-tested parts blend, with its fade as their alpha.
                u32 pipelineId = run->pipeline;
                u32 fadeFlag = 0;
                if (fadePhase && pipelineId >= kActorOpaque && pipelineId <= kActorAlphaKeyTwoSided) {
                    pipelineId = kActorFade + (pipelineId - kActorOpaque);
                    fadeFlag = 1; // model_frag.glsl VD_FLAG_FADED
                }
                const VkPipeline pipeline = m_pipelines[pipelineId];
                if (pipeline != boundPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    boundPipeline = pipeline;
                }
                u32 texture = material.texture;
                if (material.slot == 0xFF) {
                    texture = 0;
                } else if (material.slot != 0) {
                    texture = draw.textures[material.slot].index;
                }
                const VkDescriptorSet set = textureSet(texture, (material.flags & pack::kModelMaterialClamp) != 0);
                if (set != boundTexture) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_actorLayout, 1, 1, &set, 0,
                                            nullptr);
                    boundTexture = set;
                }
                ModelPush push{{material.flags, 0, material.blend, fadeFlag},
                               {0.0f, 0.0f, 0.0f, 1.0f},
                               {color[0], color[1], color[2], color[3]},
                               {}};
                uvRows(m_actorUv, draw.firstUvTransform, material.uvAnimation, push.uv);
                if (!anyPushed || std::memcmp(&push, &pushed, sizeof(push)) != 0) {
                    vkCmdPushConstants(cmd, m_actorLayout, kPushStages, 0, sizeof(push), &push);
                    pushed = push;
                    anyPushed = true;
                }
                vkCmdDrawIndexed(cmd, runCount, draw.instanceCount, run->firstIndex, 0, draw.firstInstance);
                const u32 triangles = runCount / 3 * draw.instanceCount;
                ++stats.drawCalls;
                stats.triangles += triangles;
                ++stats.actorDrawCalls;
                stats.actorTriangles += triangles;
            };
            if (mesh) {
                // Already one run per material: draw them as they are.
                for (const MeshRun& r : mesh->runs) {
                    if (!inPhase(r.pipeline, r.transparent)) {
                        continue;
                    }
                    const Part part{r.firstIndex, r.indexCount, 0, r.material, 0, r.pipeline, r.transparent, 0};
                    run = &part;
                    runCount = r.indexCount;
                    flush();
                    run = nullptr;
                }
                continue;
            }
            for (const Part& part : model.parts) {
                if (!inPhase(part.pipeline, part.transparent) ||
                    (masked && !(m_groupMask[part.group >> 6] & (1ull << (part.group & 63))))) {
                    continue;
                }
                if (run && part.firstIndex == run->firstIndex + runCount && part.material == run->material) {
                    runCount += part.indexCount;
                    continue;
                }
                flush();
                run = &part;
                runCount = part.indexCount;
            }
            flush();
        }
    }
}

} // namespace vd
