#include "render/Renderer.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "render/FrameUniforms.h"
#include "render/HorizonRenderer.h"
#include "render/ModelRenderer.h"
#include "render/ParticleRenderer.h"
#include "render/ModelViewRenderer.h"
#include "render/PortraitRenderer.h"
#include "render/ScreenFx.h"
#include "render/SkyRenderer.h"
#include "render/WeatherRenderer.h"
#include "render/GrassRenderer.h" // phase 14 (grass)
#include "render/UiRenderer.h"
#include "render/TextureCompositor.h"
#include "render/TerrainRenderer.h"
#include "render/vk/Device.h"
#include "render/vk/Swapchain.h"
#include "render/vk/Texture.h"
#include "render/vk/Uploader.h"
#include "render/vk/WorldPass.h"

#if !defined(VD_DIST) // phase 22: the published build has no debug overlay (no Dear ImGui)
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#endif

#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

namespace vd {
namespace {

constexpr u32 kFramesInFlight = 2;
constexpr VkClearColorValue kClearColor = {{0.05f, 0.07f, 0.12f, 1.0f}};
// Staging ring for in-game uploads: a terrain tile needs ~2.4 MB, so this allows several per frame.
constexpr u64 kUploadRingBytes = 24ull << 20;

constexpr u32 kTriangleVertSpv[] = {
#include "triangle.vert.spv.inc"
};
constexpr u32 kTriangleFragSpv[] = {
#include "triangle.frag.spv.inc"
};
constexpr u32 kUpscaleVertSpv[] = {
#include "upscale.vert.spv.inc"
};
constexpr u32 kUpscaleFragSpv[] = {
#include "upscale.frag.spv.inc"
};

// Must match the push_constant block in shaders/triangle.vert.
struct TrianglePushConstants {
    f32 preRotate[4];
    f32 aspectScale[2];
    f32 angle;
    f32 padding;
};
static_assert(sizeof(TrianglePushConstants) == 32, "push constant layout mismatch");

// Must match the push_constant block in shaders/upscale.vert / upscale.frag.
struct UpscalePushConstants {
    f32 uvScale[4]; // the scene's share of its image, the largest coordinate sampled
    f32 texel[4];   // one texel of the image, sharpening
};
static_assert(sizeof(UpscalePushConstants) == 32, "push constant layout mismatch");

// Dynamic resolution (phase 10): the lowest scale of the world's resolution, and the sharpening of the stretch.
constexpr f32 kMinRenderScale = 0.5f;
constexpr f32 kUpscaleSharpness = 0.35f;

struct FrameSlot {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    bool timestampsPending = false;
};

enum class Rebuild : u8 { None, IfChanged, Force };

VkFormat chooseDepthFormat(VkPhysicalDevice gpu) {
    // D24S8 is the native depth format on Adreno; D32/D16 as fallbacks.
    for (VkFormat format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(gpu, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

bool hasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// Orders a frame's attachment writes after the previous frame's (shared depth image) and after the swapchain
// image is acquired (the acquire semaphore waits at COLOR_ATTACHMENT_OUTPUT). The portraits' pass uses it
// too: render passes are only compatible (share pipelines) with the same dependencies.
VkSubpassDependency mainPassDependency() {
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    return dependency;
}

u32 rotationDegrees(const f32 m[4]) {
    if (m[0] == 1.0f) return 0;
    if (m[1] == 1.0f) return 90;
    if (m[0] == -1.0f) return 180;
    return 270;
}

#if !defined(VD_DIST)
// ImGui lays out in logical (player-facing) pixels, but the framebuffer is in the display's native
// orientation. Apply the same pre-rotation as the scene shaders, on the CPU (overlay only: few vertices).
void preRotateDrawData(ImDrawData* drawData, const f32 m[4], VkExtent2D logical, VkExtent2D framebuffer) {
    const f32 lw = static_cast<f32>(logical.width);
    const f32 lh = static_cast<f32>(logical.height);
    const f32 fw = static_cast<f32>(framebuffer.width);
    const f32 fh = static_cast<f32>(framebuffer.height);
    auto toFramebuffer = [&](f32 x, f32 y) {
        const f32 cx = 2.0f * x / lw - 1.0f;
        const f32 cy = 2.0f * y / lh - 1.0f;
        const f32 rx = m[0] * cx + m[2] * cy;
        const f32 ry = m[1] * cx + m[3] * cy;
        return ImVec2((rx + 1.0f) * 0.5f * fw, (ry + 1.0f) * 0.5f * fh);
    };
    for (ImDrawList* list : drawData->CmdLists) {
        for (ImDrawVert& vertex : list->VtxBuffer) {
            vertex.pos = toFramebuffer(vertex.pos.x, vertex.pos.y);
        }
        for (ImDrawCmd& cmd : list->CmdBuffer) {
            const ImVec2 a = toFramebuffer(cmd.ClipRect.x, cmd.ClipRect.y);
            const ImVec2 b = toFramebuffer(cmd.ClipRect.z, cmd.ClipRect.w);
            cmd.ClipRect = ImVec4(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
        }
    }
    drawData->DisplayPos = ImVec2(0.0f, 0.0f);
    drawData->DisplaySize = ImVec2(fw, fh);
    drawData->FramebufferScale = ImVec2(1.0f, 1.0f);
}

void checkImGuiVkResult(VkResult result) {
    if (result != VK_SUCCESS) {
        VD_LOG_ERROR("ImGui Vulkan backend: %s", vk::resultToString(result));
    }
}
#endif

} // namespace

struct Renderer::Impl {
    PlatformInfo platform;
    vk::Device device;
    vk::Swapchain swapchain;

    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat renderPassColorFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline trianglePipeline = VK_NULL_HANDLE;

    // Dynamic resolution (phase 10). Below a scale of 1 the world is drawn by scenePass into the top-left part of
    // sceneImage (swapchain-sized, the display's native orientation), then stretched over the swapchain image by
    // the upscale pipeline at the start of the main pass, under the UI, which keeps the full resolution. At 1 the
    // world goes straight into the swapchain image as before: no extra pass, no extra bandwidth.
    VkRenderPass scenePass = VK_NULL_HANDLE; // compatible with renderPass: the world's pipelines draw in both
    VkImage sceneImage = VK_NULL_HANDLE;
    VmaAllocation sceneAllocation = VK_NULL_HANDLE;
    VkImageView sceneView = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
    VkSampler upscaleSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout upscaleSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool upscalePool = VK_NULL_HANDLE;
    VkDescriptorSet upscaleSet = VK_NULL_HANDLE;
    VkPipelineLayout upscaleLayout = VK_NULL_HANDLE;
    VkPipeline upscalePipeline = VK_NULL_HANDLE;
    f32 renderScale = 1.0f;

    // Phase 18 (world): the quality options, applied at a safe point (applyQuality(): the start of a frame, the GPU
    // idle): the main pass's samples (multisampling; its transient color, swapchain-sized) and the anisotropy of the
    // world's textures.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 pendingSamples = 1;
    u32 anisotropy = 1;
    u32 pendingAnisotropy = 1;
    vk::TransientImage multisampledColor;
    VkSampleCountFlagBits supportedSamples(u32 wanted) const;
    void applyQuality();

    // Swapchain-sized resources.
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished; // one per swapchain image
    bool loggedDepthMemory = false;

    std::array<FrameSlot, kFramesInFlight> frames{};
    u32 frameIndex = 0;

    // Scene: per-frame uniforms (set 0 of world pipelines), uploads and the terrain pass.
    VkBuffer frameUniforms = VK_NULL_HANDLE;
    VmaAllocation frameUniformsAllocation = VK_NULL_HANDLE;
    u8* frameUniformsMapped = nullptr;
    VkDeviceSize frameUniformsStride = 0;
    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool frameSetPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> frameSets{};
    vk::Uploader uploader;
    TerrainRenderer terrain;
    HorizonRenderer horizon;
    ModelRenderer models;
    TextureCompositor compositor;
    SkyRenderer sky;
    UiRenderer ui; // phase 7: the touch UI
    ParticleRenderer particles; // phase 9: particles and ribbons
    WeatherRenderer weather;    // phase 11: rain, snow and sand
    GrassRenderer grass;        // phase 14: the terrain's detail doodads
    PortraitRenderer portraits; // unit frame portraits, drawn with the actor pipelines
    std::array<u32, kPortraitSlots> portraitTextures{}; // their texture slots (sampled views)
    ModelViewRenderer modelViews; // phase 15: the interface's model frames
    std::array<u32, kModelViewSlots> modelViewTextures{};
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    f64 timestampPeriodNs = 0.0;
    u64 timestampMask = 0;

    bool surfaceAttached = false;
    Rebuild rebuild = Rebuild::None;
    RenderStats stats;

    // Debug overlay (Dear ImGui). The backend is created with the first render pass.
    bool imguiBackendReady = false;
    bool uiFrameActive = false;

    // Textures. Slot 0 is reserved so that TextureHandle{0} is invalid.
    struct TextureSlot {
        vk::Texture texture;
        VkDescriptorSet imguiSet = VK_NULL_HANDLE;
        bool used = false;
    };
    struct RetiredTexture {
        u32 index;
        u64 retireFrame;
    };
    std::vector<TextureSlot> textures{1};
    std::vector<u32> freeTextureSlots;
    std::vector<RetiredTexture> retiredTextures;
    u64 submittedFrames = 0;
    u64 completedFrames = 0;

    struct StagingSlot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
        u64 size = 0;
    };
    std::vector<StagingSlot> stagingBlocks{1}; // slot 0 reserved: id 0 is invalid
    struct RetiredStaging { // phase 18 (world): retireStagingBlock()
        u32 id;
        u64 retireFrame;
    };
    std::vector<RetiredStaging> retiredStaging;
    void collectRetiredStaging(bool everything);

    Result<void> initImGuiBackend();
    Result<void> createSceneResources();
    void destroySceneResources();
    void releaseTextureNow(u32 index);
    void registerPortraitTextures();
    void collectRetiredTextures(bool everything);
    Result<void> createFrameResources();
    void destroyFrameResources();
    Result<void> createRenderPassAndPipeline(VkFormat colorFormat);
    void destroyRenderPassAndPipeline();
    Result<void> createUpscalePipeline();
    Result<void> createSceneTarget();
    void destroySceneTarget();
    void drawScene(VkCommandBuffer cmd, u32 slot, const FrameParams& params);
    Result<void> createSwapchainResources();
    void destroySwapchainResources();
    Result<void> rebuildSwapchain(bool force);
    void readTimestamps(FrameSlot& frame, u32 slot);
    void recordFrame(FrameSlot& frame, u32 slot, u32 imageIndex, const FrameParams& params);
};

Result<void> Renderer::Impl::createSceneResources() {
    VkDevice dev = device.device();
    const VkDeviceSize uboAlignment =
        std::max<VkDeviceSize>(16, device.properties().limits.minUniformBufferOffsetAlignment);
    frameUniformsStride = (sizeof(FrameUniforms) + uboAlignment - 1) / uboAlignment * uboAlignment;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = frameUniformsStride * kFramesInFlight;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &frameUniforms, &frameUniformsAllocation,
                              &info));
    frameUniformsMapped = static_cast<u8*>(info.pMappedData);

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &frameSetLayout));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &frameSetPool));
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(frameSetLayout);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = frameSetPool;
    setInfo.descriptorSetCount = kFramesInFlight;
    setInfo.pSetLayouts = layouts.data();
    VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, frameSets.data()));
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        const VkDescriptorBufferInfo bufferDescriptor{frameUniforms, frameUniformsStride * i, sizeof(FrameUniforms)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frameSets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferDescriptor;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }

    // Phase 10: what the upscale samples the scene with (one set: the scene image is rewritten only while idle).
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VD_VK_TRY(vkCreateSampler(dev, &samplerInfo, nullptr, &upscaleSampler));
    VkDescriptorSetLayoutBinding sceneBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layoutInfo.pBindings = &sceneBinding;
    VD_VK_TRY(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &upscaleSetLayout));
    VkDescriptorPoolSize scenePoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    poolInfo.maxSets = 1;
    poolInfo.pPoolSizes = &scenePoolSize;
    VD_VK_TRY(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &upscalePool));
    setInfo.descriptorPool = upscalePool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &upscaleSetLayout;
    VD_VK_TRY(vkAllocateDescriptorSets(dev, &setInfo, &upscaleSet));

    VD_TRY(uploader.create(device, kUploadRingBytes, kFramesInFlight));
    VD_TRY(horizon.init(device, frameSetLayout, kFramesInFlight));
    VD_TRY(terrain.init(device, frameSetLayout));
    VD_TRY(models.init(device, frameSetLayout, terrain.materialLayout(), kFramesInFlight));
    VD_TRY(sky.init(device, frameSetLayout, models.textureLayout())); // phase 11: the models' texture sets
    VD_TRY(ui.init(device, models.textureLayout(), kFramesInFlight));
    VD_TRY(particles.init(device, frameSetLayout, models.textureLayout()));
    VD_TRY(weather.init(device, frameSetLayout, kFramesInFlight));
    VD_TRY(grass.init(device, frameSetLayout, models.textureLayout(), kFramesInFlight)); // phase 14
    VD_TRY(portraits.init(device, frameSetLayout, kFramesInFlight));
    VD_TRY(modelViews.init(device, frameSetLayout, kFramesInFlight)); // phase 15
    return compositor.init(device, models.textureLayout());
}

