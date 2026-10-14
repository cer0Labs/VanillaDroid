#include "net/AccountDataMessages.h"

#include "net/ByteBuffer.h"

#include <zlib.h>

#include <cstring>

namespace vd::net {
namespace {

// --- MD5 (RFC 1321) ----------------------------------------------------------------------------------------------

constexpr u32 kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                            14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
constexpr u32 kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

u32 rotateLeft(u32 x, u32 c) {
    return (x << c) | (x >> (32u - c));
}

void md5Block(std::array<u32, 4>& state, const u8* block) {
    u32 m[16];
    for (u32 i = 0; i < 16; ++i) {
        m[i] = static_cast<u32>(block[i * 4]) | (static_cast<u32>(block[i * 4 + 1]) << 8) |
               (static_cast<u32>(block[i * 4 + 2]) << 16) | (static_cast<u32>(block[i * 4 + 3]) << 24);
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (u32 i = 0; i < 64; ++i) {
        u32 f = 0, g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const u32 next = d;
        d = c;
        c = b;
        b = b + rotateLeft(a + f + kSine[i] + m[g], kShift[i]);
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

Md5 md5(std::span<const u8> data) {
    std::array<u32, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    usize at = 0;
    for (; data.size() - at >= 64; at += 64) {
        md5Block(state, data.data() + at);
    }
    // The tail, the 0x80 byte, zeros up to 56 bytes of the last block, the length in bits.
    u8 tail[128] = {};
    const usize rest = data.size() - at;
    if (rest > 0) {
        std::memcpy(tail, data.data() + at, rest);
    }
    tail[rest] = 0x80;
    const usize blocks = rest + 1 + 8 <= 64 ? 1 : 2;
    const u64 bits = static_cast<u64>(data.size()) * 8u;
    for (u32 i = 0; i < 8; ++i) {
        tail[blocks * 64 - 8 + i] = static_cast<u8>(bits >> (8 * i));
    }
    for (usize i = 0; i < blocks; ++i) {
        md5Block(state, tail + i * 64);
    }
    Md5 out{};
    for (u32 i = 0; i < 4; ++i) {
        for (u32 j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<u8>(state[i] >> (8 * j));
        }
    }
    return out;
}

bool md5IsZero(const Md5& digest) {
    for (const u8 b : digest) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

bool parseTutorialFlags(std::span<const u8> payload, TutorialWords& out) {
    ByteReader r(payload);
    for (u32& word : out) {
        word = r.u32v();
    }
    return r.ok();
}

void writeTutorialFlag(std::vector<u8>& out, u32 id) {
    ByteWriter(out).u32v(id);
}

bool parseAccountDataMd5(std::span<const u8> payload, std::array<Md5, kAccountDataTypes>& out) {
    ByteReader r(payload);
    for (Md5& digest : out) {
        r.bytes(digest.data(), digest.size());
    }
    return r.ok();
}

void writeRequestAccountData(std::vector<u8>& out, u32 type) {
    ByteWriter(out).u32v(type);
}

bool writeUpdateAccountData(std::vector<u8>& out, u32 type, std::string_view text) {
    if (text.size() > kAccountDataMaxSize) {
        return false;
    }
    if (text.empty()) {
        ByteWriter(out).u32v(type).u32v(0);
        return true;
    }
    // A plain zlib stream (deflate with its header and adler32), as compress() makes it.
    z_stream z{};
    if (deflateInit(&z, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return false;
    }
    const usize start = out.size();
    const uLong bound = deflateBound(&z, static_cast<uLong>(text.size()));
    out.resize(start + 8 + bound);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    z.avail_in = static_cast<uInt>(text.size());
    z.next_out = out.data() + start + 8;
    z.avail_out = static_cast<uInt>(bound);
    const int result = deflate(&z, Z_FINISH);
    const uLong written = z.total_out;
    deflateEnd(&z);
    if (result != Z_STREAM_END) {
        out.resize(start);
        return false;
    }
    out.resize(start + 8 + written);
    const u32 size = static_cast<u32>(text.size());
    std::memcpy(out.data() + start, &type, 4);
    std::memcpy(out.data() + start + 4, &size, 4);
    return true;
}

bool parseUpdateAccountData(std::span<const u8> payload, AccountDataUpdate& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    const u32 size = r.u32v();
    out.text.clear();
    if (!r.ok() || size > kAccountDataMaxSize) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    const std::span<const u8> stream = r.view(r.remaining());
    out.text.resize(size);
    uLongf inflated = size;
    if (uncompress(reinterpret_cast<Bytef*>(out.text.data()), &inflated, stream.data(),
                   static_cast<uLong>(stream.size())) != Z_OK ||
        inflated != size) {
        out.text.clear();
        return false;
    }
    return true;
}

} // namespace vd::net
