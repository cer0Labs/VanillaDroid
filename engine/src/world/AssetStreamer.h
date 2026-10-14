#pragma once

#include "core/SpscQueue.h"
#include "core/Types.h"
#include "pack/PackReader.h"
#include "render/Renderer.h"

#include <array>
#include <atomic>
#include <semaphore>
#include <thread>

namespace vd {

// IO thread for variable-size assets (models, textures): reads the pack (page faults = flash reads),
// decompresses into a heap copy for the game thread, and writes the raw bytes sequentially into a
// staging ring (GPU-visible memory) so the upload is a GPU copy. The ring is a byte FIFO: the IO thread
// allocates at the head, the game thread frees at the tail once the frame that copied the data has
// completed on the GPU.
//
// Threading: request/poll/release/update are game-thread only. Results must be released in poll order.
class AssetStreamer {
public:
    struct Done {
        u64 tag = 0; // caller's id for the request
        bool ok = false;
        u64 stagingOffset = 0; // raw asset bytes in the staging block
        u64 size = 0;
        u8* data = nullptr; // heap copy of the same bytes; freed by release()
        u64 end = 0;        // ring position after this asset (internal)
    };

    ~AssetStreamer();

    // `staging` is owned by the caller and must outlive the streamer. `threadName`: a string literal.
    void start(const StagingBlock& staging, const char* threadName = "vd-assets");
    void stop(); // joins; drops queued work

    // `asset` points into a mapped pack that must stay open until the result is released. False when
    // the request queue is full (retry later). Phase 18 (world): `decodeAstc` (a GPU without ASTC: desktop
    // development) turns an ASTC texture blob into an RGBA8 one here (render/TextureStaging.h), so that the game thread
    // only records its copy (the software decode cost it up to 45 ms a frame).
    bool request(u64 tag, const pack::PackAsset& asset, bool decodeAstc = false);
    bool poll(Done& out);
    // The GPU reads `done`'s staging bytes in frame `frame` (Renderer::frameNumber() + 1 when the copy was
    // just recorded; 0 if nothing was uploaded). Frees the heap copy unless `keepData` (the caller then owns
    // done.data and frees it with delete[]).
    void release(Done& done, u64 frame, bool keepData = false);
    // Reclaims the ring space of frames the GPU has finished.
    void update(u64 completedFrames);

    u32 pending() const { return m_pending; }
    u64 capacity() const { return m_staging.size; }

private:
    struct Request {
        u64 tag;
        pack::PackAsset asset;
        bool decodeAstc = false;
    };
    struct Release {
        u64 end;
        u64 frame;
    };

    void run();
    bool reserve(u64 size, u64& start); // IO thread; waits for space
    void decodeTexture(u8*& blob, u64& size); // IO thread: ASTC -> RGBA8, header and mips (phase 18, world)

    StagingBlock m_staging;
    SpscQueue<Request, 512> m_requests; // game -> IO
    SpscQueue<Done, 512> m_done;        // IO -> game
    // Game thread only: FIFO of ring space waiting for its frame to complete.
    std::array<Release, 512> m_releases{};
    u32 m_releaseFirst = 0;
    u32 m_releaseCount = 0;
    std::atomic<u64> m_tail{0};         // written by the game thread
    u64 m_head = 0;                     // IO thread
    u32 m_pending = 0;                  // requested and not yet released (game thread)
    std::thread m_thread;
    std::counting_semaphore<1 << 16> m_wake{0};  // new requests
    std::counting_semaphore<1 << 16> m_space{0}; // ring space freed
    std::atomic<bool> m_stop{false};
    const char* m_threadName = "vd-assets";
};

} // namespace vd
