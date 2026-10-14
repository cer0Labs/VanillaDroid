#include "formats/PngWriter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace vd::formats {
namespace {

u32 crc32(const u8* data, usize size, u32 crc = 0) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (usize i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void appendBe32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value >> 24));
    out.push_back(static_cast<u8>(value >> 16));
    out.push_back(static_cast<u8>(value >> 8));
    out.push_back(static_cast<u8>(value));
}

void appendChunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& payload) {
    appendBe32(out, static_cast<u32>(payload.size()));
    const usize typeStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    appendBe32(out, crc32(out.data() + typeStart, 4 + payload.size()));
}

} // namespace

Result<void> writePng(const std::filesystem::path& path, u32 width, u32 height, const u8* rgba) {
    // Raw scanlines: filter byte 0 + RGBA row.
    std::vector<u8> raw;
    raw.reserve((usize{width} * 4 + 1) * height);
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        const u8* row = rgba + usize{y} * width * 4;
        raw.insert(raw.end(), row, row + usize{width} * 4);
    }

    // zlib stream made of "stored" (uncompressed) deflate blocks.
    std::vector<u8> zlib{0x78, 0x01};
    usize pos = 0;
    do {
        const usize len = std::min<usize>(65535, raw.size() - pos);
        const bool last = pos + len == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<u8>(len));
        zlib.push_back(static_cast<u8>(len >> 8));
        zlib.push_back(static_cast<u8>(~len));
        zlib.push_back(static_cast<u8>(~len >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    } while (pos < raw.size());
    u32 a = 1;
    u32 b = 0;
    for (u8 byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    appendBe32(zlib, (b << 16) | a);

    std::vector<u8> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> header;
    appendBe32(header, width);
    appendBe32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter, no interlace
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", zlib);
    appendChunk(png, "IEND", {});

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file) {
        return Error{"Cannot write " + path.string()};
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    std::fclose(file);
    return ok ? Result<void>{} : Result<void>{Error{"Short write to " + path.string()}};
}

} // namespace vd::formats
