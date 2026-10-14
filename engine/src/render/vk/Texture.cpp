#include "render/vk/Texture.h"

#include "pack/PackFormat.h"
#include "render/vk/AstcSoftware.h"
#include "render/vk/Device.h"
#include "render/vk/Uploader.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vd::vk {
namespace {

VkFormat toVkFormat(pack::TextureFormat format) {
    switch (format) {
    case pack::TextureFormat::Astc4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case pack::TextureFormat::Astc5x5: return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    case pack::TextureFormat::Astc6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case pack::TextureFormat::Astc8x8: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    case pack::TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

struct MipUpload {
    u32 width;
    u32 height;
    usize stagingOffset;
};

} // namespace

Result<Texture> createTextureFromBlob(const Device& device, const u8* blob, usize size) {
    if (size < sizeof(pack::TextureHeader)) {
        return Error{"Texture blob too small"};
    }
    pack::TextureHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.mipCount == 0 || header.mipCount > pack::kMaxMips || header.width == 0 || header.height == 0) {
        return Error{"Invalid texture header"};
    }
    const auto packFormat = static_cast<pack::TextureFormat>(header.format);
    const u32 block = pack::astcBlockSize(packFormat);
    const bool cpuDecode = block != 0 && !device.astcSupported();

    Texture texture;
    texture.format = cpuDecode ? VK_FORMAT_R8G8B8A8_UNORM : toVkFormat(packFormat);
    texture.width = header.width;
    texture.height = header.height;
    texture.mipCount = header.mipCount;
    texture.layers = std::max<u32>(1, header.layers);
    texture.isArray = header.layers > 0;
    texture.decodedOnCpu = cpuDecode;

    // Gather the bytes to upload (straight from the blob, or decoded when the GPU lacks ASTC).
    std::vector<MipUpload> mips(header.mipCount);
    std::vector<std::vector<u8>> decoded;
    usize stagingSize = 0;
    for (u32 level = 0; level < header.mipCount; ++level) {
        const u32 w = std::max(1u, texture.width >> level);
        const u32 h = std::max(1u, texture.height >> level);
        if (header.mipOffsets[level] + usize{header.mipSizes[level]} > size) {
            return Error{"Texture mip out of bounds"};
        }
        mips[level] = {w, h, stagingSize};
        if (cpuDecode) {
            // Layers of an array mip are stored back to back; decode them one by one.
            const u32 layerBytes = header.mipSizes[level] / texture.layers;
            std::vector<u8> levelRgba;
            for (u32 layer = 0; layer < texture.layers; ++layer) {
                Result<std::vector<u8>> rgba = decodeAstcSoftware(
                    blob + header.mipOffsets[level] + usize{layer} * layerBytes, layerBytes, w, h, block);
                if (!rgba) {
                    return rgba.error();
                }
                levelRgba.insert(levelRgba.end(), rgba.value().begin(), rgba.value().end());
            }
            stagingSize += levelRgba.size();
            decoded.push_back(std::move(levelRgba));
        } else {
            stagingSize += header.mipSizes[level];
        }
        stagingSize = (stagingSize + 15) & ~usize{15}; // copy offsets: multiple of the texel block size
    }
    texture.gpuBytes = stagingSize;

    VkDevice dev = device.device();
    VmaAllocator allocator = device.allocator();

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = stagingSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMemory = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    VD_VK_TRY(vmaCreateBuffer(allocator, &bufferInfo, &stagingAlloc, &staging, &stagingMemory, &stagingInfo));
    auto* mapped = static_cast<u8*>(stagingInfo.pMappedData);
    for (u32 level = 0; level < header.mipCount; ++level) {
        if (cpuDecode) {
            std::memcpy(mapped + mips[level].stagingOffset, decoded[level].data(), decoded[level].size());
        } else {
            std::memcpy(mapped + mips[level].stagingOffset, blob + header.mipOffsets[level], header.mipSizes[level]);
        }
    }
    vmaFlushAllocation(allocator, stagingMemory, 0, VK_WHOLE_SIZE);

    auto cleanupStaging = [&] { vmaDestroyBuffer(allocator, staging, stagingMemory); };

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = texture.format;
    imageInfo.extent = {texture.width, texture.height, 1};
    imageInfo.mipLevels = texture.mipCount;
    imageInfo.arrayLayers = texture.layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VkResult r = device.createStreamedImage(imageInfo, &texture.image, &texture.allocation); r != VK_SUCCESS) {
        cleanupStaging();
        return makeError("vmaCreateImage(texture)", r);
    }