void Renderer::Impl::destroySceneResources() {
    VkDevice dev = device.device();
    terrain.shutdown(device);
    horizon.shutdown(device);
    compositor.shutdown(device);
    ui.shutdown(device);
    particles.shutdown(device);
    weather.shutdown(device);
    grass.shutdown(device); // phase 14
    portraits.shutdown(device);
    modelViews.shutdown(device); // phase 15
    models.shutdown(device);
    sky.shutdown(device);
    uploader.destroy(device);
    if (frameSetPool) vkDestroyDescriptorPool(dev, frameSetPool, nullptr);
    if (frameSetLayout) vkDestroyDescriptorSetLayout(dev, frameSetLayout, nullptr);
    if (upscalePool) vkDestroyDescriptorPool(dev, upscalePool, nullptr);
    if (upscaleSetLayout) vkDestroyDescriptorSetLayout(dev, upscaleSetLayout, nullptr);
    if (upscaleSampler) vkDestroySampler(dev, upscaleSampler, nullptr);
    upscalePool = VK_NULL_HANDLE;
    upscaleSet = VK_NULL_HANDLE;
    upscaleSetLayout = VK_NULL_HANDLE;
    upscaleSampler = VK_NULL_HANDLE;
    if (frameUniforms) vmaDestroyBuffer(device.allocator(), frameUniforms, frameUniformsAllocation);
    frameSetPool = VK_NULL_HANDLE;
    frameSetLayout = VK_NULL_HANDLE;
    frameUniforms = VK_NULL_HANDLE;
    frameUniformsAllocation = VK_NULL_HANDLE;
    frameUniformsMapped = nullptr;
}

