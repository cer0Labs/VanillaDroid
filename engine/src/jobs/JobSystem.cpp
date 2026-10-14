#include "jobs/JobSystem.h"

#include "core/Assert.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

std::atomic<i32> g_defaultWorkerCount{-1}; // phase 22: setDefaultWorkerCount

} // namespace

JobSystem::~JobSystem() {
    shutdown();
}

void JobSystem::setDefaultWorkerCount(i32 count) {
    g_defaultWorkerCount.store(count, std::memory_order_relaxed);
}

void JobSystem::init(u32 workerCount) {
    const i32 configured = g_defaultWorkerCount.load(std::memory_order_relaxed);
    if (workerCount == 0 && configured >= 0) {
        workerCount = static_cast<u32>(configured);
    } else if (workerCount == 0) {
        const u32 hardware = std::max(1u, std::thread::hardware_concurrency());
        workerCount = hardware > 2 ? hardware - 2 : 1;
    }
    workerCount = std::min<u32>(workerCount, static_cast<u32>(m_threadIds.size()));
    m_workers.reserve(workerCount);
    std::atomic<u32> reported{0};
    for (u32 i = 0; i < workerCount; ++i) {
        m_workers.emplace_back([this, i, &reported] {
            m_threadIds[i] = currentThreadId();
            reported.fetch_add(1, std::memory_order_release);
            workerLoop(i);
        });
    }
    while (reported.load(std::memory_order_acquire) < workerCount) {
        std::this_thread::yield(); // ids are needed right after init (performance hint session)
    }
    VD_LOG_INFO("JobSystem: %u workers", workerCount);
}

void JobSystem::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& worker : m_workers) {
        worker.join();
    }
    m_workers.clear();
    m_stop = false;
}

void JobSystem::start(u32 count, u32 grain, RangeFn fn, void* context) {
    VD_ASSERT(!m_pending);
    if (count == 0) {
        return;
    }
    grain = std::max(grain, 1u);
    if (m_workers.empty() || count <= grain) {
        fn(context, 0, count); // too small to be worth waking anyone
        return;
    }
    VD_PROFILE_ZONE("JobSystem::start");
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // A late worker may still be leaving the previous batch; never change the batch under it.
        m_done.wait(lock, [this] { return m_activeWorkers == 0; });
        m_fn = fn;
        m_context = context;
        m_count = count;
        m_grain = grain;
        m_next.store(0, std::memory_order_relaxed);
        m_remaining.store(count, std::memory_order_relaxed);
        ++m_generation;
    }
    m_pending = true;
    // Only as many workers as there are chunks: waking all of them for a few chunks costs more than the work on
    // phones (each wake-up is a scheduler round trip, and the late ones still have to check in).
    const u32 chunks = (count + grain - 1) / grain;
    const u32 wake = std::min<u32>(static_cast<u32>(m_workers.size()), chunks);
    if (wake == m_workers.size()) {
        m_wake.notify_all();
    } else {
        for (u32 i = 0; i < wake; ++i) {
            m_wake.notify_one();
        }
    }
}

void JobSystem::wait() {
    if (!m_pending) {
        return;
    }
    m_pending = false;
    VD_PROFILE_ZONE("JobSystem::wait");
    runChunks();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [this] {
        return m_remaining.load(std::memory_order_acquire) == 0 && m_activeWorkers == 0;
    });
}

void JobSystem::runChunks() {
    for (;;) {
        const u32 begin = m_next.fetch_add(m_grain, std::memory_order_relaxed);
        if (begin >= m_count) {
            return;
        }
        const u32 end = std::min(begin + m_grain, m_count);
        m_fn(m_context, begin, end);
        const u32 finished = end - begin;
        if (m_remaining.fetch_sub(finished, std::memory_order_acq_rel) == finished) {
            // Last chunk: notify under the lock so the dispatcher cannot miss the wake-up.
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.notify_all();
        }
    }
}

void JobSystem::workerLoop(u32 index) {
    char name[16];
    std::snprintf(name, sizeof(name), "vd-worker-%u", index);
    setCurrentThreadName(name);

    u64 seenGeneration = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || m_generation != seenGeneration; });
            if (m_stop) {
                return;
            }
            seenGeneration = m_generation;
            ++m_activeWorkers;
        }
        {
            VD_PROFILE_ZONE("JobSystem::worker");
            runChunks();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_activeWorkers;
        }
        m_done.notify_all();
    }
}

} // namespace vd
