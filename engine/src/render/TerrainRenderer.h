#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackFormat.h"
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

// Terrain pass (internal to render/): the map's texture arrays (terrain layers, liquid frames), and per
// tile a buffer (vertices + chunk table + optional own indices + liquid mesh), a 1024x1024 alpha texture
// and a descriptor set. Tiles upload through the Uploader (no stalls); destruction is deferred until the
// GPU is done. Liquids are drawn in two passes: opaque (magma, slime) and translucent (river, ocean).
class TerrainRenderer {
public:
    static constexpr u32 kMaxTiles = 128;

    Result<void> init(const vk::Device& device, VkDescriptorSetLayout frameLayout);
    void shutdown(const vk::Device& device);

    Result<void> createPipeline(const vk::Device& device, VkRenderPass renderPass);
    void destroyPipeline(const vk::Device& device);

    // Phase 18 (world): map-level data without waiting for the GPU. The map's layer array and, when `liquids` is
    // given, new liquid frames (they stay from map to map otherwise), from `src` (a staging buffer an IO thread filled:
    // vk::createTextureStaged); the copies are recorded into the next frame.
    Result<void> setTextures(const vk::Device& device, vk::Uploader& uploader, VkBuffer src, u64 terrainOffset,
                             const pack::TextureHeader& terrain, u64 liquidOffset, const pack::TextureHeader* liquids);
    // lod: first/count pairs of each LOD level inside `indices`.
    Result<void> setSharedIndices(const vk::Device& device, vk::Uploader& uploader, const u16* indices, u32 count,
                                  std::span<const u32> lod);
    // The map's layer array, material set and shared indices go to the retired list (freed by collect() once the
    // frames submitted so far are done: submittedFrames >= retireFrame). Tiles retire on their own (destroyTile).
    void retireMap(u64 retireFrame);
    // Everything at once (the GPU must be idle): shutdown.
    void clearMap(const vk::Device& device);
    // Phase 18 (world): the layer arrays' sampler with this anisotropy (1: none); the GPU must be idle. On the terrain
    // it is the costliest sampling of a frame (four layers a pixel): PERFORMANCE.md §3.10.
    void setAnisotropy(const vk::Device& device, u32 level);

    // A host-visible buffer holding the tile blob at offset 0 (see Renderer::createStagingBlock).
    struct BlobSource {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };
    // Returns 0 (not an error) when the upload ring is full this frame: retry next frame. With a source
    // buffer the GPU copies straight from it (no CPU copy); otherwise the blob goes through the ring.
    Result<u32> createTile(const vk::Device& device, vk::Uploader& uploader, const u8* blob, usize size,
                           const BlobSource* source = nullptr);
    void destroyTile(u32 id, u64 retireFrame);
    // Releases retired tiles whose frames have completed (submittedFrames >= retireFrame).
    void collect(const vk::Device& device, u64 submittedFrames, bool everything);

    bool ready() const { return m_pipeline != VK_NULL_HANDLE && m_materialSet != VK_NULL_HANDLE; }
    void draw(VkCommandBuffer cmd, VkDescriptorSet frameSet, std::span<const TerrainDraw> draws, RenderStats& stats);
    void drawLiquids(VkCommandBuffer cmd, VkDescriptorSet frameSet, std::span<const TerrainDraw> draws, bool opaque,
                     RenderStats& stats);

    u32 tileCount() const { return m_liveTiles; }
    // Phase 11: the map's terrain textures carry the specular masks of "Terrain Highlights" in their alpha.
    bool specularMasks() const { return m_specularMasks; }
    u64 gpuBytes() const { return m_gpuBytes + m_textures.gpuBytes + m_liquidTextures.gpuBytes; }
    // Set 1 of the terrain and liquid pipelines (terrain layers, liquid frames), shared with WMO liquids.
    VkDescriptorSetLayout materialLayout() const { return m_materialLayout; }
    VkDescriptorSet materialSet() const { return m_materialSet; } // null until a map is set

private:
    struct Tile {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImage alpha = VK_NULL_HANDLE;
        VmaAllocation alphaAllocation = VK_NULL_HANDLE;
        VkImageView alphaView = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        u64 indexOffset = 0;
        std::array<u32, 8> lod{}; // first/count per LOD level
        bool ownIndices = false;
        u64 liquidVertexOffset = 0;
        u64 liquidIndexOffset = 0;
        u32 liquidTranslucentCount = 0; // indices, from 0
        u32 liquidOpaqueFirst = 0;
        u32 liquidOpaqueCount = 0;
        bool used = false;
        u64 gpuBytes = 0;
    };
    struct Retired {
        u32 id;
        u64 retireFrame;
    };
    // Phase 18 (world): a map's data waiting for the frames that may still use it.
    struct RetiredMap {
        vk::Texture textures;
        vk::Texture liquids; // only when replaced
        VkDescriptorSet materialSet = VK_NULL_HANDLE;
        VkBuffer sharedIndices = VK_NULL_HANDLE;
        VmaAllocation sharedIndicesAllocation = VK_NULL_HANDLE;
        u64 retireFrame = 0;
    };

    void releaseTile(const vk::Device& device, u32 id);
    void releaseMap(const vk::Device& device, RetiredMap& map);

    VkDescriptorSetLayout m_frameLayout = VK_NULL_HANDLE; // owned by the Renderer
    VkDescriptorSetLayout m_materialLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_tileLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_liquidOpaque = VK_NULL_HANDLE;
    VkPipeline m_liquidTranslucent = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkSampler m_layerSampler = VK_NULL_HANDLE;
    VkSampler m_alphaSampler = VK_NULL_HANDLE;
    VkFormat m_alphaFormat = VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    u32 m_storageAlignment = 256;

    vk::Texture m_textures;
    vk::Texture m_liquidTextures;
    VkDescriptorSet m_materialSet = VK_NULL_HANDLE;
    VkBuffer m_sharedIndices = VK_NULL_HANDLE;
    VmaAllocation m_sharedIndicesAllocation = VK_NULL_HANDLE;
    std::array<u32, 8> m_sharedLod{};

    std::vector<Tile> m_tiles{1}; // slot 0 reserved: id 0 means "no tile"
    std::vector<u32> m_freeTiles;
    std::vector<Retired> m_retired;
    std::vector<RetiredMap> m_retiredMaps; // phase 18 (world)
    // What collect() last saw: what setTextures()/setSharedIndices() replace retires after the frames in flight.
    static constexpr u64 kRetireFrames = 3;
    u64 m_lastSubmitted = 0;
    u32 m_liveTiles = 0;
    bool m_specularMasks = false;
    u64 m_gpuBytes = 0;
};

} // namespace vd
