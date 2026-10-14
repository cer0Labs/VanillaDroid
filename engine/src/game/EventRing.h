#pragma once

#include "core/Types.h"

#include <array>

namespace vd::game {

// Fixed ring of the last N events with an ever-growing sequence number. Readers keep their own cursor, so
// several can follow the same ring (floating combat text and the chat frame, say) and none of them consumes
// it:
//
//     u32 cursor = ring.end();                  // from now on
//     while (const T* e = ring.next(cursor)) { ... }
//
// A reader that falls more than N behind skips what was overwritten. Slots are reused: push() hands back the
// oldest one (its strings and vectors keep their capacity), so a warmed-up ring never allocates.
template <typename T, u32 N> class EventRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    static constexpr u32 kCapacity = N;

    // The slot for a new event, to be filled by the caller (it still holds an old event's values).
    T& push() {
        T& slot = m_items[m_end & (N - 1)];
        ++m_end;
        return slot;
    }
    // Sequence number of the next event to arrive.
    u32 end() const { return m_end; }
    // The event at `cursor` and advances it; nullptr once caught up.
    const T* next(u32& cursor) const {
        if (m_end - cursor > N) {
            cursor = m_end - N; // fell behind: resume at the oldest event still here
        }
        if (cursor == m_end) {
            return nullptr;
        }
        return &m_items[cursor++ & (N - 1)];
    }
    // The newest event, nullptr when empty.
    const T* last() const { return m_end == 0 ? nullptr : &m_items[(m_end - 1) & (N - 1)]; }
    // Events still held (at most N).
    u32 size() const { return m_end < N ? m_end : N; }

private:
    std::array<T, N> m_items{};
    u32 m_end = 0;
};

} // namespace vd::game
