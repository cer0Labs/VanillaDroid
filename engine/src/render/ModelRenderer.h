#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "anim/BoneMatrix.h"
#include "render/Renderer.h"
#include "render/vk/Texture.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>
#include <vector>

namespace vd {

namespace vk {
class Device;
class Uploader;
} // namespace vk

// Static models (internal to render/): baked M2/WMO geometry in one buffer per model, instanced draws
// (one per run of parts and visible model), pipelines per blend mode created up front. Textures come from
// the Renderer's texture slots; each gets a descriptor set here (repeat and clamp samplers). WMO liquids
// use the terrain's liquid frames (its material set, `liquidLayout`).
class ModelRenderer {
public:
    static constexpr u32 kMaxInstances = 16384; // per frame
    static constexpr u32 kMaxTextures = 8192;
    static constexpr u32 kMaxFrames = 4;
    static constexpr u32 kMaxActorInstances = 4096; // per frame
    static constexpr u32 kMaxBones = 65536;         // bone matrices per frame (~3 MB)
    // Bone palettes are written by the game before renderFrame() waits for its frame's fence, so they cycle over
    // one more buffer than there are frames in flight.
    static constexpr u32 kBoneBuffers = 3;
    static constexpr u32 kMaxDoodadBones = 16384; // phase 14: animated doodads' matrices per frame (768 KB)

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout,
                      VkDescriptorSetLayout liquidLayout, u32 framesInFlight);
    void shutdown(const vk::Device& device);
    Result<void> createPipelines(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipelines(const vk::Device& device);

    // `src` holds the baked model blob at `srcOffset`; `blob` is a CPU copy (header, parts, materials).
    Result<u32> createModel(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 srcOffset,
                            const u8* blob, usize size);
    Result<u32> createActorModel(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 srcOffset,
                                 const u8* blob, usize size);
    Result<u32> createActorMesh(const vk::Device& device, vk::Uploader& uploader, u32 model, std::span<const u16> indices,
                                std::span<const ActorMeshRun> runs);
    void destroyActorMesh(u32 mesh, u64 retireFrame);
    void setMaterialTexture(u32 model, u32 material, u32 texture);
    void destroyModel(u32 id, u64 retireFrame);
    void collect(const vk::Device& device, u64 submittedFrames, bool everything);

    void registerTexture(const vk::Device& device, u32 texture, VkImageView view);
    void unregisterTexture(const vk::Device& device, u32 texture);
    // Phase 18 (world): the textures' samplers with this anisotropy (1: none), every texture set rewritten with them.
    // The GPU must be idle.
    void setAnisotropy(const vk::Device& device, u32 level);
    bool alphaToCoverage = true; // phase 18 (world): alpha-keyed pipelines' coverage when multisampled (next creation)

    // Once per frame, after the frame's fence: copies the instance transforms for `slot`.
    void prepare(const vk::Device& device, u32 slot, std::span<const ModelInstanceData> instances);
    // `liquidSet`: the terrain's material set; WMO liquids are skipped while it is null.
    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, VkDescriptorSet liquidSet, u32 slot,
              std::span<const ModelDraw> draws, std::span<const u16> groups, bool transparent,
              RenderStats& stats) const;

    // Mapped bone palette `buffer` (0..kBoneBuffers): write-only.
    std::span<anim::BoneMatrix> boneMatrices(u32 buffer) const;
    // Once per frame, after the frame's fence: actor instances for `slot`, and the used part of bone palette `buffer`.
    void prepareActors(const vk::Device& device, u32 slot, u32 buffer, std::span<const ActorInstanceData> instances,
                       u32 boneCount);
    // Copies the frame's bone palette into GPU memory (host-visible memory is slow to read from vertex shaders on
    // mobile GPUs). Outside a render pass, before any actor draw.
    void recordBoneCopy(VkCommandBuffer cmd, u32 slot) const;
    void drawActors(VkCommandBuffer cmd, VkDescriptorSet frameSet, u32 slot, std::span<const ActorDraw> draws,
                    std::span<const u16> geosets, std::span<const glm::vec4> colors, bool transparent,
                    RenderStats& stats) const;

    // Phase 14 (doodads): the animated doodads' bone palette, as the actors' (written by the game, copied each frame to
    // GPU memory before the scene; draw() binds it for ModelDraw::kDoodadSkinned).
    std::span<anim::BoneMatrix> doodadBoneMatrices(u32 buffer) const;
    void prepareDoodadBones(const vk::Device& device, u32 slot, u32 buffer, u32 boneCount);
    void recordDoodadBoneCopy(VkCommandBuffer cmd, u32 slot) const;

    // This frame's texture animations (FrameParams::uvTransforms / actorUvTransforms), read by draw() and
    // drawActors() through ModelDraw/ActorDraw::firstUvTransform. Valid until the frame is recorded.
    void setUvTransforms(std::span<const UvTransform> models, std::span<const UvTransform> actors) {
        m_modelUv = models;
        m_actorUv = actors;
    }

    VkDescriptorSetLayout textureLayout() const { return m_textureLayout; }
    // The texture's descriptor set (clamp or repeat sampler), or the placeholder's if it has none.
    VkDescriptorSet textureSet(u32 texture, bool clamp) const;

