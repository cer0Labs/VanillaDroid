#pragma once

#include "core/Types.h"

#include <cstddef>
#include <memory>
#include <type_traits>

namespace vd {

// Linear allocator reset once per frame: per-frame scratch memory (draw lists, culling results...)
// without heap traffic. Only trivially destructible types: reset() never runs destructors.
class FrameArena {
public:
    explicit FrameArena(usize capacityBytes);
    ~FrameArena();
    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    // Returns nullptr when the arena is exhausted (asserts in debug builds).
    void* allocate(usize size, usize alignment = alignof(std::max_align_t));

    template <typename T>
    T* allocateArray(usize count) {
        static_assert(std::is_trivially_destructible_v<T>, "FrameArena never runs destructors");
        void* memory = allocate(sizeof(T) * count, alignof(T));
        if (!memory) {
            return nullptr;
        }
        T* items = static_cast<T*>(memory);
        std::uninitialized_value_construct_n(items, count);
        return items;
    }

    void reset() { m_offset = 0; }

    usize used() const { return m_offset; }
    usize peak() const { return m_peak; }
    usize capacity() const { return m_capacity; }

private:
    u8* m_memory = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
    usize m_peak = 0;
};

} // namespace vd
