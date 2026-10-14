#include "render/vk/Uploader.h"

#include "render/vk/Device.h"

#include <algorithm>

namespace vd::vk {
namespace {

constexpr u64 kAlignment = 16; // covers texel blocks (ASTC 16 B) and bufferOffset rules

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

Result<void> Uploader::create(const Device& device, u64 capacity, u32 framesInFlight) {
    if (framesInFlight > kMaxFrames) {
        return Error{"Uploader: too many frames in flight"};
    }
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = capacity;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VD_VK_TRY(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &m_buffer, &m_allocation, &info));
    m_mapped = static_cast<u8*>(info.pMappedData);
    m_capacity = capacity;
    m_bufferCopies.reserve(256);
    m_imageCopies.reserve(64);
    m_barriers.reserve(64);
    return {};
}

void Uploader::destroy(const Device& device) {
    if (m_buffer) {
        vmaDestroyBuffer(device.allocator(), m_buffer, m_allocation);
    }
    *this = Uploader{};
}

bool Uploader::reserve(u64 size, Staging& out) {
    if (size == 0 || size > m_capacity) {
        return false;
    }
    u64 start = alignUp(m_head, kAlignment);
    if (start % m_capacity + size > m_capacity) {
        start = alignUp(start, m_capacity); // does not fit before the end: wrap to the ring start
    }
    // Phase 18 (world): an empty ring takes anything that fits it (the counters would count the gap the wrap skips).
    if (start + size - m_tail > m_capacity && !(m_tail == m_head && size <= m_capacity)) {
        return false; // still in use by frames in flight
    }
    m_head = start + size;
    out.offset = start % m_capacity;
    out.data = m_mapped + out.offset;
    out.size = size;
    return true;
}

void Uploader::commit(const Device& device, const Staging& staging) {
    vmaFlushAllocation(device.allocator(), m_allocation, staging.offset, staging.size);
}

void Uploader::copyBuffer(const Staging& staging, u64 srcOffset, VkBuffer dst, u64 dstOffset, u64 size) {
    copyBufferFrom(m_buffer, staging.offset + srcOffset, dst, dstOffset, size);
}

void Uploader::copyImage(const Staging& staging, u64 srcOffset, VkImage dst, u32 width, u32 height) {
    copyImageFrom(m_buffer, staging.offset + srcOffset, dst, width, height);
}

void Uploader::copyBufferFrom(VkBuffer src, u64 srcOffset, VkBuffer dst, u64 dstOffset, u64 size) {
    m_bufferCopies.push_back({src, dst, VkBufferCopy{srcOffset, dstOffset, size}});
}

void Uploader::copyImageFrom(VkBuffer src, u64 srcOffset, VkImage dst, u32 width, u32 height) {
    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    copyImageFrom(src, dst, {&region, 1}, 1, 1);
}

void Uploader::copyImageFrom(VkBuffer src, VkImage dst, std::span<const VkBufferImageCopy> regions, u32 mipLevels,
                             u32 layers) {
    m_imageCopies.push_back({src, dst, static_cast<u32>(m_imageRegions.size()), static_cast<u32>(regions.size()),
                             mipLevels, layers});
    m_imageRegions.insert(m_imageRegions.end(), regions.begin(), regions.end());
}

void Uploader::updateImageLayer(VkBuffer src, VkImage dst, std::span<const VkBufferImageCopy> regions, u32 mipLevels,
                                u32 layer) {
    m_imageCopies.push_back({src, dst, static_cast<u32>(m_imageRegions.size()), static_cast<u32>(regions.size()),
                             mipLevels, 1, layer, true});
    m_imageRegions.insert(m_imageRegions.end(), regions.begin(), regions.end());
}

void Uploader::record(VkCommandBuffer cmd) {
    m_recordedHead = m_head;
    if (!hasPendingWork()) {
        return;
    }
    auto imageBarrier = [](const ImageCopy& copy, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                           VkAccessFlags dst) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = copy.dst;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, copy.mipLevels, copy.firstLayer, copy.layers};
        return barrier;
    };

    m_barriers.clear();
    bool preserving = false; // layers of images earlier frames sampled: wait for those reads first
    for (const ImageCopy& copy : m_imageCopies) {
        m_barriers.push_back(imageBarrier(
            copy, copy.preserve ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy.preserve ? VK_ACCESS_SHADER_READ_BIT : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT));
        preserving |= copy.preserve;
    }
    if (!m_barriers.empty()) {
        vkCmdPipelineBarrier(cmd,
                             preserving ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<u32>(m_barriers.size()), m_barriers.data());
    }

    // Consecutive copies between the same pair of buffers become one vkCmdCopyBuffer.
    for (usize i = 0; i < m_bufferCopies.size();) {
        usize end = i + 1;
        while (end < m_bufferCopies.size() && m_bufferCopies[end].dst == m_bufferCopies[i].dst &&
               m_bufferCopies[end].src == m_bufferCopies[i].src) {
            ++end;
        }
        VkBufferCopy regions[16];
        const usize count = std::min<usize>(end - i, 16);
        for (usize k = 0; k < count; ++k) {
            regions[k] = m_bufferCopies[i + k].region;
        }
        vkCmdCopyBuffer(cmd, m_bufferCopies[i].src, m_bufferCopies[i].dst, static_cast<u32>(count), regions);
        i += count;
    }
    for (const ImageCopy& copy : m_imageCopies) {
        vkCmdCopyBufferToImage(cmd, copy.src, copy.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy.regionCount,
                               m_imageRegions.data() + copy.firstRegion);
    }

    m_barriers.clear();
    for (const ImageCopy& copy : m_imageCopies) {
        m_barriers.push_back(imageBarrier(copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_SHADER_READ_BIT));
    }
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &memory, 0, nullptr, static_cast<u32>(m_barriers.size()), m_barriers.data());

    m_bufferCopies.clear();
    m_imageCopies.clear();
    m_imageRegions.clear();
}

void Uploader::frameSubmitted(u32 slot) {
    m_frameEnd[slot] = m_recordedHead;
}

void Uploader::frameCompleted(u32 slot) {
    // Everything reserved before that frame was recorded has been consumed by it (or earlier frames).
    m_tail = std::max(m_tail, m_frameEnd[slot]);
}

} // namespace vd::vk
