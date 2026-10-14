#pragma once

#include "core/Types.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace vd {

// Fixed pool of worker threads for data-parallel frame work (culling, animation, particles...).
// The calling thread participates, so a parallelFor never idles the main thread.
// Not reentrant: parallelFor is meant to be called from the game thread only.
class JobSystem {
public:
    JobSystem() = default;
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // workerCount = 0: the default count (setDefaultWorkerCount), else hardware threads - 2 (leaves room for the
    // game thread and the OS), at least 1.
    void init(u32 workerCount = 0);
    // Phase 22: the workers of every init() without a count from now on, for a program that starts many job systems
    // (the baker's --jobs; 0: none, the calling thread does all the work); negative: hardware threads - 2 again.
    static void setDefaultWorkerCount(i32 count);
    void shutdown();
    u32 workerCount() const { return static_cast<u32>(m_workers.size()); }
    // Kernel thread ids of the workers (for ADPF / affinity; zeros where the platform has none).
    std::span<const i32> threadIds() const { return {m_threadIds.data(), m_workers.size()}; }

    // Runs fn(begin, end) over [0, count) in chunks of `grain` items and blocks until all finish.
    // No allocation: fn is passed by reference, never copied into a std::function.
    template <typename Fn>
    void parallelFor(u32 count, u32 grain, Fn&& fn) {
        begin(count, grain, fn);
        wait();
    }

    // The same, split: begin() hands the chunks to the workers and returns at once, so the calling thread can do
    // other work; wait() then helps with what is left and blocks until everything has finished. `fn` must stay alive
    // until wait() returns, and one batch runs at a time.
    template <typename Fn>
    void begin(u32 count, u32 grain, Fn& fn) {
        start(count, grain, [](void* context, u32 first, u32 end) { (*static_cast<Fn*>(context))(first, end); },
              const_cast<void*>(static_cast<const void*>(&fn)));
    }
    void wait();

private:
    using RangeFn = void (*)(void* context, u32 begin, u32 end);

    void start(u32 count, u32 grain, RangeFn fn, void* context);
    void workerLoop(u32 index);
    void runChunks();

    std::vector<std::thread> m_workers;
    std::array<i32, 64> m_threadIds{}; // filled by init() once every worker has reported
    std::mutex m_mutex;
    std::condition_variable m_wake; // workers: new batch or stop
    std::condition_variable m_done; // dispatcher: batch finished / workers left

    // Current batch. Written under m_mutex only while no worker is inside runChunks().
    RangeFn m_fn = nullptr;
    void* m_context = nullptr;
    u32 m_count = 0;
    u32 m_grain = 1;
    u64 m_generation = 0; // guarded by m_mutex
    u32 m_activeWorkers = 0; // workers inside runChunks(), guarded by m_mutex
    bool m_stop = false;     // guarded by m_mutex

    std::atomic<u32> m_next{0};      // next item index to claim
    std::atomic<u32> m_remaining{0}; // items not yet finished
    bool m_pending = false;          // a batch was started and not waited for (dispatcher thread only)
};

} // namespace vd
