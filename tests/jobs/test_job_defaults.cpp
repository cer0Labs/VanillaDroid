// Phase 22 (phonebaker): the worker count a program sets for every JobSystem it starts (the baker's --jobs).
#include "jobs/JobSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace {

// Restores the hardware default when a test ends (the setting is process-wide).
struct DefaultWorkers {
    explicit DefaultWorkers(vd::i32 count) { vd::JobSystem::setDefaultWorkerCount(count); }
    ~DefaultWorkers() { vd::JobSystem::setDefaultWorkerCount(-1); }
};

} // namespace

TEST_CASE("JobSystem: init() takes the default worker count") {
    {
        const DefaultWorkers workers(3);
        vd::JobSystem jobs;
        jobs.init();
        CHECK(jobs.workerCount() == 3);
    }
    {
        const DefaultWorkers workers(5);
        vd::JobSystem jobs;
        jobs.init(2); // an explicit count still wins
        CHECK(jobs.workerCount() == 2);
    }
    vd::JobSystem jobs;
    jobs.init(); // back to hardware threads - 2
    const vd::u32 hardware = std::max(1u, std::thread::hardware_concurrency());
    CHECK(jobs.workerCount() == (hardware > 2 ? hardware - 2 : 1));
}

TEST_CASE("JobSystem: no workers runs every batch on the calling thread") {
    const DefaultWorkers workers(0); // --jobs 1
    vd::JobSystem jobs;
    jobs.init();
    CHECK(jobs.workerCount() == 0);
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<vd::u32> visited{0};
    std::atomic<bool> elsewhere{false};
    jobs.parallelFor(1000, 7, [&](vd::u32 begin, vd::u32 end) {
        visited += end - begin;
        if (std::this_thread::get_id() != caller) {
            elsewhere = true;
        }
    });
    CHECK(visited == 1000);
    CHECK_FALSE(elsewhere);
}
