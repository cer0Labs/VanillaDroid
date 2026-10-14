#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"
#include "render/vk/VkCommon.h"

namespace vd::vk {

class Device;

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mipCount = 0;
    u32 layers = 1;
    bool isArray = false; // viewed as VK_IMAGE_VIEW_TYPE_2D_ARRAY (TextureHeader::layers > 0)
    u64 gpuBytes = 0;
    bool decodedOnCpu = false; // GPU lacks ASTC: mips were decoded to RGBA8 before upload
};

// Creates a sampled image (2D or 2D array) from a pack texture blob (pack::TextureHeader + mips).
// Synchronous upload (staging buffer, one submit, fence wait): fine for tools and loading screens;
// in-game streaming (phase 2) goes through a staging ring instead.
Result<Texture> createTextureFromBlob(const Device& device, const u8* blob, usize size);

class Uploader;
// Same, without stalling: `src` (a staging buffer) holds the blob at `srcOffset` and the copies are recorded
// by the Uploader into the next frame; `blob` is a CPU copy for the header. GPUs without ASTC fall back to
// the synchronous path (desktop development only).
Result<Texture> createTextureStreamed(const Device& device, Uploader& uploader, VkBuffer src, u64 srcOffset,
                                      const u8* blob, usize size);
// Phase 18 (world): a texture whose texels an IO thread already wrote into `src` at `srcOffset`, laid out as `header`
// says (mip offsets from srcOffset) in a format the GPU samples (ASTC only where the device has it: the writer decoded
// it to RGBA8 otherwise). Nothing is decoded or waited for here: the copies are recorded into the next frame.
Result<Texture> createTextureStaged(const Device& device, Uploader& uploader, VkBuffer src, u64 srcOffset,
                                    const pack::TextureHeader& header);
void destroyTexture(const Device& device, Texture& texture);

} // namespace vd::vk