Result<void> Renderer::Impl::createFrameResources() {
    VkDevice dev = device.device();
    for (FrameSlot& frame : frames) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = device.queueFamily();
        VD_VK_TRY(vkCreateCommandPool(dev, &poolInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VD_VK_TRY(vkAllocateCommandBuffers(dev, &allocInfo, &frame.commandBuffer));

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VD_VK_TRY(vkCreateSemaphore(dev, &semaphoreInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VD_VK_TRY(vkCreateFence(dev, &fenceInfo, nullptr, &frame.inFlight));
    }

    if (device.timestampValidBits() > 0) {
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * kFramesInFlight;
        VD_VK_TRY(vkCreateQueryPool(dev, &queryInfo, nullptr, &timestampPool));
        timestampPeriodNs = device.properties().limits.timestampPeriod;
        const u32 bits = device.timestampValidBits();
        timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    }
    return {};
}

void Renderer::Impl::destroyFrameResources() {
    VkDevice dev = device.device();
    for (FrameSlot& frame : frames) {
        if (frame.inFlight) vkDestroyFence(dev, frame.inFlight, nullptr);
        if (frame.imageAvailable) vkDestroySemaphore(dev, frame.imageAvailable, nullptr);
        if (frame.commandPool) vkDestroyCommandPool(dev, frame.commandPool, nullptr);
        frame = {};
    }
    if (timestampPool) {
        vkDestroyQueryPool(dev, timestampPool, nullptr);
        timestampPool = VK_NULL_HANDLE;
    }
}

Result<void> Renderer::Impl::createRenderPassAndPipeline(VkFormat colorFormat) {
    VkDevice dev = device.device();

    // Tile-friendly render pass: everything is cleared on load; only color is stored. Depth never
    // leaves tile memory (DONT_CARE + transient/lazily allocated image). Phase 18 (world): with multisampling the color
    // drawn into is transient too and resolves into the swapchain image on the tile (vk/WorldPass).
    device.setMainPassSamples(samples);
    const VkSubpassDependency dependency = mainPassDependency();
    Result<VkRenderPass> mainPass = vk::createWorldPass(device, colorFormat, depthFormat, samples,
                                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                        dependency);
    if (!mainPass) {
        return mainPass.error();
    }
    renderPass = mainPass.value();
    renderPassColorFormat = colorFormat;
    // Phase 10: the world at a lower resolution, into the scene image. Compatible with the main pass (same
    // attachments, subpass and dependency; only layouts and load/store differ): barriers before and after make the
    // image an attachment and then a texture for the upscale.
    Result<VkRenderPass> worldPass = vk::createWorldPass(device, colorFormat, depthFormat, samples,
                                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dependency);
    if (!worldPass) {
        return worldPass.error();
    }
    scenePass = worldPass.value();

    VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TrianglePushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout));

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    moduleInfo.codeSize = sizeof(kTriangleVertSpv);
    moduleInfo.pCode = kTriangleVertSpv;
    VD_VK_TRY(vkCreateShaderModule(dev, &moduleInfo, nullptr, &vertModule));
    moduleInfo.codeSize = sizeof(kTriangleFragSpv);
    moduleInfo.pCode = kTriangleFragSpv;
    const VkResult fragResult = vkCreateShaderModule(dev, &moduleInfo, nullptr, &fragModule);
    if (fragResult != VK_SUCCESS) {
        vkDestroyShaderModule(dev, vertModule, nullptr);
        return vk::makeError("vkCreateShaderModule(frag)", fragResult);
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples; // phase 18 (world): the main pass's
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<u32>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<u32>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    const VkResult pipelineResult =
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &trianglePipeline);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    vkDestroyShaderModule(dev, fragModule, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        return vk::makeError("vkCreateGraphicsPipelines", pipelineResult);
    }

    VD_TRY(createUpscalePipeline());
    VD_TRY(terrain.createPipeline(device, renderPass));
    VD_TRY(sky.createPipeline(device, renderPass));
    VD_TRY(horizon.createPipeline(device, renderPass));
    VD_TRY(models.createPipelines(device, renderPass));
    VD_TRY(particles.createPipelines(device, renderPass));
    VD_TRY(weather.createPipelines(device, renderPass));
    VD_TRY(grass.createPipelines(device, renderPass)); // phase 14
    VD_TRY(ui.createPipeline(device, renderPass));
    VD_TRY(portraits.createTargets(device, colorFormat, depthFormat, dependency, samples));
    VD_TRY(modelViews.createTargets(device, colorFormat, depthFormat, dependency, samples)); // phase 15
    registerPortraitTextures();

#if !defined(VD_DIST)
    // The overlay draws inside the main render pass (no extra pass on a tiled GPU).
    if (!imguiBackendReady) {
        return initImGuiBackend();
    }
    ImGui_ImplVulkan_PipelineInfo overlayPipeline{};
    overlayPipeline.RenderPass = renderPass;
    overlayPipeline.Subpass = 0;
    overlayPipeline.MSAASamples = samples; // phase 18 (world)
    ImGui_ImplVulkan_CreateMainPipeline(&overlayPipeline);
#endif
    return {};
}

// Phase 10: the scene stretched over the main pass's color, before the UI. No depth test, no blending.
Result<void> Renderer::Impl::createUpscalePipeline() {
    VkDevice dev = device.device();
    VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(UpscalePushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &upscaleSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VD_VK_TRY(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &upscaleLayout));

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    moduleInfo.codeSize = sizeof(kUpscaleVertSpv);
    moduleInfo.pCode = kUpscaleVertSpv;
    VD_VK_TRY(vkCreateShaderModule(dev, &moduleInfo, nullptr, &vertModule));
    moduleInfo.codeSize = sizeof(kUpscaleFragSpv);
    moduleInfo.pCode = kUpscaleFragSpv;
    if (const VkResult result = vkCreateShaderModule(dev, &moduleInfo, nullptr, &fragModule); result != VK_SUCCESS) {
        vkDestroyShaderModule(dev, vertModule, nullptr);
        return vk::makeError("vkCreateShaderModule(upscale.frag)", result);
    }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples; // phase 18 (world): drawn in the main pass
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<u32>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<u32>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = upscaleLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    const VkResult result =
        vkCreateGraphicsPipelines(dev, device.pipelineCache(), 1, &pipelineInfo, nullptr, &upscalePipeline);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    vkDestroyShaderModule(dev, fragModule, nullptr);
    if (result != VK_SUCCESS) {
        return vk::makeError("vkCreateGraphicsPipelines(upscale)", result);
    }
    return {};
}

// Phase 10: the image the world is drawn into below full resolution (as large as the swapchain's, so changing the
// scale never reallocates: only the render area changes), and its framebuffer with the main pass's depth.
Result<void> Renderer::Impl::createSceneTarget() {
    VkDevice dev = device.device();
    const VkExtent2D extent = swapchain.extent();
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain.format();
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VD_VK_TRY(vmaCreateImage(device.allocator(), &imageInfo, &allocInfo, &sceneImage, &sceneAllocation, nullptr));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = sceneImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchain.format();
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VD_VK_TRY(vkCreateImageView(dev, &viewInfo, nullptr, &sceneView));
    std::array<VkImageView, 3> views{};
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = scenePass;
    framebufferInfo.attachmentCount = vk::worldPassViews(samples, multisampledColor.view, depthView, sceneView, views);
    framebufferInfo.pAttachments = views.data();
    framebufferInfo.width = extent.width;
    framebufferInfo.height = extent.height;
    framebufferInfo.layers = 1;
    VD_VK_TRY(vkCreateFramebuffer(dev, &framebufferInfo, nullptr, &sceneFramebuffer));
    const VkDescriptorImageInfo image{upscaleSampler, sceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = upscaleSet;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    return {};
}

void Renderer::Impl::destroySceneTarget() {
    VkDevice dev = device.device();
    if (sceneFramebuffer) vkDestroyFramebuffer(dev, sceneFramebuffer, nullptr);
    if (sceneView) vkDestroyImageView(dev, sceneView, nullptr);
    if (sceneImage) vmaDestroyImage(device.allocator(), sceneImage, sceneAllocation);
    sceneFramebuffer = VK_NULL_HANDLE;
    sceneView = VK_NULL_HANDLE;
    sceneImage = VK_NULL_HANDLE;
    sceneAllocation = VK_NULL_HANDLE;
}

void Renderer::Impl::releaseTextureNow(u32 index) {
    models.unregisterTexture(device, index);
    TextureSlot& slot = textures[index];
#if !defined(VD_DIST)
    if (slot.imguiSet && imguiBackendReady) {
        ImGui_ImplVulkan_RemoveTexture(slot.imguiSet);
    }
#endif
    vk::destroyTexture(device, slot.texture);
    slot = {};
    freeTextureSlots.push_back(index);
}

// The portrait slots' views as textures of their own (the UI draws them like any other): the slots are
// reserved at init and never destroyed; their views change with the portraits' targets.
void Renderer::Impl::registerPortraitTextures() {
    for (u32 slot = 0; slot < kPortraitSlots; ++slot) {
        if (portraitTextures[slot] != 0 && portraits.sampledView(slot)) {
            models.registerTexture(device, portraitTextures[slot], portraits.sampledView(slot));
        }
    }
    for (u32 slot = 0; slot < kModelViewSlots; ++slot) { // phase 15: the model frames' images, the same way
        if (modelViewTextures[slot] != 0 && modelViews.sampledView(slot)) {
            models.registerTexture(device, modelViewTextures[slot], modelViews.sampledView(slot));
        }
    }
}

void Renderer::Impl::collectRetiredStaging(bool everything) {
    usize kept = 0;
    for (const RetiredStaging& retired : retiredStaging) {
        if (everything || submittedFrames >= retired.retireFrame) {
            StagingSlot& slot = stagingBlocks[retired.id];
            if (slot.buffer) {
                vmaDestroyBuffer(device.allocator(), slot.buffer, slot.allocation);
            }
            slot = {};
        } else {
            retiredStaging[kept++] = retired;
        }
    }
    retiredStaging.resize(kept);
}

void Renderer::Impl::collectRetiredTextures(bool everything) {
    auto it = std::remove_if(retiredTextures.begin(), retiredTextures.end(), [&](const RetiredTexture& retired) {
        if (everything || submittedFrames >= retired.retireFrame) {
            releaseTextureNow(retired.index);
            return true;
        }
        return false;
    });
    retiredTextures.erase(it, retiredTextures.end());
}

#if !defined(VD_DIST)
Result<void> Renderer::Impl::initImGuiBackend() {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = device.instance();
    info.PhysicalDevice = device.physicalDevice();
    info.Device = device.device();
    info.QueueFamily = device.queueFamily();
    info.Queue = device.queue();
    // Font atlas + debug textures; retired textures keep their set for kFramesInFlight frames.
    info.DescriptorPoolSize = 32;
    info.MinImageCount = 2;
    info.ImageCount = kFramesInFlight + 1; // vertex/index buffers cycled per draw; > frames in flight
    info.PipelineInfoMain.RenderPass = renderPass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = samples; // phase 18 (world)
    info.CheckVkResultFn = checkImGuiVkResult;
    info.PipelineCache = device.pipelineCache();
    if (!ImGui_ImplVulkan_Init(&info)) {
        return Error{"ImGui_ImplVulkan_Init failed"};
    }
    imguiBackendReady = true;
    return {};
}
#endif

void Renderer::Impl::destroyRenderPassAndPipeline() {
    VkDevice dev = device.device();
    terrain.destroyPipeline(device);
    sky.destroyPipeline(device);
    horizon.destroyPipeline(device);
    models.destroyPipelines(device);
    particles.destroyPipelines(device);
    weather.destroyPipelines(device);
    grass.destroyPipelines(device); // phase 14
    ui.destroyPipeline(device);
    portraits.destroyTargets(device);
    modelViews.destroyTargets(device); // phase 15
    if (trianglePipeline) vkDestroyPipeline(dev, trianglePipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    if (upscalePipeline) vkDestroyPipeline(dev, upscalePipeline, nullptr);
    if (upscaleLayout) vkDestroyPipelineLayout(dev, upscaleLayout, nullptr);
    if (scenePass) vkDestroyRenderPass(dev, scenePass, nullptr);
    if (renderPass) vkDestroyRenderPass(dev, renderPass, nullptr);
    trianglePipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    upscalePipeline = VK_NULL_HANDLE;
    upscaleLayout = VK_NULL_HANDLE;
    scenePass = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    renderPassColorFormat = VK_FORMAT_UNDEFINED;
}

Result<void> Renderer::Impl::createSwapchainResources() {
    VkDevice dev = device.device();
    const VkExtent2D extent = swapchain.extent();

    VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = depthFormat;
    depthInfo.extent = {extent.width, extent.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = samples; // phase 18 (world): the main pass's
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Tiled GPUs back transient attachments with lazily allocated memory: no DRAM is ever committed.
    // Desktop GPUs have no such memory type, so fall back to regular device memory.
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    bool lazy = true;
    if (vmaCreateImage(device.allocator(), &depthInfo, &allocInfo, &depthImage, &depthAllocation, nullptr) !=
        VK_SUCCESS) {
        lazy = false;
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VD_VK_TRY(vmaCreateImage(device.allocator(), &depthInfo, &allocInfo, &depthImage, &depthAllocation, nullptr));
    }
    if (!loggedDepthMemory) {
        VD_LOG_INFO("Depth buffer: format %d, %s memory", static_cast<int>(depthFormat),
                    lazy ? "lazily allocated (on-tile)" : "device");
        loggedDepthMemory = true;
    }

    VkImageViewCreateInfo depthViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthViewInfo.image = depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depthFormat;
    const VkImageAspectFlags aspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    depthViewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    VD_VK_TRY(vkCreateImageView(dev, &depthViewInfo, nullptr, &depthView));

    if (samples != VK_SAMPLE_COUNT_1_BIT) { // phase 18 (world): the multisampled color, never out of the tile
        Result<vk::TransientImage> color = vk::createTransientImage(device, swapchain.format(), extent, samples, false);
        if (!color) {
            return color.error();
        }
        multisampledColor = color.value();
    }
    const u32 imageCount = swapchain.imageCount();
    framebuffers.resize(imageCount, VK_NULL_HANDLE);
    renderFinished.resize(imageCount, VK_NULL_HANDLE);
    for (u32 i = 0; i < imageCount; ++i) {
        std::array<VkImageView, 3> views{};
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount =
            vk::worldPassViews(samples, multisampledColor.view, depthView, swapchain.imageView(i), views);
        framebufferInfo.pAttachments = views.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VD_VK_TRY(vkCreateFramebuffer(dev, &framebufferInfo, nullptr, &framebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VD_VK_TRY(vkCreateSemaphore(dev, &semaphoreInfo, nullptr, &renderFinished[i]));
    }

    VD_TRY(createSceneTarget());

    const VkExtent2D logical = swapchain.logicalExtent();
    stats.width = logical.width;
    stats.height = logical.height;
    f32 rotation[4];
    swapchain.preRotation(rotation);
    stats.rotationDegrees = rotationDegrees(rotation);
    return {};
}

void Renderer::Impl::destroySwapchainResources() {
    VkDevice dev = device.device();
    destroySceneTarget();
    for (VkFramebuffer framebuffer : framebuffers) {
        if (framebuffer) vkDestroyFramebuffer(dev, framebuffer, nullptr);
    }
    for (VkSemaphore semaphore : renderFinished) {
        if (semaphore) vkDestroySemaphore(dev, semaphore, nullptr);
    }
    framebuffers.clear();
    renderFinished.clear();
    vk::destroyTransientImage(device, multisampledColor); // phase 18 (world)
    if (depthView) vkDestroyImageView(dev, depthView, nullptr);
    if (depthImage) vmaDestroyImage(device.allocator(), depthImage, depthAllocation);
    depthView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthAllocation = VK_NULL_HANDLE;
}

Result<void> Renderer::Impl::rebuildSwapchain(bool force) {
    if (!force && !swapchain.isStale(device)) {
        return {};
    }
    // Rare event (resize, fold/unfold, rotation): a full idle is simpler and safe.
    vkDeviceWaitIdle(device.device());
    destroySwapchainResources();

    Result<bool> built = swapchain.build(device);
    if (!built) {
        return built.error();
    }
    if (!built.value()) {
        return {}; // zero-sized surface; retried on the next frame
    }
    if (swapchain.format() != renderPassColorFormat) {
        destroyRenderPassAndPipeline();
        VD_TRY(createRenderPassAndPipeline(swapchain.format()));
    }
    return createSwapchainResources();
}

void Renderer::Impl::readTimestamps(FrameSlot& frame, u32 slot) {
    if (!frame.timestampsPending) {
        return;
    }
    frame.timestampsPending = false;
    std::array<u64, 2> ticks{};
    if (vkGetQueryPoolResults(device.device(), timestampPool, slot * 2, 2, sizeof(ticks), ticks.data(),
                              sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const u64 elapsed = (ticks[1] - ticks[0]) & timestampMask;
        stats.gpuFrameMs = static_cast<f32>(static_cast<f64>(elapsed) * timestampPeriodNs / 1.0e6);
    }
}

void Renderer::Impl::recordFrame(FrameSlot& frame, u32 slot, u32 imageIndex, const FrameParams& params) {
    VkCommandBuffer cmd = frame.commandBuffer;
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    if (timestampPool) {
        vkCmdResetQueryPool(cmd, timestampPool, slot * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool, slot * 2);
    }
    // Streamed uploads first: this frame's draws may already use them. Then the textures composed from them.
    uploader.record(cmd);
    compositor.record(cmd, submittedFrames + 1);
    models.recordBoneCopy(cmd, slot);
    models.recordDoodadBoneCopy(cmd, slot); // phase 14
    // The unit frames' portraits asked this frame: their own small passes, with the actors' pipelines.
    models.setUvTransforms(params.uvTransforms, params.actorUvTransforms);
    RenderStats portraitStats;
    portraits.record(device, cmd, slot, params.portraits, params.portraitActors, params.actorGeosets,
                     params.actorColors, models, portraitStats);
    RenderStats modelViewStats; // phase 15: the interface's model frames, the same way
    modelViews.record(device, cmd, slot, params.modelViews, params.portraitActors, params.actorGeosets,
                      params.actorColors, models, modelViewStats);

    f32 preRotate[4];
    swapchain.preRotation(preRotate);
    const bool drawWorld = terrain.ready(); // a map is loaded (even if every tile is culled)
    if (drawWorld) {
        glm::mat4 rotation(1.0f);
        rotation[0][0] = preRotate[0];
        rotation[0][1] = preRotate[1];
        rotation[1][0] = preRotate[2];
        rotation[1][1] = preRotate[3];
        const SceneLighting& light = params.lighting;
        FrameUniforms uniforms;
        uniforms.viewProj = rotation * params.viewProj;
        uniforms.lightDirection = glm::vec4(light.lightDirection, 0.0f);
        uniforms.directColor = glm::vec4(light.directColor, 1.0f);
        uniforms.ambientColor = glm::vec4(light.ambientColor, 1.0f);
        uniforms.fogColor = glm::vec4(light.fogColor, 1.0f);
        uniforms.fogParams = glm::vec4(light.fogStart, 1.0f / std::max(1.0f, light.fogEnd - light.fogStart),
                                       params.timeSeconds, light.fogEnd);
        for (u32 i = 0; i < 5; ++i) {
            uniforms.sky[i] = glm::vec4(light.sky[i], 1.0f);
        }
        uniforms.riverClose = glm::vec4(light.riverClose, 1.0f);
        uniforms.riverFar = glm::vec4(light.riverFar, 1.0f);
        uniforms.oceanClose = glm::vec4(light.oceanClose, 1.0f);
        uniforms.oceanFar = glm::vec4(light.oceanFar, 1.0f);
        uniforms.waterAlphas = light.waterAlphas;
        uniforms.shadowColor = glm::vec4(light.shadowColor, 1.0f);
        uniforms.sunColor = glm::vec4(light.sunColor, light.celestialGlow);
        uniforms.sunHalo = glm::vec4(light.sunHalo, light.cloudDensity);
        uniforms.cloudEmissive = glm::vec4(light.cloudEmissive, light.glow);
        uniforms.cloudLayer1 = glm::vec4(light.cloudLayer1, light.storm);
        uniforms.cloudLayer2 = glm::vec4(light.cloudLayer2, light.dayFraction);
        uniforms.conditions = glm::vec4(light.underwater ? 1.0f : 0.0f, light.dead ? 1.0f : 0.0f, 0.0f, 0.0f);
        uniforms.screenFx = glm::vec4(light.screenGlow, light.screenDeath, 0.0f, 0.0f);
        uniforms.surfaceLight = glm::vec4(light.nightGlow, light.terrainHighlights ? 1.0f : 0.0f,
                                          terrain.specularMasks() ? 1.0f : 0.0f, 0.0f);
        std::memcpy(frameUniformsMapped + frameUniformsStride * slot, &uniforms, sizeof(uniforms));
        vmaFlushAllocation(device.allocator(), frameUniformsAllocation, frameUniformsStride * slot, sizeof(uniforms));
        weather.prepare(device, slot, params.weather); // phase 11
    }

    const VkExtent2D extent = swapchain.extent();
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = kClearColor;
    if (drawWorld) {
        // Phase 11: graded like everything the world shaders draw (shaders/screenfx.glsl).
        const glm::vec3 fog = glm::clamp(
            applyScreenFx(params.lighting.fogColor, {params.lighting.screenGlow, params.lighting.screenDeath}), 0.0f,
            1.0f);
        clearValues[0].color = {{fog.x, fog.y, fog.z, 1.0f}};
    }
    clearValues[1].depthStencil = {1.0f, 0};

    stats.drawCalls = 0;
    stats.triangles = 0;
    stats.wmoDrawCalls = 0;
    stats.wmoTriangles = 0;
    stats.m2DrawCalls = 0;
    stats.m2Triangles = 0;
    stats.actorDrawCalls = 0;
    stats.actorTriangles = 0;
    stats.uiDrawCalls = 0;
    stats.uiQuads = 0;
    stats.particleDrawCalls = 0;
    stats.particleQuads = 0;
    stats.weatherDrawCalls = 0;
    stats.weatherVertices = 0;
    stats.grassDrawCalls = 0; // phase 14
    stats.grassInstances = 0;
    stats.grassTriangles = 0;
    // The portraits' passes, recorded above.
    stats.drawCalls += portraitStats.drawCalls;
    stats.triangles += portraitStats.triangles;
    stats.actorDrawCalls += portraitStats.actorDrawCalls;
    stats.actorTriangles += portraitStats.actorTriangles;
    stats.portraitRenders = portraitStats.portraitRenders;
    stats.drawCalls += modelViewStats.drawCalls; // phase 15
    stats.triangles += modelViewStats.triangles;
    stats.actorDrawCalls += modelViewStats.actorDrawCalls;
    stats.actorTriangles += modelViewStats.actorTriangles;
    stats.modelViewRenders = modelViewStats.modelViewRenders;
    const VkExtent2D logical = swapchain.logicalExtent();

    // Phase 10: below full resolution the world goes first into the top-left part of the scene image (same aspect
    // as the framebuffer: the projection and its pre-rotation are unchanged), which becomes a texture for the main
    // pass. On a tiled GPU the render area is what gets binned: fewer pixels, fewer tiles.
    const bool scaled = drawWorld && renderScale < 1.0f && sceneFramebuffer != VK_NULL_HANDLE;
    VkExtent2D sceneExtent = extent;
    if (scaled) {
        sceneExtent.width = std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(extent.width) * renderScale)));
        sceneExtent.height = std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(extent.height) * renderScale)));
        VkImageMemoryBarrier toTarget{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTarget.srcAccessMask = 0; // the last frame's upscale only read it: an execution dependency is enough
        toTarget.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTarget.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTarget.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toTarget.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTarget.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTarget.image = sceneImage;
        toTarget.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTarget);
        VkRenderPassBeginInfo sceneInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        sceneInfo.renderPass = scenePass;
        sceneInfo.framebuffer = sceneFramebuffer;
        sceneInfo.renderArea = {{0, 0}, sceneExtent};
        sceneInfo.clearValueCount = static_cast<u32>(clearValues.size());
        sceneInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(cmd, &sceneInfo, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport sceneViewport{0.0f, 0.0f, static_cast<f32>(sceneExtent.width),
                                       static_cast<f32>(sceneExtent.height), 0.0f, 1.0f};
        const VkRect2D sceneScissor{{0, 0}, sceneExtent};
        vkCmdSetViewport(cmd, 0, 1, &sceneViewport);
        vkCmdSetScissor(cmd, 0, 1, &sceneScissor);
        drawScene(cmd, slot, params);
        vkCmdEndRenderPass(cmd);
        VkImageMemoryBarrier toTexture = toTarget;
        toTexture.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTexture.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTexture.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toTexture.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTexture);
    }
    stats.renderScale = scaled ? renderScale : 1.0f;
    stats.sceneWidth = sceneExtent.width;
    stats.sceneHeight = sceneExtent.height;

    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = renderPass;
    passInfo.framebuffer = framebuffers[imageIndex];
    passInfo.renderArea = {{0, 0}, extent};
    passInfo.clearValueCount = static_cast<u32>(clearValues.size());
    passInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width), static_cast<f32>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (scaled) {
        // The scene stretched over the whole framebuffer, the UI drawn over it at full resolution.
        const f32 imageWidth = static_cast<f32>(extent.width);
        const f32 imageHeight = static_cast<f32>(extent.height);
        UpscalePushConstants push{};
        push.uvScale[0] = static_cast<f32>(sceneExtent.width) / imageWidth;
        push.uvScale[1] = static_cast<f32>(sceneExtent.height) / imageHeight;
        push.uvScale[2] = (static_cast<f32>(sceneExtent.width) - 0.5f) / imageWidth;
        push.uvScale[3] = (static_cast<f32>(sceneExtent.height) - 0.5f) / imageHeight;
        push.texel[0] = 1.0f / imageWidth;
        push.texel[1] = 1.0f / imageHeight;
        push.texel[2] = kUpscaleSharpness;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscalePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscaleLayout, 0, 1, &upscaleSet, 0, nullptr);
        vkCmdPushConstants(cmd, upscaleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        ++stats.drawCalls;
        ++stats.triangles;
    } else if (drawWorld) {
        drawScene(cmd, slot, params);
    } else {
        // No world yet: the phase 0 smoke test. It swings around "up" instead of spinning, so a wrong
        // pre-rotation is visible.
        TrianglePushConstants push{};
        std::memcpy(push.preRotate, preRotate, sizeof(preRotate));
        const f32 aspect = static_cast<f32>(logical.width) / static_cast<f32>(logical.height);
        push.aspectScale[0] = aspect > 1.0f ? 1.0f / aspect : 1.0f;
        push.aspectScale[1] = aspect > 1.0f ? 1.0f : aspect;
        push.angle = 0.6f * std::sin(params.timeSeconds * 2.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        stats.drawCalls = 1;
        stats.triangles = 1;
    }

    ui.draw(device, cmd, slot, params.uiVertices, params.uiCommands, extent, logical, preRotate, models, stats);
#if !defined(VD_DIST)
    if (uiFrameActive) {
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (preRotate[0] != 1.0f) { // not identity
            preRotateDrawData(drawData, preRotate, logical, extent);
        }
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
        uiFrameActive = false;
    }
#endif
    vkCmdEndRenderPass(cmd);

    if (timestampPool) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool, slot * 2 + 1);
        frame.timestampsPending = true;
    }
    vkEndCommandBuffer(cmd);
}

