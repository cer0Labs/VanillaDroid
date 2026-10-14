#pragma once

#include "core/SpscQueue.h"
#include "core/Types.h"
#include "pack/PackReader.h"

#include <array>
#include <atomic>
#include <semaphore>
#include <thread>
#include <vector>

namespace vd {

// IO thread for terrain tiles: touching the mapped pack (page faults = flash reads), LZ4 decompression
// and the copy into GPU staging memory never happen on the game thread. Each of the few recycled slots
// has a heap buffer (LZ4 target, read by the game thread) and a staging buffer (written sequentially
// by the IO thread, copied by the GPU). The game thread hands a slot back with release() once the GPU
// copy has executed.
//
// Why two buffers: host-visible memory on Adreno reads like uncached memory. LZ4 re-reads its own
// output, and the game thread reads the heights back, so both work on the heap copy (measured on an
// Adreno 650: decompressing straight into staging took 40-130 ms instead of 2-17 ms).
//
// Threading: request/poll/release/inFlight are game-thread only; the queues are lock-free SPSC.
class TerrainStreamer {
public:
    static constexpr u32 kSlots = 4;

    struct Done {
        u16 tx = 0;
        u16 ty = 0;
        u32 slot = 0;
        bool ok = false;
        f32 ioMs = 0.0f; // read + decompress time on the IO thread
    };

    // `staging`: kSlots mapped buffers of `capacity` bytes each, owned by the caller.
    void start(const std::array<u8*, kSlots>& staging, usize capacity);
    // Waits for in-flight work, then joins the thread. Pack data referenced by requests must stay
    // mapped until then.
    void stop();

    bool running() const { return m_thread.joinable(); } // phase 18 (world)
    bool canRequest() const { return !m_freeSlots.empty(); }
    // `asset` points into a mapped pack that must outlive the request.
    bool request(u16 tx, u16 ty, const pack::PackAsset& asset);
    bool poll(Done& out);
    // The decompressed blob in normal memory (its twin in the slot's staging buffer feeds the GPU).
    const u8* data(u32 slot) const { return m_heap[slot].data(); }
    usize size(u32 slot) const { return m_sizes[slot]; }
    void release(u32 slot);
    u32 inFlight() const { return kSlots - static_cast<u32>(m_freeSlots.size()); }

private:
    struct Request {
        u16 tx;
        u16 ty;
        u32 slot;
        pack::PackAsset asset;
    };

    void run();

    SpscQueue<Request, 8> m_requests; // game -> IO
    SpscQueue<Done, 8> m_done;        // IO -> game
    std::array<std::vector<u8>, kSlots> m_heap;
    std::array<u8*, kSlots> m_staging{};
    usize m_capacity = 0;
    std::array<usize, kSlots> m_sizes{};
    std::vector<u32> m_freeSlots; // game thread only
    std::thread m_thread;
    std::counting_semaphore<64> m_wake{0};
    std::atomic<bool> m_stop{false};
};

} // namespace vd
