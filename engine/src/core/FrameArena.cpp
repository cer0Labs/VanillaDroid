#include "core/FrameArena.h"

#include "core/Assert.h"

#include <new>

namespace vd {
namespace {

constexpr std::align_val_t kBlockAlignment{64}; // cache line

} // namespace

FrameArena::FrameArena(usize capacityBytes)
    : m_memory(static_cast<u8*>(::operator new(capacityBytes, kBlockAlignment))), m_capacity(capacityBytes) {}

FrameArena::~FrameArena() {
    ::operator delete(m_memory, kBlockAlignment);
}

void* FrameArena::allocate(usize size, usize alignment) {
    VD_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0);
    const usize start = (m_offset + alignment - 1) & ~(alignment - 1);
    if (start + size > m_capacity) {
        VD_ASSERT(!"FrameArena exhausted");
        return nullptr;
    }
    m_offset = start + size;
    if (m_offset > m_peak) {
        m_peak = m_offset;
    }
    return m_memory + start;
}

} // namespace vd