// The world: opaque first (front to back), then the sky fills whatever is left at depth 1, then blended surfaces over
// both. Into the main pass, or the scene pass below full resolution.
void Renderer::Impl::drawScene(VkCommandBuffer cmd, u32 slot, const FrameParams& params) {
    models.setUvTransforms(params.uvTransforms, params.actorUvTransforms);
    terrain.draw(cmd, frameSets[slot], params.terrain, stats);
    terrain.drawLiquids(cmd, frameSets[slot], params.terrain, true, stats);
    models.draw(cmd, frameSets[slot], terrain.materialSet(), slot, params.models, params.modelGroups, false, stats);
    models.drawActors(cmd, frameSets[slot], slot, params.actors, params.actorGeosets, params.actorColors, false,
                      stats);
    // Phase 14 (bodyfx): the units' blob shadows and footprints on the opaque ground, under the grass.
    particles.draw(cmd, frameSets[slot], slot, params.groundDecals, models, stats);
    grass.draw(cmd, frameSets[slot], slot, params, models, stats); // phase 14: alpha tested, after the opaque world
    horizon.draw(cmd, frameSets[slot], slot, params.horizon, device, stats);
    sky.draw(cmd, frameSets[slot], params.sky, models, stats);
    terrain.drawLiquids(cmd, frameSets[slot], params.terrain, false, stats);
    models.draw(cmd, frameSets[slot], terrain.materialSet(), slot, params.models, params.modelGroups, true, stats);
    models.drawActors(cmd, frameSets[slot], slot, params.actors, params.actorGeosets, params.actorColors, true,
                      stats);
    particles.draw(cmd, frameSets[slot], slot, params.particles, models, stats);
    weather.draw(cmd, frameSets[slot], slot, stats); // phase 11: the nearest of all, mostly
    sky.drawGlare(cmd, frameSets[slot], params.sky, models, stats); // phase 11: over the whole scene
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

Result<void> Renderer::init(const PlatformInfo& platform) {
    m_impl = std::make_unique<Impl>();
    Impl& r = *m_impl;
    r.platform = platform;

    vk::DeviceOptions options;
#if !defined(NDEBUG)
    options.enableValidation = true;
#endif
    if (!platform.cacheDir.empty()) {
        options.pipelineCachePath = platform.cacheDir + "/pipeline_cache.bin";
    }
    VD_TRY(r.device.create(options));

    r.depthFormat = chooseDepthFormat(r.device.physicalDevice());
    if (r.depthFormat == VK_FORMAT_UNDEFINED) {
        return Error{"No supported depth format"};
    }

#if !defined(VD_DIST)
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // never write imgui.ini (and nothing to persist on the device)
    io.Fonts->AddFontDefault();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(platform.contentScale);
    style.FontScaleDpi = platform.contentScale;
#endif

    VD_TRY(r.createFrameResources());
    VD_TRY(r.createSceneResources());
    // Texture slots for the portraits' images (registered once the main render pass gives them a format).
    for (u32& index : r.portraitTextures) {
        index = static_cast<u32>(r.textures.size());
        r.textures.emplace_back().used = true;
    }
    for (u32& index : r.modelViewTextures) { // phase 15
        index = static_cast<u32>(r.textures.size());
        r.textures.emplace_back().used = true;
    }
    return {};
}

Result<TextureHandle> Renderer::createTexture(const u8* blob, usize size, TextureInfo* info, bool debugUiVisible) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Impl& r = *m_impl;
    const Clock::time_point start = Clock::now();
    Result<vk::Texture> created = vk::createTextureFromBlob(r.device, blob, size);
    if (!created) {
        return created.error();
    }
    u32 index = 0;
    if (!r.freeTextureSlots.empty()) {
        index = r.freeTextureSlots.back();
        r.freeTextureSlots.pop_back();
    } else {
        index = static_cast<u32>(r.textures.size());
        r.textures.emplace_back();
    }
    Impl::TextureSlot& slot = r.textures[index];
    slot.texture = created.value();
    slot.used = true;
    r.models.registerTexture(r.device, index, slot.texture.view); // phase 11: the sky draws them too
#if !defined(VD_DIST)
    if (r.imguiBackendReady && debugUiVisible) {
        slot.imguiSet = ImGui_ImplVulkan_AddTexture(slot.texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
#endif
    if (info) {
        info->width = slot.texture.width;
        info->height = slot.texture.height;
        info->mipCount = slot.texture.mipCount;
        info->gpuBytes = slot.texture.gpuBytes;
        info->nativeAstc = !slot.texture.decodedOnCpu && slot.texture.format != VK_FORMAT_R8G8B8A8_UNORM;
        info->uploadMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    }
    return TextureHandle{index};
}

void Renderer::destroyTexture(TextureHandle handle) {
    if (!m_impl || !handle.valid() || handle.index >= m_impl->textures.size() || !m_impl->textures[handle.index].used) {
        return;
    }
    // Frames already submitted may still sample it: release once they have all completed.
    m_impl->textures[handle.index].used = false;
    m_impl->retiredTextures.push_back({handle.index, m_impl->submittedFrames + kFramesInFlight});
}

void Renderer::clearTerrainMap() {
    if (!m_impl) {
        return;
    }
    // Phase 18 (world): retired, not waited for. Copies still queued for them are recorded into the next frame,
    // before the retired objects are freed.
    m_impl->terrain.retireMap(m_impl->submittedFrames + kFramesInFlight);
    m_impl->horizon.retire(m_impl->submittedFrames + kFramesInFlight);
}

Result<void> Renderer::setTerrainGeometry(std::span<const u16> sharedIndices, std::span<const u32> sharedLod,
                                          std::span<const u32> horizonHeights, std::span<const u16> horizonIndices) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Impl& r = *m_impl;
    VD_TRY(r.terrain.setSharedIndices(r.device, r.uploader, sharedIndices.data(), static_cast<u32>(sharedIndices.size()),
                                      sharedLod));
    return r.horizon.setData(r.device, r.uploader, horizonHeights, horizonIndices);
}

Result<void> Renderer::setTerrainTextures(const StagingBlock& block, u64 terrainOffset,
                                          const pack::TextureHeader& terrain, u64 liquidOffset,
                                          const pack::TextureHeader* liquids) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size()) {
        return Error{"Bad staging block for the terrain textures"};
    }
    Impl& r = *m_impl;
    const Impl::StagingSlot& slot = r.stagingBlocks[block.id];
    vmaFlushAllocation(r.device.allocator(), slot.allocation, 0, VK_WHOLE_SIZE); // written by another thread
    return r.terrain.setTextures(r.device, r.uploader, slot.buffer, terrainOffset, terrain, liquidOffset, liquids);
}

