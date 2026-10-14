#pragma once

#include "core/Types.h"

#include <array>
#include <atomic>
#include <type_traits>

namespace vd {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding from alignas: intended, keeps the two indices on separate cache lines
#endif

// Bounded lock-free queue for exactly one producer thread and one consumer thread. No allocation:
// storage is inline. Capacity must be a power of two; one slot stays empty.
template <typename T, u32 Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue holds plain data");

public:
    // Producer side. False when full.
    bool push(const T& item) {
        const u32 head = m_head.load(std::memory_order_relaxed);
        const u32 next = (head + 1) & (Capacity - 1);
        if (next == m_tail.load(std::memory_order_acquire)) {
            return false;
        }
        m_items[head] = item;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. False when empty.
    bool pop(T& out) {
        const u32 tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        out = m_items[tail];
        m_tail.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> m_items{};
    alignas(64) std::atomic<u32> m_head{0}; // written by the producer
    alignas(64) std::atomic<u32> m_tail{0}; // written by the consumer
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace vd
