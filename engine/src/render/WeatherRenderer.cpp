#include "render/WeatherRenderer.h"

#include "core/Profile.h"
#include "render/vk/Device.h"
#include "render/vk/Pipeline.h"

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kRainVertSpv[] = {
#include "weather_rain.vert.spv.inc"
};
constexpr u32 kFlakeVertSpv[] = {
#include "weather_flake.vert.spv.inc"
};
constexpr u32 kMistVertSpv[] = {
#include "weather_mist.vert.spv.inc"
};
constexpr u32 kWeatherFragSpv[] = {
#include "weather.frag.spv.inc"
};

// Must match the push_constant block of the weather vertex shaders.
struct WeatherPush {
    u32 params[4]; // x: part (rain: 0 the drops, 1 their splashes)
};

// game::WeatherType.
constexpr u32 kKindRain = 1;
constexpr u32 kKindSnow = 2;
constexpr u32 kKindSand = 3;

} // namespace

Result<void> WeatherRenderer::init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                                   u32 framesInFlight) {
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

    const VkDescriptorSetLayoutBinding uniformBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                      VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uniformBinding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_uniformLayout));
    const VkDescriptorSetLayoutBinding textureBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layoutInfo.pBindings = &textureBinding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_textureLayout));

    const std::array<VkDescriptorPoolSize, 2> poolSizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFrames},
         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kWeatherTextures}}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kMaxFrames + kWeatherTextures;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));

    // One uniform block per frame in flight, host visible: the frame writes its own after its fence.
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>(16, device.properties().limits.minUniformBufferOffsetAlignment);
    m_uniformsStride = (sizeof(WeatherUniforms) + alignment - 1) / alignment * alignment;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = m_uniformsStride * m_frames;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_uniforms, &m_uniformsAllocation,
                              &info));
    m_uniformsMapped = static_cast<u8*>(info.pMappedData);
    std::array<VkDescriptorSetLayout, kMaxFrames> layouts;
    layouts.fill(m_uniformLayout);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = m_pool;
    setInfo.descriptorSetCount = m_frames;
    setInfo.pSetLayouts = layouts.data();
    VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, m_uniformSets.data()));
    for (u32 i = 0; i < m_frames; ++i) {
        const VkDescriptorBufferInfo buffer{m_uniforms, m_uniformsStride * i, sizeof(WeatherUniforms)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_uniformSets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }

    const std::array<VkDescriptorSetLayout, 3> setLayouts{frameLayout, m_uniformLayout, m_textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(WeatherPush)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_layout));
    return {};
}

void WeatherRenderer::shutdown(const vk::Device& device) {
    VkDevice dev = device.device();
    if (dev == VK_NULL_HANDLE) {
        return;
    }
    destroyPipelines(device);
    for (vk::Texture& texture : m_textures) {
        vk::destroyTexture(device, texture);
        texture = {};
    }
    m_textureSets = {};
    m_uniformSets = {};
    if (m_uniforms) {
        vmaDestroyBuffer(device.allocator(), m_uniforms, m_uniformsAllocation);
    }
    m_uniforms = VK_NULL_HANDLE;
    m_uniformsAllocation = VK_NULL_HANDLE;
    m_uniformsMapped = nullptr;
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_uniformLayout)
        vkDestroyDescriptorSetLayout(dev, m_uniformLayout, nullptr);
    if (m_textureLayout)
        vkDestroyDescriptorSetLayout(dev, m_textureLayout, nullptr);
    if (m_sampler)
        vkDestroySampler(dev, m_sampler, nullptr);
    m_layout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_uniformLayout = VK_NULL_HANDLE;
    m_textureLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

Result<void> WeatherRenderer::createPipelines(const vk::Device& device, VkRenderPass renderPass) {
    const std::span<const u32> vertexShaders[kPipelineCount] = {kRainVertSpv, kFlakeVertSpv, kMistVertSpv};
    for (u32 i = 0; i < kPipelineCount; ++i) {
        vk::GraphicsPipelineDesc desc;
        desc.vertexSpv = vertexShaders[i];
        desc.fragmentSpv = kWeatherFragSpv;
        desc.layout = m_layout;
        desc.renderPass = renderPass;
        desc.cullMode = VK_CULL_MODE_NONE; // facing the camera, or seen edge-on
        desc.depthWrite = false;
        desc.blend = vk::BlendMode::Alpha;
        desc.name = "weather pipeline";
        Result<VkPipeline> pipeline = vk::createGraphicsPipeline(device, desc);
        if (!pipeline) {
            return pipeline.error();
        }
        m_pipelines[i] = pipeline.value();
    }
    return {};
}

