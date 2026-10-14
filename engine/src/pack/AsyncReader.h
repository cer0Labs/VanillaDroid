#pragma once

#include "core/SpscQueue.h"
#include "core/Types.h"
#include "pack/PackReader.h"

#include <atomic>
#include <semaphore>
#include <thread>

namespace vd::pack {

// IO thread that reads pack assets (page faults = flash reads) and decompresses them into heap buffers, for
// data the CPU uses in place (collision tiles). Nothing of it happens on the game thread, which only hands out
// requests and takes the finished buffers.
//
// Threading: request/poll/pending are for one thread (the game thread); the queues are lock-free SPSC.
class AsyncReader {
public:
    struct Done {
        u64 tag = 0;
        u8* data = nullptr; // rawSize bytes; the receiver owns it (delete[]); null when the read failed
        u32 size = 0;
    };

    ~AsyncReader();

    // `threadName`: a string literal.
    void start(const char* threadName);
    // Joins the thread and frees whatever was queued or finished but not taken.
    void stop();

    // Checks a loaded asset on the IO thread (bounds of every offset and index): a false return drops it.
    using Validate = bool (*)(const u8* data, u32 size);

    // `asset` points into a mapped pack that must stay open until its result is polled (or stop()). False
    // when the queue is full: try again later.
    bool request(u64 tag, const PackAsset& asset, Validate validate = nullptr);
    bool poll(Done& out);
    u32 pending() const { return m_pending; }

private:
    struct Request {
        u64 tag;
        PackAsset asset;
        Validate validate;
    };

    void run();

    SpscQueue<Request, 64> m_requests; // game -> IO
    SpscQueue<Done, 64> m_done;        // IO -> game (never more than 63 requests in flight)
    std::thread m_thread;
    std::counting_semaphore<1 << 16> m_wake{0};
    std::atomic<bool> m_stop{false};
    u32 m_pending = 0;
    const char* m_threadName = "vd-read";
};

} // namespace vd::pack