bool Renderer::nativeAstc() const {
    return m_impl && m_impl->device.astcSupported();
}

Result<TerrainTileHandle> Renderer::createTerrainTile(const StagingBlock& block, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || size > block.size) {
        return Error{"Bad staging block for a terrain tile"};
    }
    const Impl::StagingSlot& slot = m_impl->stagingBlocks[block.id];
    const TerrainRenderer::BlobSource source{slot.buffer, slot.allocation};
    Result<u32> id = m_impl->terrain.createTile(m_impl->device, m_impl->uploader, block.data, size, &source);
    if (!id) {
        return id.error();
    }
    return TerrainTileHandle{id.value()};
}

Result<TextureHandle> Renderer::createTexture(const StagingBlock& block, u64 offset, const u8* blob, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || offset + size > block.size) {
        return Error{"Bad staging block for a texture"};
    }
    Impl& r = *m_impl;
    Result<vk::Texture> created =
        vk::createTextureStreamed(r.device, r.uploader, r.stagingBlocks[block.id].buffer, offset, blob, size);
    if (!created) {
        return created.error();
    }
    u32 index = 0;
    if (!r.freeTextureSlots.empty()) {
        index = r.freeTextureSlots.back();
        r.freeTextureSlots.pop_back();
    } else {
        index = static_cast<u32>(r.textures.size());
        r.textures.emplace_back();
    }
    Impl::TextureSlot& slot = r.textures[index];
    slot.texture = created.value();
    slot.used = true;
    r.models.registerTexture(r.device, index, slot.texture.view);
    return TextureHandle{index};
}