void WeatherRenderer::destroyPipelines(const vk::Device& device) {
    for (VkPipeline& pipeline : m_pipelines) {
        if (pipeline) {
            vkDestroyPipeline(device.device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}

Result<void> WeatherRenderer::setTextures(const vk::Device& device,
                                          std::span<const std::span<const u8>> blobs) {
    VkDevice dev = device.device();
    std::array<vk::Texture, kWeatherTextures> created{};
    for (u32 i = 0; i < kWeatherTextures && i < blobs.size(); ++i) {
        if (blobs[i].empty()) {
            continue;
        }
        Result<vk::Texture> texture = vk::createTextureFromBlob(device, blobs[i].data(), blobs[i].size());
        if (!texture) {
            for (vk::Texture& done : created) {
                vk::destroyTexture(device, done);
            }
            return texture.error();
        }
        created[i] = texture.value();
    }
    vkDeviceWaitIdle(dev); // the old ones may still be in use
    for (u32 i = 0; i < kWeatherTextures; ++i) {
        vk::destroyTexture(device, m_textures[i]);
        m_textures[i] = created[i];
        if (!m_textures[i].view) {
            continue;
        }
        if (!m_textureSets[i]) {
            VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setInfo.descriptorPool = m_pool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts = &m_textureLayout;
            VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, &m_textureSets[i]));
        }
        const VkDescriptorImageInfo image{m_sampler, m_textures[i].view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_textureSets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
    return {};
}

void WeatherRenderer::prepare(const vk::Device& device, u32 slot, const WeatherUniforms* weather) {
    if (slot >= m_frames) {
        return;
    }
    Frame& frame = m_frame[slot];
    frame = {};
    if (!weather || weather->time.y == 0 || !m_uniformsMapped) {
        return;
    }
    frame.kind = weather->time.y;
    frame.particles = weather->particles.life.x;
    frame.mists = weather->mist.life.x;
    std::memcpy(m_uniformsMapped + m_uniformsStride * slot, weather, sizeof(WeatherUniforms));
    vmaFlushAllocation(device.allocator(), m_uniformsAllocation, m_uniformsStride * slot,
                       sizeof(WeatherUniforms));
}

void WeatherRenderer::drawPart(VkCommandBuffer cmd, Pipeline pipeline, WeatherTexture texture, u32 part,
                               u32 vertices, RenderStats& stats) const {
    const VkDescriptorSet set = m_textureSets[static_cast<u32>(texture)];
    if (vertices == 0 || !set || !m_pipelines[pipeline]) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[pipeline]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 2, 1, &set, 0, nullptr);
    const WeatherPush push{{part, 0, 0, 0}};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, vertices, 1, 0, 0);
    ++stats.drawCalls;
    ++stats.weatherDrawCalls;
    stats.triangles += vertices / 3;
    stats.weatherVertices += vertices;
}

void WeatherRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot,
                           RenderStats& stats) const {
    if (slot >= m_frames || m_frame[slot].kind == 0) {
        return;
    }
    VD_PROFILE_ZONE("WeatherRenderer::draw");
    const Frame& frame = m_frame[slot];
    const std::array<VkDescriptorSet, 2> sets{frameSet, m_uniformSets[slot]};
    // Every pipeline shares the layout: the frame and the weather stay bound across them.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, static_cast<u32>(sets.size()),
                            sets.data(), 0, nullptr);
    switch (frame.kind) {
    case kKindRain:
        // MapWeather's two packets: Drop (Vertex_rain) and Patter (Vertex_patter), a triangle each per drop.
        drawPart(cmd, kRain, WeatherTexture::RainDrop, 0, frame.particles * 3, stats);
        drawPart(cmd, kRain, WeatherTexture::RainSplash, 1, frame.particles * 3, stats);
        break;
    case kKindSnow:
        drawPart(cmd, kFlake, WeatherTexture::SnowFlake, 0, frame.particles * 3, stats);
        drawPart(cmd, kMist, WeatherTexture::SnowMist, 0, frame.mists * 6, stats);
        break;
    case kKindSand:
        drawPart(cmd, kFlake, WeatherTexture::SandGrain, 0, frame.particles * 3, stats);
        drawPart(cmd, kMist, WeatherTexture::SandGrain, 0, frame.mists * 6, stats);
        break;
    default: break;
    }
}

} // namespace vd
