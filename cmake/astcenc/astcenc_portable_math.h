// Phase 22 (ADR-0005 §3: the same packs on every platform). astc-encoder's builds are invariant across
// compilers and instruction sets, but for the C library: astcenc_weight_align.cpp fills its angular tables
// with sinf / cosf, and astcenc_entry.cpp takes a logf when it sets a context up. Those differ in the last
// bit between libraries (MSVC's, bionic's, Apple's; MSVC's even between CPUs, as it takes an FMA3 path when
// there is one), and a bit there can turn a close choice of the encoder the other way: a phone baked one
// block differently in 19 of the 58,502 textures. The two files are built through astcenc_*_portable.cpp,
// which include them after this header: the three functions computed in double and rounded once, the value
// every library gives but for vanishingly rare near-ties.
//
// Included after every header the upstream file uses, so that the macros only reach the upstream code itself.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vd_astcenc {

inline float sinf(float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    // The one entry of the tables where MSVC's sinf on the PC the packs were baked with so far is a bit off
    // the rounded double, sin_table[59][5]: kept, so those textures stay the same.
    if (bits == 0x420d38d9u) {
        const uint32_t msvc = 0xbf2e1fb3u;
        float value;
        std::memcpy(&value, &msvc, sizeof(value));
        return value;
    }
    return static_cast<float>(std::sin(static_cast<double>(x)));
}

inline float cosf(float x) {
    return static_cast<float>(std::cos(static_cast<double>(x)));
}

inline float logf(float x) {
    return static_cast<float>(std::log(static_cast<double>(x)));
}

} // namespace vd_astcenc

#define sinf(x) ::vd_astcenc::sinf(x)
#define cosf(x) ::vd_astcenc::cosf(x)
#define logf(x) ::vd_astcenc::logf(x)
