#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace vd::formats {

// Helpers for the IFF-like chunk files of the client (ADT, WDT, WDL, WMO): u32 magic (reversed on disk),
// u32 size, payload.

constexpr u32 kChunkHeaderSize = 8;

// Magic as stored on disk (little-endian u32 of the reversed characters): chunkId("MVER") matches the
// bytes "REVM".
constexpr u32 chunkId(const char (&tag)[5]) {
    return (u32(u8(tag[0])) << 24) | (u32(u8(tag[1])) << 16) | (u32(u8(tag[2])) << 8) | u32(u8(tag[3]));
}

template <typename T>
T load(const u8* at) {
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}

struct Chunk {
    u32 id = 0;
    std::span<const u8> data;
};

// Chunk header at `offset`, with its payload checked against the buffer.
inline Result<Chunk> chunkAt(std::span<const u8> file, usize offset) {
    if (offset + kChunkHeaderSize > file.size()) {
        return Error{"Chunk header out of bounds at " + std::to_string(offset)};
    }
    const u32 id = load<u32>(file.data() + offset);
    const u32 size = load<u32>(file.data() + offset + 4);
    if (offset + kChunkHeaderSize + size > file.size()) {
        return Error{"Chunk data out of bounds at " + std::to_string(offset)};
    }
    return Chunk{id, file.subspan(offset + kChunkHeaderSize, size)};
}

inline std::string idToString(u32 id) {
    const char text[5] = {char(id >> 24), char(id >> 16), char(id >> 8), char(id), 0};
    return text;
}

// Every chunk of a buffer in file order. A last chunk that runs past the end of the buffer is cut at the
// end (a few client files are a byte short, e.g. Undercity_144.wmo); anything else malformed is an error.
inline Result<std::vector<Chunk>> readChunks(std::span<const u8> data) {
    std::vector<Chunk> chunks;
    usize offset = 0;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            const u32 id = load<u32>(data.data() + offset);
            chunks.push_back({id, data.subspan(offset + kChunkHeaderSize)});
            break;
        }
        chunks.push_back(chunk.value());
        offset += kChunkHeaderSize + chunk.value().data.size();
    }
    return chunks;
}

// NUL-terminated string at `offset` of a string block ("" if out of range).
inline std::string stringAt(std::span<const u8> block, u32 offset) {
    if (offset >= block.size()) {
        return {};
    }
    const char* text = reinterpret_cast<const char*>(block.data() + offset);
    return std::string(text, ::strnlen(text, block.size() - offset));
}

// Records of a fixed-size struct in a chunk payload.
template <typename T>
std::vector<T> loadArray(std::span<const u8> data) {
    std::vector<T> out(data.size() / sizeof(T));
    if (!out.empty()) {
        std::memcpy(out.data(), data.data(), out.size() * sizeof(T));
    }
    return out;
}

} // namespace vd::formats