Result<ModelHandle> Renderer::createModel(const StagingBlock& block, u64 offset, const u8* blob, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || offset + size > block.size) {
        return Error{"Bad staging block for a model"};
    }
    Impl& r = *m_impl;
    Result<u32> id = r.models.createModel(r.device, r.uploader, r.stagingBlocks[block.id].buffer, offset, blob, size);
    if (!id) {
        return id.error();
    }
    return ModelHandle{id.value()};
}

Result<ModelHandle> Renderer::createActorModel(const StagingBlock& block, u64 offset, const u8* blob, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || offset + size > block.size) {
        return Error{"Bad staging block for an actor model"};
    }
    Impl& r = *m_impl;
    Result<u32> id =
        r.models.createActorModel(r.device, r.uploader, r.stagingBlocks[block.id].buffer, offset, blob, size);
    if (!id) {
        return id.error();
    }
    return ModelHandle{id.value()};
}

Result<ActorMeshHandle> Renderer::createActorMesh(ModelHandle model, std::span<const u16> indices,
                                                  std::span<const ActorMeshRun> runs) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Result<u32> id = m_impl->models.createActorMesh(m_impl->device, m_impl->uploader, model.index, indices, runs);
    if (!id) {
        return id.error();
    }
    return ActorMeshHandle{id.value()};
}

void Renderer::destroyActorMesh(ActorMeshHandle mesh) {
    if (m_impl && mesh.valid()) {
        m_impl->models.destroyActorMesh(mesh.index, m_impl->submittedFrames + kFramesInFlight);
    }
}

Result<TextureHandle> Renderer::composeTexture(u32 width, u32 height, std::span<const ComposeLayer> layers) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Impl& r = *m_impl;
    std::array<TextureCompositor::Layer, 32> compose{};
    u32 count = 0;
    for (const ComposeLayer& layer : layers) {
        if (count == compose.size() || !layer.texture.valid() || layer.texture.index >= r.textures.size() ||
            !r.textures[layer.texture.index].used) {
            continue;
        }
        compose[count++] = {r.models.textureSet(layer.texture.index, true),
                            {layer.x, layer.y, layer.width, layer.height}};
    }
    Result<vk::Texture> created = r.compositor.compose(r.device, width, height, {compose.data(), count});
    if (!created) {
        return created.error();
    }
    u32 index = 0;
    if (!r.freeTextureSlots.empty()) {
        index = r.freeTextureSlots.back();
        r.freeTextureSlots.pop_back();
    } else {
        index = static_cast<u32>(r.textures.size());
        r.textures.emplace_back();
    }
    Impl::TextureSlot& slot = r.textures[index];
    slot.texture = created.value();
    slot.used = true;
    r.models.registerTexture(r.device, index, slot.texture.view);
    return TextureHandle{index};
}

std::span<anim::BoneMatrix> Renderer::boneMatrices() {
    if (!m_impl) {
        return {};
    }
    return m_impl->models.boneMatrices(static_cast<u32>(m_impl->submittedFrames % ModelRenderer::kBoneBuffers));
}

std::span<anim::BoneMatrix> Renderer::doodadBoneMatrices() {
    if (!m_impl) {
        return {};
    }
    return m_impl->models.doodadBoneMatrices(static_cast<u32>(m_impl->submittedFrames % ModelRenderer::kBoneBuffers));
}

std::span<ParticleVertex> Renderer::particleVertices() {
    if (!m_impl) {
        return {};
    }
    return m_impl->particles.vertices(static_cast<u32>(m_impl->submittedFrames % ParticleRenderer::kBuffers));
}

void Renderer::setModelTexture(ModelHandle model, u32 material, TextureHandle texture) {
    if (m_impl) {
        m_impl->models.setMaterialTexture(model.index, material, texture.index);
    }
}

void Renderer::destroyModel(ModelHandle model) {
    if (m_impl && model.valid()) {
        m_impl->models.destroyModel(model.index, m_impl->submittedFrames + kFramesInFlight);
    }
}

u32 Renderer::modelCount() const {
    return m_impl ? m_impl->models.modelCount() : 0;
}

u64 Renderer::modelGpuBytes() const {
    return m_impl ? m_impl->models.gpuBytes() : 0;
}

Result<void> Renderer::setUiTextures(const u8* atlas, usize atlasSize, const u8* fonts, usize fontsSize) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    return m_impl->ui.setTextures(m_impl->device, atlas, atlasSize, fonts, fontsSize);
}

bool Renderer::setUiIcon(u32 slot, const StagingBlock& block, u64 offset, const u8* blob, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || offset + size > block.size) {
        return false;
    }
    Impl& r = *m_impl;
    return r.ui.setIcon(r.device, r.uploader, slot, r.stagingBlocks[block.id].buffer, offset, blob, size);
}

TextureHandle Renderer::portraitTexture(u32 slot) const {
    if (!m_impl || slot >= kPortraitSlots || !m_impl->portraits.sampledView(slot)) {
        return {};
    }
    return TextureHandle{m_impl->portraitTextures[slot]};
}

