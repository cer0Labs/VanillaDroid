#include "render/TerrainRenderer.h"

#include "core/Log.h"
#include "pack/PackFormat.h"
#include "pack/TerrainFormat.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kTerrainVertSpv[] = {
#include "terrain.vert.spv.inc"
};
constexpr u32 kTerrainFragSpv[] = {
#include "terrain.frag.spv.inc"
};
constexpr u32 kLiquidVertSpv[] = {
#include "liquid.vert.spv.inc"
};
constexpr u32 kLiquidFragSpv[] = {
#include "liquid.frag.spv.inc"
};

struct TerrainPush {
    f32 origin[4];
};

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

bool supportsLinearSampling(VkPhysicalDevice gpu, VkFormat format) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(gpu, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
}

} // namespace

Result<void> TerrainRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout) {
    VkDevice dev = device.device();
    m_frameLayout = frameLayout;
    m_storageAlignment =
        static_cast<u32>(std::max<VkDeviceSize>(16, device.properties().limits.minStorageBufferOffsetAlignment));
    // B4G4R4A4 is mandatory in Vulkan, but checked anyway: the fallback expands to RGBA8 on the CPU.
    if (!supportsLinearSampling(device.physicalDevice(), VK_FORMAT_B4G4R4A4_UNORM_PACK16)) {
        m_alphaFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VD_LOG_WARN("Terrain alpha: B4G4R4A4 not filterable, using RGBA8");
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    // No anisotropic filtering by default: with 4 ASTC layers per pixel it is the most expensive thing in the pass.
    // Measured on Adreno 650 (2400x1080, Goldshire view): GPU 4.2 ms off, 6.3 ms at 2x, 9.8 ms at 4x. Phase 18
    // (world): a video option (setAnisotropy()), Ultra only.
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &m_layerSampler));
    samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &m_alphaSampler));

    // Material: terrain layer array (binding 0) and liquid frames array (binding 1).
    const std::array<VkDescriptorSetLayoutBinding, 2> material{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<u32>(material.size());
    layoutInfo.pBindings = material.data();
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_materialLayout));

    const std::array<VkDescriptorSetLayoutBinding, 2> tile{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    }};
    layoutInfo.bindingCount = static_cast<u32>(tile.size());
    layoutInfo.pBindings = tile.data();
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_tileLayout));

    const std::array<VkDescriptorSetLayout, 3> setLayouts{m_frameLayout, m_materialLayout, m_tileLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TerrainPush)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

    // Phase 18 (world): the material sets of maps being retired live next to the new one for a frame or two.
    constexpr u32 kMaterialSets = 4;
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTiles + 2 * kMaterialSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxTiles},
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kMaxTiles + kMaterialSets;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));

    m_tiles.reserve(kMaxTiles + 1);
    m_freeTiles.reserve(kMaxTiles);
    m_retired.reserve(kMaxTiles);
    m_retiredMaps.reserve(kMaterialSets);
    return {};
}

void TerrainRenderer::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    clearMap(device);
    destroyPipeline(device);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_tileLayout) vkDestroyDescriptorSetLayout(dev, m_tileLayout, nullptr);
    if (m_materialLayout) vkDestroyDescriptorSetLayout(dev, m_materialLayout, nullptr);
    if (m_alphaSampler) vkDestroySampler(dev, m_alphaSampler, nullptr);
    if (m_layerSampler) vkDestroySampler(dev, m_layerSampler, nullptr);
    *this = TerrainRenderer{};
}

