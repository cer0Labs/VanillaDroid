#pragma once

#include "core/Types.h"

#include <atomic>
#include <cstring>
#include <span>
#include <vector>

namespace vd::net {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding from alignas: intended, keeps the two indices on separate cache lines
#endif

// Lock-free ring of variable-size records for exactly one producer and one consumer thread: how packets
// move between the network thread and the main thread without locks or per-packet allocations. A record
// is contiguous in memory (a wrap marker fills the tail when it would not fit) and stays valid until pop().
class PacketRing {
public:
    // `capacity`: bytes, a power of two; the largest record must fit in it.
    explicit PacketRing(u32 capacity) : m_buffer(capacity), m_mask(capacity - 1) {}

    // Producer. False when there is no room yet (retry later).
    bool push(u16 kind, u16 opcode, std::span<const u8> payload) {
        const u64 write = m_write.load(std::memory_order_relaxed);
        const u64 read = m_read.load(std::memory_order_acquire);
        const u32 total = recordSize(static_cast<u32>(payload.size()));
        const u32 offset = static_cast<u32>(write & m_mask);
        const u32 tail = static_cast<u32>(m_buffer.size()) - offset;
        const u32 pad = tail < total ? tail : 0; // doesn't fit before the end: skip to the start
        if (total > m_buffer.size() || write + pad + total - read > m_buffer.size()) {
            return false;
        }
        if (pad) {
            writeHeader(offset, kWrap, 0, 0);
        }
        const u32 at = static_cast<u32>((write + pad) & m_mask);
        writeHeader(at, kind, opcode, static_cast<u32>(payload.size()));
        if (!payload.empty()) {
            std::memcpy(m_buffer.data() + at + kHeader, payload.data(), payload.size());
        }
        m_write.store(write + pad + total, std::memory_order_release);
        return true;
    }

    // Consumer. False when empty.
    bool front(u16& kind, u16& opcode, std::span<const u8>& payload) {
        u64 read = m_read.load(std::memory_order_relaxed);
        const u64 write = m_write.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        u32 at = static_cast<u32>(read & m_mask);
        u32 size = 0;
        readHeader(at, kind, opcode, size);
        if (kind == kWrap) { // skip the unused tail
            read += m_buffer.size() - at;
            m_read.store(read, std::memory_order_release);
            if (read == write) {
                return false;
            }
            at = 0;
            readHeader(at, kind, opcode, size);
        }
        payload = {m_buffer.data() + at + kHeader, size};
        m_frontSize = recordSize(size);
        return true;
    }
    void pop() {
        m_read.store(m_read.load(std::memory_order_relaxed) + m_frontSize, std::memory_order_release);
        m_frontSize = 0;
    }

private:
    static constexpr u32 kHeader = 8; // size u32, kind u16, opcode u16
    static constexpr u16 kWrap = 0xFFFF;
    static u32 recordSize(u32 payload) { return (kHeader + payload + 7u) & ~7u; }
    void writeHeader(u32 at, u16 kind, u16 opcode, u32 size) {
        std::memcpy(m_buffer.data() + at, &size, 4);
        std::memcpy(m_buffer.data() + at + 4, &kind, 2);
        std::memcpy(m_buffer.data() + at + 6, &opcode, 2);
    }
    void readHeader(u32 at, u16& kind, u16& opcode, u32& size) const {
        std::memcpy(&size, m_buffer.data() + at, 4);
        std::memcpy(&kind, m_buffer.data() + at + 4, 2);
        std::memcpy(&opcode, m_buffer.data() + at + 6, 2);
    }

    std::vector<u8> m_buffer;
    u64 m_mask;
    u32 m_frontSize = 0; // consumer only
    alignas(64) std::atomic<u64> m_write{0}; // producer
    alignas(64) std::atomic<u64> m_read{0};  // consumer
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace vd::net