    // One-shot command buffer: barrier -> copy every mip -> barrier to shader-read.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device.queueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(dev, &poolInfo, nullptr, &pool);
    if (result == VK_SUCCESS) {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(dev, &allocInfo, &cmd);
    }
    if (result == VK_SUCCESS) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(dev, &fenceInfo, nullptr, &fence);
    }
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = texture.image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipCount, 0, texture.layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toTransfer);

        std::vector<VkBufferImageCopy> regions(texture.mipCount);
        for (u32 level = 0; level < texture.mipCount; ++level) {
            regions[level] = {};
            regions[level].bufferOffset = mips[level].stagingOffset;
            regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.layers};
            regions[level].imageExtent = {mips[level].width, mips[level].height, 1};
        }
        vkCmdCopyBufferToImage(cmd, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(regions.size()), regions.data());

        VkImageMemoryBarrier toShader = toTransfer;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toShader);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        result = vkQueueSubmit(device.queue(), 1, &submit, fence);
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        }
    }
    if (fence) vkDestroyFence(dev, fence, nullptr);
    if (pool) vkDestroyCommandPool(dev, pool, nullptr);
    cleanupStaging();
    if (result != VK_SUCCESS) {
        destroyTexture(device, texture);
        return makeError("texture upload", result);
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = texture.isArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipCount, 0, texture.layers};
    if (VkResult r = vkCreateImageView(dev, &viewInfo, nullptr, &texture.view); r != VK_SUCCESS) {
        destroyTexture(device, texture);
        return makeError("vkCreateImageView(texture)", r);
    }
    return texture;
}

Result<Texture> createTextureStreamed(const Device& device, Uploader& uploader, VkBuffer src, u64 srcOffset,
                                      const u8* blob, usize size) {
    if (size < sizeof(pack::TextureHeader)) {
        return Error{"Texture blob too small"};
    }
    pack::TextureHeader header;
    std::memcpy(&header, blob, sizeof(header));
    const auto packFormat = static_cast<pack::TextureFormat>(header.format);
    if (pack::astcBlockSize(packFormat) != 0 && !device.astcSupported()) {
        return createTextureFromBlob(device, blob, size);
    }
    for (u32 level = 0; level < std::min<u32>(header.mipCount, pack::kMaxMips); ++level) {
        if (header.mipOffsets[level] + u64{header.mipSizes[level]} > size) {
            return Error{"Texture mip out of bounds"};
        }
    }
    return createTextureStaged(device, uploader, src, srcOffset, header);
}

Result<Texture> createTextureStaged(const Device& device, Uploader& uploader, VkBuffer src, u64 srcOffset,
                                    const pack::TextureHeader& header) {
    const auto packFormat = static_cast<pack::TextureFormat>(header.format);
    if (header.mipCount == 0 || header.mipCount > pack::kMaxMips || header.width == 0 || header.height == 0) {
        return Error{"Invalid texture header"};
    }
    if (pack::astcBlockSize(packFormat) != 0 && !device.astcSupported()) {
        return Error{"ASTC texture staged for a GPU without ASTC"};
    }
    Texture texture;
    texture.format = toVkFormat(packFormat);
    texture.width = header.width;
    texture.height = header.height;
    texture.mipCount = header.mipCount;
    texture.layers = std::max<u32>(1, header.layers);
    texture.isArray = header.layers > 0;

    VkBufferImageCopy regions[pack::kMaxMips]{};
    for (u32 level = 0; level < header.mipCount; ++level) {
        regions[level].bufferOffset = srcOffset + header.mipOffsets[level];
        regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.layers};
        regions[level].imageExtent = {std::max(1u, texture.width >> level), std::max(1u, texture.height >> level), 1};
        texture.gpuBytes += header.mipSizes[level];
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = texture.format;
    imageInfo.extent = {texture.width, texture.height, 1};
    imageInfo.mipLevels = texture.mipCount;
    imageInfo.arrayLayers = texture.layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VD_VK_TRY(device.createStreamedImage(imageInfo, &texture.image, &texture.allocation));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = texture.isArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipCount, 0, texture.layers};
    if (VkResult r = vkCreateImageView(device.device(), &viewInfo, nullptr, &texture.view); r != VK_SUCCESS) {
        destroyTexture(device, texture);
        return makeError("vkCreateImageView(texture)", r);
    }
    uploader.copyImageFrom(src, texture.image, {regions, header.mipCount}, texture.mipCount, texture.layers);
    return texture;
}

void destroyTexture(const Device& device, Texture& texture) {
    if (texture.view) {
        vkDestroyImageView(device.device(), texture.view, nullptr);
    }
    if (texture.image) {
        vmaDestroyImage(device.allocator(), texture.image, texture.allocation);
    }
    texture = {};
}

} // namespace vd::vk
