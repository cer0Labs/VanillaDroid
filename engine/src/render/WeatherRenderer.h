#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "render/vk/Texture.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>

namespace vd {

namespace vk {
class Device;
} // namespace vk

// Rain, snow and sand (internal to render/, phase 11). Like the 1.12 client's weather packets (MapWeather.cpp
// with the vertex programs Vertex_rain, Vertex_patter, Vertex_snowpoint, Vertex_sand), the particles move on
// the GPU: nothing is built on the CPU, there are not even vertex buffers. Each vertex finds its particle
// from gl_VertexIndex, and the particle where it is now from the frame's clock, its seed and the camera (the
// frame's WeatherUniforms, one small uniform buffer per frame in flight). At most two draws a frame: the
// drops and their splashes, the flakes and their mist, or the grains of sand. Alpha blended over everything
// else, depth tested, not written; pipelines created with the others, never in game.
class WeatherRenderer {
public:
    static constexpr u32 kMaxFrames = 4;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    Result<void> createPipelines(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipelines(const vk::Device& device);
    // The client's textures (WeatherTexture order). Waits for the GPU to go idle.
    Result<void> setTextures(const vk::Device& device, std::span<const std::span<const u8>> blobs);

    // Once per frame, after the frame's fence: the weather the frame in `slot` draws (null: none).
    void prepare(const vk::Device& device, u32 slot, const WeatherUniforms* weather);
    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, RenderStats& stats) const;

private:
    enum Pipeline : u8 { kRain, kFlake, kMist, kPipelineCount };
    struct Frame {
        u32 kind = 0; // game::WeatherType drawn (0: nothing)
        u32 particles = 0;
        u32 mists = 0;
    };
    void drawPart(VkCommandBuffer cmd, Pipeline pipeline, WeatherTexture texture, u32 part, u32 vertices,
                  RenderStats& stats) const;

    u32 m_frames = 0;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::array<VkPipeline, kPipelineCount> m_pipelines{};
    VkDescriptorSetLayout m_uniformLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkBuffer m_uniforms = VK_NULL_HANDLE;
    VmaAllocation m_uniformsAllocation = VK_NULL_HANDLE;
    u8* m_uniformsMapped = nullptr;
    VkDeviceSize m_uniformsStride = 0;
    std::array<VkDescriptorSet, kMaxFrames> m_uniformSets{};
    std::array<vk::Texture, kWeatherTextures> m_textures{};
    std::array<VkDescriptorSet, kWeatherTextures> m_textureSets{}; // null until its texture is set
    std::array<Frame, kMaxFrames> m_frame{};
};

} // namespace vd