u32 Renderer::portraitGeneration() const {
    return m_impl ? m_impl->portraits.generation() : 0;
}

TextureHandle Renderer::modelViewTexture(u32 slot) const {
    if (!m_impl || slot >= kModelViewSlots || !m_impl->modelViews.sampledView(slot)) {
        return {};
    }
    return TextureHandle{m_impl->modelViewTextures[slot]};
}

u32 Renderer::modelViewGeneration() const {
    return m_impl ? m_impl->modelViews.generation() : 0;
}

Result<void> Renderer::setGrassModels(const StagingBlock& block, u64 offset, const u8* blob, usize size) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size() || offset + size > block.size) {
        return Error{"Bad staging block for the grass models"};
    }
    Impl& r = *m_impl;
    return r.grass.setModels(r.device, r.uploader, r.stagingBlocks[block.id].buffer, offset, blob, size);
}

Result<void> Renderer::setWeatherTextures(std::span<const std::span<const u8>> blobs) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    return m_impl->weather.setTextures(m_impl->device, blobs);
}

Result<SkyModelHandle> Renderer::createSkyModel(const u8* blob, usize size,
                                                std::span<const TextureHandle> partTextures) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    std::array<u32, 64> slots{};
    const usize count = std::min(partTextures.size(), slots.size());
    for (usize i = 0; i < count; ++i) {
        slots[i] = partTextures[i].index;
    }
    Result<u32> model =
        m_impl->sky.createModel(m_impl->device, blob, size, std::span<const u32>(slots.data(), count));
    if (!model) {
        return model.error();
    }
    return SkyModelHandle{model.value()};
}

u32 Renderer::uiQuadCapacity() const {
    return UiRenderer::kMaxQuads;
}

Result<StagingBlock> Renderer::createStagingBlock(u64 size) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Impl& r = *m_impl;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // Written sequentially (memcpy) and never read back by the CPU: write-combined memory is fine.
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Impl::StagingSlot slot;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(r.device.allocator(), &bufferInfo, &allocInfo, &slot.buffer, &slot.allocation, &info));
    slot.mapped = static_cast<u8*>(info.pMappedData);
    slot.size = size;
    u32 id = 1;
    while (id < r.stagingBlocks.size() && r.stagingBlocks[id].buffer) {
        ++id;
    }
    if (id == r.stagingBlocks.size()) {
        r.stagingBlocks.emplace_back();
    }
    r.stagingBlocks[id] = slot;
    return StagingBlock{id, slot.mapped, size};
}

void Renderer::retireStagingBlock(StagingBlock& block) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size()) {
        return;
    }
    // Copies recorded from it go into the next frame: free once that one has completed.
    m_impl->retiredStaging.push_back({block.id, m_impl->submittedFrames + kFramesInFlight});
    block = {};
}

void Renderer::destroyStagingBlock(StagingBlock& block) {
    if (!m_impl || !block.valid() || block.id >= m_impl->stagingBlocks.size()) {
        return;
    }
    Impl& r = *m_impl;
    Impl::StagingSlot& slot = r.stagingBlocks[block.id];
    if (slot.buffer) {
        vkDeviceWaitIdle(r.device.device());
        vmaDestroyBuffer(r.device.allocator(), slot.buffer, slot.allocation);
    }
    slot = {};
    block = {};
}

u64 Renderer::frameNumber() const {
    return m_impl ? m_impl->submittedFrames : 0;
}

u64 Renderer::completedFrames() const {
    return m_impl ? m_impl->completedFrames : 0;
}

Result<TerrainTileHandle> Renderer::createTerrainTile(const u8* blob, usize size) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Result<u32> id = m_impl->terrain.createTile(m_impl->device, m_impl->uploader, blob, size);
    if (!id) {
        return id.error();
    }
    return TerrainTileHandle{id.value()};
}

void Renderer::destroyTerrainTile(TerrainTileHandle handle) {
    if (m_impl && handle.valid()) {
        // Frames already submitted may still draw it.
        m_impl->terrain.destroyTile(handle.index, m_impl->submittedFrames + kFramesInFlight);
    }
}

u32 Renderer::terrainTileCount() const {
    return m_impl ? m_impl->terrain.tileCount() : 0;
}

u64 Renderer::terrainGpuBytes() const {
    return m_impl ? m_impl->terrain.gpuBytes() + m_impl->horizon.gpuBytes() : 0;
}

void Renderer::releaseRetired() {
    if (!m_impl) {
        return;
    }
    // Everything waiting for the frames in flight: once the GPU is idle none of it is in use.
    Impl& r = *m_impl;
    vkDeviceWaitIdle(r.device.device());
    r.collectRetiredTextures(true);
    r.collectRetiredStaging(true); // phase 18 (world)
    r.horizon.collect(r.device, r.submittedFrames, true);
    r.terrain.collect(r.device, r.submittedFrames, true);
    r.models.collect(r.device, r.submittedFrames, true);
    r.compositor.collect(r.device, r.completedFrames, true);
}

u64 Renderer::gpuMemoryBytes() const {
    if (!m_impl) {
        return 0;
    }
    // VMA's own bookkeeping (no driver query): the memory blocks it holds in every heap.
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(m_impl->device.allocator(), budgets.data());
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(m_impl->device.allocator(), &memory);
    u64 bytes = 0;
    for (u32 heap = 0; memory && heap < memory->memoryHeapCount; ++heap) {
        bytes += budgets[heap].statistics.blockBytes;
    }
    return bytes;
}

#if !defined(VD_DIST)
bool Renderer::debugUiHitTest(f32 x, f32 y) const {
    if (!ImGui::GetCurrentContext()) {
        return false;
    }
    // Windows with NoInputs (the HUD) are skipped, like ImGui's own hover test.
    ImGuiWindow* hovered = nullptr;
    ImGuiWindow* underMoving = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(x, y), false, &hovered, &underMoving);
    return hovered != nullptr;
}

bool Renderer::debugUiWantsMouse() const {
    return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

u64 Renderer::debugUiTextureId(TextureHandle handle) const {
    if (!m_impl || !handle.valid() || handle.index >= m_impl->textures.size()) {
        return 0;
    }
    return reinterpret_cast<u64>(m_impl->textures[handle.index].imguiSet);
}

void Renderer::debugUiPointer(f32 x, f32 y, bool down, bool touch) {
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
    if (touch && !down) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // a lifted finger hovers nothing
    }
}

void Renderer::debugUiWheel(f32 steps) {
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, steps);
    }
}

bool Renderer::beginDebugUi(f32 deltaSeconds) {
    if (!m_impl || !m_impl->imguiBackendReady || !m_impl->swapchain.valid()) {
        return false;
    }
    Impl& r = *m_impl;
    if (r.uiFrameActive) {
        ImGui::EndFrame(); // previous frame was never rendered (e.g. swapchain rebuild)
    }
    const VkExtent2D logical = r.swapchain.logicalExtent();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<f32>(logical.width), static_cast<f32>(logical.height));
    io.DeltaTime = std::max(deltaSeconds, 1.0e-4f);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    r.uiFrameActive = true;
    return true;
}
#else
// Phase 22: the published build has no debug overlay.
bool Renderer::debugUiHitTest(f32, f32) const {
    return false;
}

bool Renderer::debugUiWantsMouse() const {
    return false;
}

u64 Renderer::debugUiTextureId(TextureHandle) const {
    return 0;
}

void Renderer::debugUiPointer(f32, f32, bool, bool) {}

void Renderer::debugUiWheel(f32) {}

bool Renderer::beginDebugUi(f32) {
    return false;
}
#endif

void Renderer::shutdown() {
    if (!m_impl) {
        return;
    }
    Impl& r = *m_impl;
    if (r.device.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(r.device.device());
        r.collectRetiredTextures(true);
        r.destroySceneResources();
        for (Impl::StagingSlot& slot : r.stagingBlocks) {
            if (slot.buffer) {
                vmaDestroyBuffer(r.device.allocator(), slot.buffer, slot.allocation);
            }
        }
        r.stagingBlocks.clear();
        for (u32 i = 1; i < r.textures.size(); ++i) {
            if (r.textures[i].used) {
                r.releaseTextureNow(i);
            }
        }
#if !defined(VD_DIST)
        if (r.imguiBackendReady) {
            if (r.uiFrameActive) {
                ImGui::EndFrame();
            }
            ImGui_ImplVulkan_Shutdown();
            r.imguiBackendReady = false;
        }
#endif
        detachSurface();
        r.destroyRenderPassAndPipeline();
        r.destroyFrameResources();
    }
#if !defined(VD_DIST)
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
#endif
    r.device.destroy();
    m_impl.reset();
}

Result<void> Renderer::attachSurface(const NativeSurface& surface) {
    if (!m_impl) {
        return Error{"Renderer not initialized"};
    }
    Impl& r = *m_impl;
    VD_TRY(r.swapchain.createSurface(r.device, r.platform, surface));
    r.surfaceAttached = true;
    r.rebuild = Rebuild::None;
    return r.rebuildSwapchain(true);
}

void Renderer::detachSurface() {
    if (!m_impl || !m_impl->surfaceAttached) {
        return;
    }
    Impl& r = *m_impl;
    vkDeviceWaitIdle(r.device.device());
    r.destroySwapchainResources();
    r.swapchain.destroySurface(r.device);
    r.surfaceAttached = false;
}

