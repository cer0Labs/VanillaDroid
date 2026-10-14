#pragma once

#include "core/Types.h"
#include "render/vk/VkCommon.h"

#include <array>
#include <span>
#include <vector>

namespace vd::vk {

class Device;

// In-game GPU uploads without stalls. Data is copied into a persistently mapped staging ring and the
// transfers are recorded at the start of the next frame's command buffer (before the render pass), so
// whatever is uploaded before renderFrame() can be drawn in that same frame. Ring space used by a
// frame is reclaimed once that frame's fence has signalled.
class Uploader {
public:
    static constexpr u32 kMaxFrames = 4;

    Result<void> create(const Device& device, u64 capacity, u32 framesInFlight);
    void destroy(const Device& device);

    struct Staging {
        u8* data = nullptr; // write here, then commit
        u64 offset = 0;     // in the ring buffer
        u64 size = 0;
    };
    // Reserves `size` bytes (aligned to 16). False when the ring is full this frame: retry next frame.
    bool reserve(u64 size, Staging& out);
    // Flushes the written range (non-coherent memory) after the caller filled it.
    void commit(const Device& device, const Staging& staging);

    // Queued transfers from a committed staging range.
    void copyBuffer(const Staging& staging, u64 srcOffset, VkBuffer dst, u64 dstOffset, u64 size);
    // Whole-image copy of mip 0 / layer 0; the image goes UNDEFINED -> SHADER_READ_ONLY_OPTIMAL.
    void copyImage(const Staging& staging, u64 srcOffset, VkImage dst, u32 width, u32 height);
    // Same, from a caller-owned source buffer (e.g. a staging block filled by the IO thread). The caller
    // keeps `src` alive and unmodified until the frame that records the copy has completed.
    void copyBufferFrom(VkBuffer src, u64 srcOffset, VkBuffer dst, u64 dstOffset, u64 size);
    void copyImageFrom(VkBuffer src, u64 srcOffset, VkImage dst, u32 width, u32 height);
    // Several regions (e.g. every mip of a texture); the whole image (mipLevels x layers) goes
    // UNDEFINED -> SHADER_READ_ONLY_OPTIMAL.
    void copyImageFrom(VkBuffer src, VkImage dst, std::span<const VkBufferImageCopy> regions, u32 mipLevels,
                       u32 layers);
    // Rewrites one layer of an array image already in SHADER_READ_ONLY_OPTIMAL (the other layers keep their
    // contents): the regions must all target `layer`. From the ring (`staging`) or a caller-owned buffer.
    void updateImageLayer(VkBuffer src, VkImage dst, std::span<const VkBufferImageCopy> regions, u32 mipLevels,
                          u32 layer);
    VkBuffer buffer() const { return m_buffer; }

    bool hasPendingWork() const { return !m_bufferCopies.empty() || !m_imageCopies.empty(); }
    // Drops queued transfers that were never recorded (their destinations are about to be destroyed,
    // e.g. a map change before any frame was drawn). Their ring space is reclaimed as usual.
    void discardPending() {
        m_bufferCopies.clear();
        m_imageCopies.clear();
        m_imageRegions.clear();
    }
    // Records every queued transfer plus the barriers that make them visible to vertex input, index
    // fetch and shaders. Must be called outside a render pass.
    void record(VkCommandBuffer cmd);
    // Ring bookkeeping: the frame recorded into `slot` was submitted / its fence has signalled.
    void frameSubmitted(u32 slot);
    void frameCompleted(u32 slot);

    u64 capacity() const { return m_capacity; }
    u64 used() const { return m_head - m_tail; }

private:
    struct BufferCopy {
        VkBuffer src;
        VkBuffer dst;
        VkBufferCopy region;
    };
    struct ImageCopy {
        VkBuffer src;
        VkImage dst;
        u32 firstRegion; // into m_imageRegions
        u32 regionCount;
        u32 mipLevels;
        u32 layers;
        u32 firstLayer = 0;
        bool preserve = false; // the image is in use (SHADER_READ_ONLY_OPTIMAL): only these layers change
    };

    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    u8* m_mapped = nullptr;
    u64 m_capacity = 0;
    u64 m_head = 0; // monotonic byte counters: [m_tail, m_head) is in use
    u64 m_tail = 0;
    u64 m_recordedHead = 0; // m_head when record() last ran
    std::array<u64, kMaxFrames> m_frameEnd{};
    std::vector<BufferCopy> m_bufferCopies;
    std::vector<ImageCopy> m_imageCopies;
    std::vector<VkBufferImageCopy> m_imageRegions;
    std::vector<VkImageMemoryBarrier> m_barriers; // scratch, kept to avoid per-frame allocations
};

} // namespace vd::vk
