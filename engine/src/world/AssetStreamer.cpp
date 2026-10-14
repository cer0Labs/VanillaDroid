#include "world/AssetStreamer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"
#include "render/TextureStaging.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vd {
namespace {

constexpr u64 kAlignment = 256; // covers every copy offset rule (texel blocks, buffer offsets)
constexpr u32 kMaxPending = 480;

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

AssetStreamer::~AssetStreamer() {
    stop();
}

void AssetStreamer::start(const StagingBlock& staging, const char* threadName) {
    if (m_thread.joinable()) {
        return;
    }
    m_threadName = threadName;
    m_staging = staging;
    m_head = 0;
    m_tail = 0;
    m_pending = 0;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void AssetStreamer::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    m_wake.release();
    m_space.release();
    m_thread.join();
    Request request;
    while (m_requests.pop(request)) {
    }
    Done done;
    while (m_done.pop(done)) {
        delete[] done.data;
    }
    m_releaseFirst = 0;
    m_releaseCount = 0;
    m_pending = 0;
}

bool AssetStreamer::request(u64 tag, const pack::PackAsset& asset, bool decodeAstc) {
    if (m_pending >= kMaxPending || !m_requests.push({tag, asset, decodeAstc})) {
        return false;
    }
    ++m_pending;
    m_wake.release();
    return true;
}

bool AssetStreamer::poll(Done& out) {
    return m_done.pop(out);
}

void AssetStreamer::release(Done& done, u64 frame, bool keepData) {
    if (!keepData) {
        delete[] done.data;
    }
    done.data = nullptr;
    // At most kMaxPending results are alive, so the FIFO never overflows.
    m_releases[(m_releaseFirst + m_releaseCount) % m_releases.size()] = {done.end, frame};
    ++m_releaseCount;
    --m_pending;
}

void AssetStreamer::update(u64 completedFrames) {
    // Space is freed in allocation order (results are released in poll order).
    bool advanced = false;
    while (m_releaseCount > 0 && m_releases[m_releaseFirst].frame <= completedFrames) {
        m_tail.store(m_releases[m_releaseFirst].end, std::memory_order_release);
        m_releaseFirst = (m_releaseFirst + 1) % m_releases.size();
        --m_releaseCount;
        advanced = true;
    }
    if (advanced) {
        m_space.release();
    }
}

void AssetStreamer::decodeTexture(u8*& blob, u64& size) {
    if (size < sizeof(pack::TextureHeader)) {
        return;
    }
    pack::TextureHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (pack::astcBlockSize(static_cast<pack::TextureFormat>(header.format)) == 0) {
        return;
    }
    u64 bytes = 0;
    const pack::TextureHeader layout = stagedTextureLayout(header, false, bytes); // RGBA8 mips from offset 0
    // A blob like the pack's: its header first, then the mips (16-byte aligned for the copies). One that would take
    // more than half the ring stays as it is (the game thread decodes it, as before).
    const u64 base = (sizeof(pack::TextureHeader) + 15) & ~u64{15};
    if (base + bytes > m_staging.size / 2) {
        return;
    }
    pack::TextureHeader decodedHeader = layout;
    for (u32 level = 0; level < std::min<u32>(layout.mipCount, pack::kMaxMips); ++level) {
        decodedHeader.mipOffsets[level] += static_cast<u32>(base);
    }
    u8* decoded = new u8[base + bytes];
    std::memcpy(decoded, &decodedHeader, sizeof(decodedHeader));
    if (!stageTexture(blob, size, layout, decoded + base, &m_stop)) {
        delete[] decoded;
        return; // left as it was: the game thread decodes it (the old path)
    }
    delete[] blob;
    blob = decoded;
    size = base + bytes;
}

bool AssetStreamer::reserve(u64 size, u64& start) {
    const u64 capacity = m_staging.size;
    for (;;) {
        start = alignUp(m_head, kAlignment);
        if (start % capacity + size > capacity) {
            start = alignUp(start, capacity); // does not fit before the end: wrap
        }
        // Phase 18 (world): an empty ring (everything released: the tail caught up with the head) takes any asset that
        // fits it. The counters alone would also count the gap skipped by the wrap as in use, and an asset larger than
        // the part of the ring before the head (Stormwind's 29 MB) waited forever for a tail that no release moves.
        const u64 tail = m_tail.load(std::memory_order_acquire);
        if (start + size - tail <= capacity || (tail == m_head && size <= capacity)) {
            m_head = start + size;
            return true;
        }
        if (m_stop.load()) {
            return false;
        }
        static_cast<void>(m_space.try_acquire_for(std::chrono::milliseconds(5))); // or time out and re-check
    }
}

void AssetStreamer::run() {
    setCurrentThreadName(m_threadName);
    for (;;) {
        m_wake.acquire();
        if (m_stop.load()) {
            return;
        }
        Request request;
        while (!m_stop.load() && m_requests.pop(request)) {
            VD_PROFILE_ZONE("AssetStreamer::load");
            Done done;
            done.tag = request.tag;
            u64 size = request.asset.rawSize;
            u8* heap = size ? new u8[size] : nullptr;
            const Result<void> read = size ? pack::PackReader::read(request.asset, heap, size) : Result<void>();
            if (read && request.decodeAstc && request.asset.type == pack::AssetType::Texture) {
                decodeTexture(heap, size); // phase 18 (world): desktop without ASTC
            }
            u64 start = 0;
            if (read && size <= m_staging.size && reserve(size, start)) {
                std::memcpy(m_staging.data + start % m_staging.size, heap, size); // sequential writes only
                done.ok = true;
                done.stagingOffset = start % m_staging.size;
                done.size = size;
                done.data = heap;
                done.end = start + size;
            } else {
                if (!read) {
                    VD_LOG_ERROR("Asset %.*s: %s", static_cast<int>(request.asset.name.size()), request.asset.name.data(),
                                 read.error().message.c_str());
                } else if (size > m_staging.size) {
                    VD_LOG_ERROR("Asset %.*s (%llu bytes) does not fit the staging ring",
                                 static_cast<int>(request.asset.name.size()), request.asset.name.data(),
                                 static_cast<unsigned long long>(size));
                }
                delete[] heap;
                done.end = m_head; // nothing allocated
            }
            m_done.push(done); // capacity 512 > kMaxPending: never full
        }
    }
}

} // namespace vd