    u32 modelCount() const { return m_liveModels; }
    u64 gpuBytes() const { return m_gpuBytes; }

private:
    enum Pipeline : u8 {
        kOpaque,
        kOpaqueTwoSided,
        kAlphaKey,
        kAlphaKeyTwoSided,
        kAlpha,
        kAdd,
        kMod,
        kMod2x,
        kLiquidOpaque, // magma, slime
        kLiquid,       // water, ocean
        kActorOpaque,  // skinned variants of the first eight, same order
        kActorOpaqueTwoSided,
        kActorAlphaKey,
        kActorAlphaKeyTwoSided,
        kActorAlpha,
        kActorAdd,
        kActorMod,
        kActorMod2x,
        // Phase 11: the opaque and alpha-tested actor variants for translucent units (ActorDraw::fade): alpha blended
        // without writing depth, same order as kActorOpaque..kActorAlphaKeyTwoSided.
        kActorFade,
        kActorFadeTwoSided,
        kActorFadeAlphaKey,
        kActorFadeAlphaKeyTwoSided,
        // Phase 14: the first eight over skinned doodads (shaders/doodad.vert), same order.
        kDoodadOpaque,
        kDoodadOpaqueTwoSided,
        kDoodadAlphaKey,
        kDoodadAlphaKeyTwoSided,
        kDoodadAlpha,
        kDoodadAdd,
        kDoodadMod,
        kDoodadMod2x,
        kPipelineCount
    };
    struct Part {
        u32 firstIndex;
        u32 indexCount;
        u32 vertexOffset;
        u16 material;
        u16 group;
        u8 pipeline;
        u8 transparent;
        u8 interior;
    };
    struct Material {
        u32 texture = 0; // Renderer texture slot, 0 = placeholder
        u16 flags = 0;
        u8 blend = 0;
        u8 slot = 0; // actors: pack::ActorTextureSlot (0 = `texture`)
        u16 uvAnimation = 0; // 1 + the model's UV animation (pack::ModelMaterial / ActorMaterial), 0 = static
        f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // static color multiplier (actors; M2 kModelMaterialTinted)
        u32 emissive = 0; // phase 11: night glow color (RGBA8) of SIDN materials, 0 = none
    };
    struct Model {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u64 indexOffset = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        u64 gpuBytes = 0;
        std::vector<Part> parts;
        u32 groupCount = 0;
        std::vector<Material> materials;
        std::array<f32, 4> ambient{};
        bool used = false;
        bool actor = false; // skinned (ActorVertex); parts' `group` is the geoset index
        u64 skinOffset = 0; // phase 14: an animated doodad's pack::ModelSkinVertex stream in `buffer`, 0 = none
    };
    struct Retired {
        u32 id;
        u64 retireFrame;
    };
    struct MeshRun {
        u32 firstIndex;
        u32 indexCount;
        u16 material;
        u8 pipeline;
        u8 transparent;
    };
    struct Mesh {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32 model = 0;
        std::vector<MeshRun> runs;
        bool used = false;
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u8* mapped = nullptr;
    };

    void release(const vk::Device& device, u32 id);

    VkDescriptorSetLayout m_textureLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_liquidLayout = VK_NULL_HANDLE; // same set 0 and push range: set 0 stays bound
    VkPipelineLayout m_actorLayout = VK_NULL_HANDLE;  // + set 2: the bone palette
    VkDescriptorSetLayout m_boneLayout = VK_NULL_HANDLE;
    std::array<Buffer, kBoneBuffers> m_bones{};
    std::array<Buffer, kMaxFrames> m_actorInstances{};
    std::array<u32, kMaxFrames> m_actorInstanceCount{};
    std::array<u32, kMaxFrames> m_boneBufferOf{}; // bone palette used by each frame slot
    std::array<u32, kMaxFrames> m_boneCount{};
    std::array<VkBuffer, kMaxFrames> m_gpuBones{}; // device-local copy read by the vertex shader
    std::array<VmaAllocation, kMaxFrames> m_gpuBoneMemory{};
    std::array<VkDescriptorSet, kMaxFrames> m_gpuBoneSets{};
    // Phase 14: the animated doodads' palette, laid out like the actors'.
    std::array<Buffer, kBoneBuffers> m_doodadBones{};
    std::array<u32, kMaxFrames> m_doodadBoneBufferOf{};
    std::array<u32, kMaxFrames> m_doodadBoneCount{};
    std::array<VkBuffer, kMaxFrames> m_gpuDoodadBones{};
    std::array<VmaAllocation, kMaxFrames> m_gpuDoodadBoneMemory{};
    std::array<VkDescriptorSet, kMaxFrames> m_gpuDoodadBoneSets{};
    std::array<VkPipeline, kPipelineCount> m_pipelines{};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkSampler m_repeatSampler = VK_NULL_HANDLE;
    VkSampler m_clampSampler = VK_NULL_HANDLE;
    vk::Texture m_placeholder;
    std::vector<std::array<VkDescriptorSet, 2>> m_textureSets; // [texture slot] = {repeat, clamp}
    std::vector<VkImageView> m_textureViews;                     // phase 18 (world): what each set samples
    mutable std::vector<u64> m_groupMask; // draw() scratch: visible groups of one model, sized at createModel
    std::array<Buffer, kMaxFrames> m_instances{};
    std::array<u32, kMaxFrames> m_instanceCount{};
    u32 m_frames = 0;

    std::vector<Mesh> m_meshes{1}; // slot 0 reserved
    std::vector<u32> m_freeMeshes;
    std::vector<Retired> m_retiredMeshes;
    std::span<const UvTransform> m_modelUv;
    std::span<const UvTransform> m_actorUv;
    std::vector<Model> m_models{1}; // slot 0 reserved
    std::vector<u32> m_freeModels;
    std::vector<Retired> m_retired;
    u32 m_liveModels = 0;
    u64 m_gpuBytes = 0;
};

} // namespace vd