void Renderer::onSurfaceChanged(u32 width, u32 height) {
    if (!m_impl || !m_impl->surfaceAttached) {
        return;
    }
    Impl& r = *m_impl;
    r.swapchain.setNativeSize(width, height);
    if (r.rebuild == Rebuild::None) {
        r.rebuild = Rebuild::IfChanged;
    }
}

bool Renderer::renderFrame(const FrameParams& params) {
    if (!m_impl || !m_impl->surfaceAttached) {
        return false;
    }
    Impl& r = *m_impl;
    if (r.rebuild != Rebuild::None || !r.swapchain.valid()) {
        const bool force = r.rebuild == Rebuild::Force || !r.swapchain.valid();
        r.rebuild = Rebuild::None;
        if (Result<void> rebuilt = r.rebuildSwapchain(force); !rebuilt) {
            VD_LOG_ERROR("Swapchain rebuild failed: %s", rebuilt.error().message.c_str());
            return false;
        }
        if (!r.swapchain.valid()) {
            return false;
        }
    }

    r.applyQuality(); // phase 18 (world): an options change, at this safe point
    VkDevice dev = r.device.device();
    FrameSlot& frame = r.frames[r.frameIndex];
    const Clock::time_point waitStart = Clock::now();
    {
        VD_PROFILE_ZONE("Render::waitFence");
        vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    }
    r.readTimestamps(frame, r.frameIndex);
    r.portraits.readTimestamps(r.device, r.frameIndex, r.stats);
    r.modelViews.readTimestamps(r.device, r.frameIndex, r.stats); // phase 15
    // The fence belongs to the frame submitted kFramesInFlight frames ago (frames finish in order).
    if (r.submittedFrames + 1 > kFramesInFlight) {
        r.completedFrames = std::max(r.completedFrames, r.submittedFrames + 1 - kFramesInFlight);
    }
    r.uploader.frameCompleted(r.frameIndex);
    r.collectRetiredTextures(false);
    r.collectRetiredStaging(false); // phase 18 (world)
    r.horizon.collect(r.device, r.submittedFrames, false);
    r.terrain.collect(r.device, r.submittedFrames, false);
    r.models.collect(r.device, r.submittedFrames, false);
    r.compositor.collect(r.device, r.completedFrames, false);
    r.models.prepare(r.device, r.frameIndex, params.modelInstances);
    r.models.prepareActors(r.device, r.frameIndex, static_cast<u32>(r.submittedFrames % ModelRenderer::kBoneBuffers),
                           params.actorInstances, params.boneCount);
    r.models.prepareDoodadBones(r.device, r.frameIndex,
                                static_cast<u32>(r.submittedFrames % ModelRenderer::kBoneBuffers),
                                params.doodadBoneCount); // phase 14
    r.particles.prepare(r.device, r.frameIndex, static_cast<u32>(r.submittedFrames % ParticleRenderer::kBuffers),
                        params.particles.empty() && params.groundDecals.empty() ? 0 : params.particleQuads);
    r.grass.prepare(r.device, r.frameIndex, params.grassInstances); // phase 14

    u32 imageIndex = 0;
    VkResult acquired = VK_SUCCESS;
    {
        VD_PROFILE_ZONE("Render::acquire");
        acquired = r.swapchain.acquire(r.device, frame.imageAvailable, imageIndex);
    }
    f64 blockedSeconds = secondsBetween(waitStart, Clock::now());
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        r.rebuild = Rebuild::Force;
        return false;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        r.rebuild = Rebuild::IfChanged;
    } else if (acquired != VK_SUCCESS) {
        VD_LOG_ERROR("vkAcquireNextImageKHR: %s", vk::resultToString(acquired));
        return false;
    }

    // Only reset the fence once work is guaranteed to be submitted, or the next wait deadlocks.
    vkResetFences(dev, 1, &frame.inFlight);
    {
        VD_PROFILE_ZONE("Render::record");
        vkResetCommandPool(dev, frame.commandPool, 0);
        r.recordFrame(frame, r.frameIndex, imageIndex, params);
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r.renderFinished[imageIndex];
    VkResult submitted = VK_SUCCESS;
    {
        VD_PROFILE_ZONE("Render::submit");
        submitted = vkQueueSubmit(r.device.queue(), 1, &submit, frame.inFlight);
    }
    if (submitted != VK_SUCCESS) {
        VD_LOG_ERROR("vkQueueSubmit: %s", vk::resultToString(submitted));
        return false;
    }
    ++r.submittedFrames;
    r.uploader.frameSubmitted(r.frameIndex);

    const Clock::time_point presentStart = Clock::now();
    VkResult presented = VK_SUCCESS;
    {
        VD_PROFILE_ZONE("Render::present"); // Swappy sleeps in here to pace frames
        presented = r.swapchain.present(r.device, r.renderFinished[imageIndex], imageIndex);
    }
    blockedSeconds += secondsBetween(presentStart, Clock::now());
    r.stats.blockedMs = static_cast<f32>(blockedSeconds * 1000.0);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        r.rebuild = Rebuild::Force;
    } else if (presented == VK_SUBOPTIMAL_KHR) {
        if (r.rebuild == Rebuild::None) {
            r.rebuild = Rebuild::IfChanged;
        }
    } else if (presented != VK_SUCCESS) {
        VD_LOG_ERROR("present: %s", vk::resultToString(presented));
    }

    r.frameIndex = (r.frameIndex + 1) % kFramesInFlight;
    return true;
}

VkSampleCountFlagBits Renderer::Impl::supportedSamples(u32 wanted) const {
    const VkSampleCountFlags counts = device.framebufferSampleCounts();
    for (VkSampleCountFlagBits candidate : {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT}) {
        if (static_cast<u32>(candidate) <= wanted && (counts & candidate)) {
            return candidate;
        }
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

// Phase 18 (world): what the video options changed, once nothing in flight uses what goes: the samplers of the world's
// textures (their descriptor sets rewritten) and, for multisampling, the render passes, their pipelines (from the
// persisted pipeline cache: every variant seen once is a cache hit afterwards) and the swapchain-sized attachments.
// Like the client's own (OptionsFrame: anisotropic "requires restart", multisampling restarts the graphics), it holds
// one frame: it only ever happens when the player applies the options, never during play.
void Renderer::Impl::applyQuality() {
    const VkSampleCountFlagBits wantedSamples = supportedSamples(pendingSamples);
    const u32 wantedAnisotropy = std::clamp<u32>(pendingAnisotropy, 1, static_cast<u32>(device.maxAnisotropy()));
    const bool rebuildPasses = wantedSamples != samples && surfaceAttached && swapchain.valid();
    if (wantedAnisotropy == anisotropy && !rebuildPasses) {
        return;
    }
    VD_PROFILE_ZONE("Render::applyQuality");
    const Clock::time_point start = Clock::now();
    vkDeviceWaitIdle(device.device());
    if (wantedAnisotropy != anisotropy) {
        anisotropy = wantedAnisotropy;
        terrain.setAnisotropy(device, anisotropy);
        models.setAnisotropy(device, anisotropy);
    }
    if (rebuildPasses) {
        samples = wantedSamples;
        destroySwapchainResources();
        destroyRenderPassAndPipeline();
        if (Result<void> created = createRenderPassAndPipeline(swapchain.format()); !created) {
            VD_LOG_ERROR("Multisampling x%u: %s", static_cast<u32>(samples), created.error().message.c_str());
        } else if (Result<void> resources = createSwapchainResources(); !resources) {
            VD_LOG_ERROR("Multisampling x%u: %s", static_cast<u32>(samples), resources.error().message.c_str());
        }
    }
    VD_LOG_INFO("Video quality: anisotropic filtering x%u, multisampling x%u (applied in %.1f ms)", anisotropy,
                static_cast<u32>(samples), secondsBetween(start, Clock::now()) * 1000.0);
}

void Renderer::setAnisotropy(u32 level) {
    if (m_impl) {
        m_impl->pendingAnisotropy = std::max(1u, level);
    }
}

void Renderer::setMultisample(u32 samples) {
    if (m_impl) {
        m_impl->pendingSamples = std::max(1u, samples);
    }
}

u32 Renderer::maxAnisotropy() const {
    return m_impl ? static_cast<u32>(m_impl->device.maxAnisotropy()) : 1u;
}

u32 Renderer::maxMultisample() const {
    return m_impl ? static_cast<u32>(m_impl->supportedSamples(8)) : 1u;
}

void Renderer::setAlphaToCoverage(bool enabled) {
    if (m_impl) {
        m_impl->models.alphaToCoverage = enabled;
    }
}

u32 Renderer::multisample() const {
    return m_impl ? static_cast<u32>(m_impl->samples) : 1u;
}

void Renderer::setRenderScale(f32 scale) {
    if (m_impl) {
        // Whole percents: the scene's size only changes when the scale really does.
        const f32 clamped = std::clamp(scale, kMinRenderScale, 1.0f);
        m_impl->renderScale = std::round(clamped * 100.0f) / 100.0f;
    }
}

f32 Renderer::renderScale() const {
    return m_impl ? m_impl->renderScale : 1.0f;
}

void Renderer::setFrameInterval(u32 frames) {
    if (m_impl) {
        m_impl->swapchain.setFrameInterval(m_impl->device, frames);
        m_impl->stats.frameInterval = m_impl->swapchain.frameInterval();
    }
}

const RenderStats& Renderer::stats() const {
    return m_impl->stats;
}

} // namespace vd
