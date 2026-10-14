#pragma once

#include "core/Types.h"

#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace vd::net {

// Little-endian reader over a packet. Reading past the end sets failed() and returns zeros instead of
// throwing (no exceptions in the engine): parse, then check ok() once.
class ByteReader {
public:
    ByteReader() = default;
    explicit ByteReader(std::span<const u8> data) : m_data(data) {}

    bool ok() const { return !m_failed; }
    bool failed() const { return m_failed; }
    // Marks the packet malformed (a value the format does not allow): every later read fails too.
    void fail() { m_failed = true; }
    usize position() const { return m_pos; }
    usize remaining() const { return m_failed ? 0 : m_data.size() - m_pos; }
    bool atEnd() const { return remaining() == 0; }
    std::span<const u8> rest() const { return m_failed ? std::span<const u8>{} : m_data.subspan(m_pos); }

    u8 u8v() { return read<u8>(); }
    u16 u16v() { return read<u16>(); }
    u32 u32v() { return read<u32>(); }
    u64 u64v() { return read<u64>(); }
    i32 i32v() { return read<i32>(); }
    f32 f32v() { return read<f32>(); }

    // Copies `size` bytes; zeros when out of range.
    void bytes(void* out, usize size) {
        if (!take(size)) {
            std::memset(out, 0, size);
            return;
        }
        std::memcpy(out, m_data.data() + m_pos - size, size);
    }
    std::span<const u8> view(usize size) {
        return take(size) ? m_data.subspan(m_pos - size, size) : std::span<const u8>{};
    }
    void skip(usize size) { take(size); }

    // NUL-terminated string (the terminator is consumed, not returned).
    std::string_view cstring() {
        if (m_failed) {
            return {};
        }
        const u8* begin = m_data.data() + m_pos;
        const void* end = std::memchr(begin, 0, m_data.size() - m_pos);
        if (!end) {
            m_failed = true;
            return {};
        }
        const usize length = static_cast<usize>(static_cast<const u8*>(end) - begin);
        m_pos += length + 1;
        return {reinterpret_cast<const char*>(begin), length};
    }

    // Packed GUID: a mask byte, then one byte for every set bit (the non-zero bytes of the u64).
    u64 packedGuid() {
        const u8 mask = u8v();
        u64 guid = 0;
        for (u32 i = 0; i < 8; ++i) {
            if (mask & (1u << i)) {
                guid |= static_cast<u64>(u8v()) << (i * 8);
            }
        }
        return guid;
    }

private:
    bool take(usize size) {
        if (m_failed || m_data.size() - m_pos < size) {
            m_failed = true;
            return false;
        }
        m_pos += size;
        return true;
    }
    template <typename T>
    T read() {
        T value{};
        bytes(&value, sizeof(T)); // the engine only targets little-endian CPUs (x86-64, arm64)
        return value;
    }

    std::span<const u8> m_data;
    usize m_pos = 0;
    bool m_failed = false;
};

// Little-endian writer appending to a caller-owned vector (reuse it: clear() keeps the capacity).
class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& out) : m_out(out) {}

    ByteWriter& u8v(u8 v) { return put(&v, 1); }
    ByteWriter& u16v(u16 v) { return put(&v, 2); }
    ByteWriter& u32v(u32 v) { return put(&v, 4); }
    ByteWriter& u64v(u64 v) { return put(&v, 8); }
    ByteWriter& f32v(f32 v) { return put(&v, 4); }
    ByteWriter& bytes(std::span<const u8> data) { return put(data.data(), data.size()); }
    ByteWriter& cstring(std::string_view text) {
        put(text.data(), text.size());
        return u8v(0);
    }
    ByteWriter& packedGuid(u64 guid) {
        const usize maskAt = m_out.size();
        u8v(0);
        u8 mask = 0;
        for (u32 i = 0; i < 8; ++i) {
            const u8 byte = static_cast<u8>(guid >> (i * 8));
            if (byte) {
                mask |= static_cast<u8>(1u << i);
                u8v(byte);
            }
        }
        m_out[maskAt] = mask;
        return *this;
    }
    usize size() const { return m_out.size(); }

private:
    ByteWriter& put(const void* data, usize size) {
        const u8* bytesIn = static_cast<const u8*>(data);
        m_out.insert(m_out.end(), bytesIn, bytesIn + size);
        return *this;
    }

    std::vector<u8>& m_out;
};

} // namespace vd::net
