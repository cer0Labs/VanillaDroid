#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>

namespace vd {

// Asset paths are case-insensitive with '\' separators in the client. The canonical form is
// lowercase ASCII with '/', and packs are indexed by the FNV-1a 64 hash of that form.

constexpr char normalizeAssetPathChar(char c) {
    if (c == '\\') {
        return '/';
    }
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string normalizeAssetPath(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        c = normalizeAssetPathChar(c);
    }
    return out;
}

constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

// FNV-1a 64 over raw bytes; chain calls by passing the previous result as `hash`.
inline u64 fnv1a64(const void* data, usize size, u64 hash = kFnvOffset) {
    const auto* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

// Hash of the canonical form, normalizing on the fly (no allocation): any spelling of a path
// ("Interface\\Icons\\X.blp", "interface/icons/x.blp") gives the same hash.
constexpr u64 assetPathHash(std::string_view path) {
    u64 hash = kFnvOffset;
    for (char c : path) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(c));
        hash *= kFnvPrime;
    }
    return hash;
}

// FNV-1a 64 of a string as it is (case-sensitive): keys of the interface's texts (core/Text).
constexpr u64 stringHash(std::string_view text) {
    u64 hash = kFnvOffset;
    for (char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace vd