Result<void> TerrainRenderer::createPipeline(const vk::Device& device, VkRenderPass renderPass) {
    const VkVertexInputBindingDescription binding{0, sizeof(pack::TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> attributes{{
        {0, 0, VK_FORMAT_R32_SFLOAT, offsetof(pack::TerrainVertex, height)},
        {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(pack::TerrainVertex, normal)},
    }};
    vk::GraphicsPipelineDesc desc;
    desc.vertexSpv = kTerrainVertSpv;
    desc.fragmentSpv = kTerrainFragSpv;
    desc.layout = m_pipelineLayout;
    desc.renderPass = renderPass;
    desc.bindings = {&binding, 1};
    desc.attributes = attributes;
    // Tile triangles are counter-clockwise seen from above; the heightfield is never seen from below.
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    desc.name = "terrain pipeline";
    Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
    if (!pipeline) {
        return pipeline.error();
    }
    m_pipeline = pipeline.value();

    // Liquids: implicit positions like the terrain; seen from above and from below (swimming).
    const VkVertexInputBindingDescription liquidBinding{0, sizeof(pack::LiquidVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> liquidAttributes{{
        {0, 0, VK_FORMAT_R32_SFLOAT, offsetof(pack::LiquidVertex, height)},
        {1, 0, VK_FORMAT_R32_UINT, offsetof(pack::LiquidVertex, grid)}, // grid | depth << 16 | kind << 24
    }};
    desc.vertexSpv = kLiquidVertSpv;
    desc.fragmentSpv = kLiquidFragSpv;
    desc.bindings = {&liquidBinding, 1};
    desc.attributes = liquidAttributes;
    desc.cullMode = VK_CULL_MODE_NONE;
    desc.name = "liquid pipeline (opaque)";
    Result<VkPipeline> opaque = vk::createGraphicsPipeline(device, desc);
    if (!opaque) {
        return opaque.error();
    }
    m_liquidOpaque = opaque.value();
    desc.depthWrite = false;
    desc.blend = vk::BlendMode::Alpha;
    desc.name = "liquid pipeline (translucent)";
    Result<VkPipeline> translucent = vk::createGraphicsPipeline(device, desc);
    if (!translucent) {
        return translucent.error();
    }
    m_liquidTranslucent = translucent.value();
    return {};
}

void TerrainRenderer::destroyPipeline(const vk::Device& device) {
    for (VkPipeline* pipeline : {&m_pipeline, &m_liquidOpaque, &m_liquidTranslucent}) {
        if (*pipeline) {
            vkDestroyPipeline(device.device(), *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
}

void TerrainRenderer::setAnisotropy(const vk::Device& device, u32 level) {
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
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        return;
    }
    collect(device, m_lastSubmitted, true); // retired material sets hold the old sampler: the GPU is idle, they go
    vkDestroySampler(device.device(), m_layerSampler, nullptr);
    m_layerSampler = sampler;
    if (!m_materialSet) {
        return;
    }
    const std::array<VkDescriptorImageInfo, 2> images{{
        {m_layerSampler, m_textures.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_layerSampler, m_liquidTextures.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = m_materialSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device.device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

Result<void> TerrainRenderer::setTextures(const vk::Device& device, vk::Uploader& uploader, VkBuffer src,
                                          u64 terrainOffset, const pack::TextureHeader& terrain, u64 liquidOffset,
                                          const pack::TextureHeader* liquids) {
    if (!liquids && !m_liquidTextures.image) {
        return Error{"Terrain textures without liquid frames"};
    }
    Result<vk::Texture> layers = vk::createTextureStaged(device, uploader, src, terrainOffset, terrain);
    if (!layers) {
        return layers.error();
    }
    vk::Texture frames = m_liquidTextures;
    if (liquids) {
        Result<vk::Texture> staged = vk::createTextureStaged(device, uploader, src, liquidOffset, *liquids);
        if (!staged) {
            vk::destroyTexture(device, layers.value());
            return staged.error();
        }
        frames = staged.value();
    }
    auto drop = [&] {
        vk::destroyTexture(device, layers.value());
        if (liquids) {
            vk::destroyTexture(device, frames);
        }
    };
    if (!layers.value().isArray || !frames.isArray || frames.layers < 4 * pack::kLiquidFrames) {
        drop();
        return Error{"Terrain and liquid textures must be 2D arrays"};
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_materialLayout;
    if (VkResult r = vkAllocateDescriptorSets(device.device(), &allocInfo, &set); r != VK_SUCCESS) {
        drop();
        return vk::makeError("terrain material set", r);
    }
    // Whatever this replaces may still be drawn by the frames in flight: it is retired, not freed. The liquid frames
    // stay from map to map; they are replaced only on the first map, or by another liquids pack.
    RetiredMap old;
    old.retireFrame = m_lastSubmitted + kRetireFrames;
    if (liquids && m_liquidTextures.image) {
        old.liquids = m_liquidTextures;
    }
    if (m_materialSet) { // setTextures twice without retireMap()
        old.materialSet = m_materialSet;
        old.textures = m_textures;
    }
    if (old.liquids.image || old.materialSet) {
        m_retiredMaps.push_back(old);
    }
    m_specularMasks = (terrain.flags & pack::kTextureSpecularMask) != 0; // phase 11
    m_textures = layers.value();
    m_liquidTextures = frames;
    m_materialSet = set;
    const std::array<VkDescriptorImageInfo, 2> images{{
        {m_layerSampler, m_textures.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_layerSampler, m_liquidTextures.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = m_materialSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device.device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    return {};
}

Result<void> TerrainRenderer::setSharedIndices(const vk::Device& device, vk::Uploader& uploader, const u16* indices,
                                               u32 count, std::span<const u32> lod) {
    if (lod.size() != 2 * pack::kLodLevels) {
        return Error{"Bad LOD ranges for the shared terrain indices"};
    }
    for (u32 level = 0; level < pack::kLodLevels; ++level) {
        if (u64{lod[2 * level]} + lod[2 * level + 1] > count) {
            return Error{"LOD range out of the shared terrain indices"};
        }
        m_sharedLod[2 * level] = lod[2 * level];
        m_sharedLod[2 * level + 1] = lod[2 * level + 1];
    }
    if (m_sharedIndices) { // set twice without retireMap(): retired like a map's
        RetiredMap old;
        old.sharedIndices = m_sharedIndices;
        old.sharedIndicesAllocation = m_sharedIndicesAllocation;
        old.retireFrame = m_lastSubmitted + kRetireFrames;
        m_retiredMaps.push_back(old);
        m_sharedIndices = VK_NULL_HANDLE;
        m_sharedIndicesAllocation = VK_NULL_HANDLE;
    }
    const u64 size = u64{count} * sizeof(u16);
    vk::Uploader::Staging staging;
    if (!uploader.reserve(size, staging)) {
        return Error{"Upload ring full (shared terrain indices)"};
    }
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_sharedIndices,
                              &m_sharedIndicesAllocation, nullptr));
    std::memcpy(staging.data, indices, size);
    uploader.commit(device, staging);
    uploader.copyBuffer(staging, 0, m_sharedIndices, 0, size);
    return {};
}

void TerrainRenderer::retireMap(u64 retireFrame) {
    if (!m_materialSet && !m_textures.image && !m_sharedIndices) {
        return;
    }
    RetiredMap old;
    old.textures = m_textures;
    old.materialSet = m_materialSet;
    old.sharedIndices = m_sharedIndices;
    old.sharedIndicesAllocation = m_sharedIndicesAllocation;
    old.retireFrame = retireFrame;
    m_retiredMaps.push_back(old);
    m_textures = {};
    m_materialSet = VK_NULL_HANDLE;
    m_sharedIndices = VK_NULL_HANDLE;
    m_sharedIndicesAllocation = VK_NULL_HANDLE;
    m_sharedLod = {};
}

void TerrainRenderer::releaseMap(const vk::Device& device, RetiredMap& map) {
    if (map.materialSet) {
        vkFreeDescriptorSets(device.device(), m_pool, 1, &map.materialSet);
    }
    vk::destroyTexture(device, map.textures);
    vk::destroyTexture(device, map.liquids);
    if (map.sharedIndices) {
        vmaDestroyBuffer(device.allocator(), map.sharedIndices, map.sharedIndicesAllocation);
    }
    map = {};
}

void TerrainRenderer::clearMap(const vk::Device& device) {
    for (u32 id = 1; id < m_tiles.size(); ++id) {
        if (m_tiles[id].buffer) {
            releaseTile(device, id);
        }
    }
    m_retired.clear();
    for (RetiredMap& map : m_retiredMaps) {
        releaseMap(device, map);
    }
    m_retiredMaps.clear();
    if (m_materialSet) {
        vkFreeDescriptorSets(device.device(), m_pool, 1, &m_materialSet);
        m_materialSet = VK_NULL_HANDLE;
    }
    vk::destroyTexture(device, m_textures);
    vk::destroyTexture(device, m_liquidTextures);
    if (m_sharedIndices) {
        vmaDestroyBuffer(device.allocator(), m_sharedIndices, m_sharedIndicesAllocation);
        m_sharedIndices = VK_NULL_HANDLE;
        m_sharedIndicesAllocation = VK_NULL_HANDLE;
    }
    m_sharedLod = {};
}

Result<u32> TerrainRenderer::createTile(const vk::Device& device, vk::Uploader& uploader, const u8* blob, usize size,
                                        const BlobSource* source) {
    pack::TerrainTileHeader header;
    if (size < sizeof(header)) {
        return Error{"Terrain tile blob too small"};
    }
    std::memcpy(&header, blob, sizeof(header));
    constexpr u64 kVerticesSize = u64{pack::kTileVertices} * sizeof(pack::TerrainVertex);
    constexpr u64 kChunksSize = u64{pack::kTileChunks} * sizeof(pack::TerrainChunk);
    constexpr u32 kAlphaTexels = pack::kTerrainAlphaSize * pack::kTerrainAlphaSize;
    const bool ownIndices = (header.flags & pack::kTerrainTileHasHoles) != 0;
    if (header.version != pack::kTerrainFormatVersion || header.verticesOffset + kVerticesSize > size ||
        header.chunksOffset + kChunksSize > size || header.alphaSize != kAlphaTexels * 2 ||
        header.alphaOffset + u64{header.alphaSize} > size ||
        (ownIndices && header.indicesOffset + u64{header.indexCount} * 2 > size) ||
        header.liquidVerticesOffset + u64{header.liquidVertexCount} * sizeof(pack::LiquidVertex) > size ||
        header.liquidIndicesOffset + u64{header.liquidIndexCount} * 2 > size ||
        header.liquidOpaqueFirst > header.liquidIndexCount) {
        return Error{"Corrupt terrain tile blob"};
    }
    if (!ownIndices && m_sharedIndices == VK_NULL_HANDLE) {
        return Error{"Terrain tile without the map's shared indices"};
    }
    std::array<u32, 8> lod{};
    for (u32 level = 0; level < pack::kLodLevels; ++level) {
        const pack::LodRange range = ownIndices ? header.lod[level]
                                                : pack::LodRange{m_sharedLod[2 * level], m_sharedLod[2 * level + 1]};
        if (ownIndices && u64{range.first} + range.count > header.indexCount) {
            return Error{"Corrupt terrain tile LOD ranges"};
        }
        lod[2 * level] = range.first;
        lod[2 * level + 1] = range.count;
    }
    if (m_freeTiles.empty() && m_tiles.size() > kMaxTiles) {
        return Error{"Too many terrain tiles"};
    }

    // GPU buffer: [vertices][chunk table][own indices][liquid vertices][liquid indices].
    const u64 chunksOffset = alignUp(kVerticesSize, m_storageAlignment);
    const u64 indexOffset = alignUp(chunksOffset + kChunksSize, m_storageAlignment);
    const u64 terrainEnd = ownIndices ? indexOffset + u64{header.indexCount} * 2 : chunksOffset + kChunksSize;
    const u64 liquidVerticesSize = u64{header.liquidVertexCount} * sizeof(pack::LiquidVertex);
    const u64 liquidVertexOffset = alignUp(terrainEnd, 16);
    const u64 liquidIndexOffset = alignUp(liquidVertexOffset + liquidVerticesSize, 16);
    const bool hasLiquids = header.liquidIndexCount > 0;
    const u64 bufferSize = hasLiquids ? liquidIndexOffset + u64{header.liquidIndexCount} * 2 : terrainEnd;
    const u64 alphaBytes = u64{kAlphaTexels} * (m_alphaFormat == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 2);
    const u64 alphaStagingOffset = alignUp(bufferSize, 16);
    // The RGBA8 fallback needs a CPU conversion, so it always goes through the ring.
    const bool direct = source && source->buffer && m_alphaFormat != VK_FORMAT_R8G8B8A8_UNORM;

    vk::Uploader::Staging staging;
    if (!direct && !uploader.reserve(alphaStagingOffset + alphaBytes, staging)) {
        return u32{0}; // ring busy: the caller retries next frame
    }

    Tile tile;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VD_VK_TRY(device.createStreamedBuffer(bufferInfo, &tile.buffer, &tile.allocation));

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_alphaFormat;
    imageInfo.extent = {pack::kTerrainAlphaSize, pack::kTerrainAlphaSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VkResult r = device.createStreamedImage(imageInfo, &tile.alpha, &tile.alphaAllocation); r != VK_SUCCESS) {
        vmaDestroyBuffer(device.allocator(), tile.buffer, tile.allocation);
        return vk::makeError("vmaCreateImage(terrain alpha)", r);
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = tile.alpha;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_alphaFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkResult result = vkCreateImageView(device.device(), &viewInfo, nullptr, &tile.alphaView);
    if (result == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = m_pool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &m_tileLayout;
        result = vkAllocateDescriptorSets(device.device(), &setInfo, &tile.set);
    }
    if (result != VK_SUCCESS) {
        if (tile.alphaView) vkDestroyImageView(device.device(), tile.alphaView, nullptr);
        vmaDestroyImage(device.allocator(), tile.alpha, tile.alphaAllocation);
        vmaDestroyBuffer(device.allocator(), tile.buffer, tile.allocation);
        return vk::makeError("terrain tile descriptors", result);
    }

    if (direct) {
        // The IO thread wrote the blob into mapped memory: the GPU copies each section from there.
        vmaFlushAllocation(device.allocator(), source->allocation, 0, size);
        uploader.copyBufferFrom(source->buffer, header.verticesOffset, tile.buffer, 0, kVerticesSize);
        uploader.copyBufferFrom(source->buffer, header.chunksOffset, tile.buffer, chunksOffset, kChunksSize);
        if (ownIndices) {
            uploader.copyBufferFrom(source->buffer, header.indicesOffset, tile.buffer, indexOffset,
                                    u64{header.indexCount} * 2);
        }
        uploader.copyImageFrom(source->buffer, header.alphaOffset, tile.alpha, pack::kTerrainAlphaSize,
                               pack::kTerrainAlphaSize);
        if (hasLiquids) {
            uploader.copyBufferFrom(source->buffer, header.liquidVerticesOffset, tile.buffer, liquidVertexOffset,
                                    liquidVerticesSize);
            uploader.copyBufferFrom(source->buffer, header.liquidIndicesOffset, tile.buffer, liquidIndexOffset,
                                    u64{header.liquidIndexCount} * 2);
        }
    } else {
        // Stage everything in the GPU layout, then one buffer copy and one image copy.
        std::memcpy(staging.data, blob + header.verticesOffset, kVerticesSize);
        std::memcpy(staging.data + chunksOffset, blob + header.chunksOffset, kChunksSize);
        if (ownIndices) {
            std::memcpy(staging.data + indexOffset, blob + header.indicesOffset, u64{header.indexCount} * 2);
        }
        if (hasLiquids) {
            std::memcpy(staging.data + liquidVertexOffset, blob + header.liquidVerticesOffset, liquidVerticesSize);
            std::memcpy(staging.data + liquidIndexOffset, blob + header.liquidIndicesOffset,
                        u64{header.liquidIndexCount} * 2);
        }
        const u8* alpha = blob + header.alphaOffset;
        if (m_alphaFormat == VK_FORMAT_R8G8B8A8_UNORM) {
            u8* out = staging.data + alphaStagingOffset;
            for (u32 i = 0; i < kAlphaTexels; ++i) {
                u16 texel;
                std::memcpy(&texel, alpha + i * 2, 2);
                out[i * 4 + 0] = static_cast<u8>(((texel >> 4) & 0xF) * 17);
                out[i * 4 + 1] = static_cast<u8>(((texel >> 8) & 0xF) * 17);
                out[i * 4 + 2] = static_cast<u8>(((texel >> 12) & 0xF) * 17);
                out[i * 4 + 3] = static_cast<u8>((texel & 0xF) * 17);
            }
        } else {
            std::memcpy(staging.data + alphaStagingOffset, alpha, alphaBytes);
        }
        uploader.commit(device, staging);
        uploader.copyBuffer(staging, 0, tile.buffer, 0, bufferSize);
        uploader.copyImage(staging, alphaStagingOffset, tile.alpha, pack::kTerrainAlphaSize,
                           pack::kTerrainAlphaSize);
    }

    const VkDescriptorImageInfo imageDescriptor{m_alphaSampler, tile.alphaView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo bufferDescriptor{tile.buffer, chunksOffset, kChunksSize};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = tile.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageDescriptor;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = tile.set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferDescriptor;
    vkUpdateDescriptorSets(device.device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    tile.ownIndices = ownIndices;
    tile.indexOffset = ownIndices ? indexOffset : 0;
    tile.lod = lod;
    tile.liquidVertexOffset = liquidVertexOffset;
    tile.liquidIndexOffset = liquidIndexOffset;
    tile.liquidTranslucentCount = header.liquidOpaqueFirst;
    tile.liquidOpaqueFirst = header.liquidOpaqueFirst;
    tile.liquidOpaqueCount = header.liquidIndexCount - header.liquidOpaqueFirst;
    tile.used = true;
    tile.gpuBytes = bufferSize + alphaBytes;

    u32 id = 0;
    if (!m_freeTiles.empty()) {
        id = m_freeTiles.back();
        m_freeTiles.pop_back();
    } else {
        id = static_cast<u32>(m_tiles.size());
        m_tiles.emplace_back();
    }
    m_tiles[id] = tile;
    ++m_liveTiles;
    m_gpuBytes += tile.gpuBytes;
    return id;
}

void TerrainRenderer::destroyTile(u32 id, u64 retireFrame) {
    if (id == 0 || id >= m_tiles.size() || !m_tiles[id].used) {
        return;
    }
    m_tiles[id].used = false;
    m_retired.push_back({id, retireFrame});
}

void TerrainRenderer::collect(const vk::Device& device, u64 submittedFrames, bool everything) {
    m_lastSubmitted = submittedFrames;
    usize keptMaps = 0;
    for (RetiredMap& map : m_retiredMaps) {
        if (everything || submittedFrames >= map.retireFrame) {
            releaseMap(device, map);
        } else {
            m_retiredMaps[keptMaps++] = map;
        }
    }
    m_retiredMaps.resize(keptMaps);
    usize kept = 0;
    for (const Retired& retired : m_retired) {
        if (everything || submittedFrames >= retired.retireFrame) {
            releaseTile(device, retired.id);
        } else {
            m_retired[kept++] = retired;
        }
    }
    m_retired.resize(kept);
}

void TerrainRenderer::releaseTile(const vk::Device& device, u32 id) {
    Tile& tile = m_tiles[id];
    if (tile.set) vkFreeDescriptorSets(device.device(), m_pool, 1, &tile.set);
    if (tile.alphaView) vkDestroyImageView(device.device(), tile.alphaView, nullptr);
    if (tile.alpha) vmaDestroyImage(device.allocator(), tile.alpha, tile.alphaAllocation);
    if (tile.buffer) vmaDestroyBuffer(device.allocator(), tile.buffer, tile.allocation);
    m_gpuBytes -= tile.gpuBytes;
    --m_liveTiles;
    tile = {};
    m_freeTiles.push_back(id);
}

void TerrainRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, std::span<const TerrainDraw> draws,
                           RenderStats& stats) {
    if (!ready() || draws.empty()) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const std::array<VkDescriptorSet, 2> shared{frameSet, m_materialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, static_cast<u32>(shared.size()),
                            shared.data(), 0, nullptr);
    for (const TerrainDraw& draw : draws) {
        const u32 id = draw.tile.index;
        if (id == 0 || id >= m_tiles.size() || !m_tiles[id].used) {
            continue;
        }
        const Tile& tile = m_tiles[id];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 2, 1, &tile.set, 0, nullptr);
        const TerrainPush push{{draw.origin.x, draw.origin.y, draw.origin.z, 0.0f}};
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &tile.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, tile.ownIndices ? tile.buffer : m_sharedIndices, tile.indexOffset,
                             VK_INDEX_TYPE_UINT16);
        const u32 level = std::min<u32>(draw.lod, pack::kLodLevels - 1);
        const u32 count = tile.lod[2 * level + 1];
        vkCmdDrawIndexed(cmd, count, 1, tile.lod[2 * level], 0, 0);
        ++stats.drawCalls;
        stats.triangles += count / 3;
    }
}

void TerrainRenderer::drawLiquids(VkCommandBuffer cmd, VkDescriptorSet frameSet, std::span<const TerrainDraw> draws,
                                  bool opaque, RenderStats& stats) {
    VkPipeline pipeline = opaque ? m_liquidOpaque : m_liquidTranslucent;
    if (!pipeline || !m_materialSet) {
        return;
    }
    bool bound = false;
    for (const TerrainDraw& draw : draws) {
        const u32 id = draw.tile.index;
        if (id == 0 || id >= m_tiles.size() || !m_tiles[id].used) {
            continue;
        }
        const Tile& tile = m_tiles[id];
        const u32 first = opaque ? tile.liquidOpaqueFirst : 0;
        const u32 count = opaque ? tile.liquidOpaqueCount : tile.liquidTranslucentCount;
        if (count == 0) {
            continue;
        }
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const std::array<VkDescriptorSet, 2> shared{frameSet, m_materialSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
                                    static_cast<u32>(shared.size()), shared.data(), 0, nullptr);
            bound = true;
        }
        const TerrainPush push{{draw.origin.x, draw.origin.y, draw.origin.z, 0.0f}};
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdBindVertexBuffers(cmd, 0, 1, &tile.buffer, &tile.liquidVertexOffset);
        vkCmdBindIndexBuffer(cmd, tile.buffer, tile.liquidIndexOffset, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
        ++stats.drawCalls;
        stats.triangles += count / 3;
    }
}

} // namespace vd
