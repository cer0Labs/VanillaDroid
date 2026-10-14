#include "jobs/JobSystem.h"

#include <doctest/doctest.h>

#include <atomic>
#include <vector>

TEST_CASE("parallelFor visits every index exactly once") {
    vd::JobSystem jobs;
    jobs.init(4);

    constexpr vd::u32 kCount = 100'000;
    std::vector<std::atomic<vd::u32>> visits(kCount);
    jobs.parallelFor(kCount, 64, [&](vd::u32 begin, vd::u32 end) {
        for (vd::u32 i = begin; i < end; ++i) {
            visits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });

    bool allOnce = true;
    for (const std::atomic<vd::u32>& v : visits) {
        allOnce = allOnce && v.load() == 1;
    }
    CHECK(allOnce);
}

TEST_CASE("parallelFor survives many back-to-back batches") {
    vd::JobSystem jobs;
    jobs.init(); // default worker count

    // Small batches in a tight loop stress the batch hand-over between generations.
    std::atomic<vd::u64> total{0};
    for (int batch = 0; batch < 2000; ++batch) {
        jobs.parallelFor(257, 7, [&](vd::u32 begin, vd::u32 end) {
            vd::u64 local = 0;
            for (vd::u32 i = begin; i < end; ++i) {
                local += i;
            }
            total.fetch_add(local, std::memory_order_relaxed);
        });
    }
    CHECK(total.load() == 2000ull * (256ull * 257ull / 2ull));
}

TEST_CASE("parallelFor runs inline when there is nothing to split") {
    vd::JobSystem jobs;
    jobs.init(2);
    int calls = 0;
    jobs.parallelFor(10, 64, [&](vd::u32 begin, vd::u32 end) {
        ++calls;
        CHECK(begin == 0);
        CHECK(end == 10);
    });
    CHECK(calls == 1);
    jobs.parallelFor(0, 1, [&](vd::u32, vd::u32) { ++calls; });
    CHECK(calls == 1);
}

TEST_CASE("begin/wait: the caller works meanwhile, few chunks wake few workers") {
    vd::JobSystem jobs;
    jobs.init(6);
    for (int round = 0; round < 500; ++round) {
        std::atomic<vd::u32> sum{0};
        auto work = [&](vd::u32 begin, vd::u32 end) {
            for (vd::u32 i = begin; i < end; ++i) {
                sum.fetch_add(i + 1, std::memory_order_relaxed);
            }
        };
        const vd::u32 count = 2 + static_cast<vd::u32>(round % 5); // fewer chunks than workers
        jobs.begin(count, 1, work);
        vd::u32 mine = 0;
        for (vd::u32 i = 0; i < 1000; ++i) {
            mine += i; // the caller's own work while the batch runs
        }
        jobs.wait();
        CHECK(sum.load() == count * (count + 1) / 2);
        CHECK(mine == 499500u);
    }
    jobs.wait(); // nothing pending: returns at once
}
