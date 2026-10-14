#pragma once

#include "core/Types.h"
#include "pack/ModelFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vd::bake {

// Blob with a fixed-size header followed by sections appended at 16-byte alignment; offsets are relative to
// the blob start.
class BlobWriter {
public:
    explicit BlobWriter(usize headerSize) : m_blob(headerSize) {}

    template <typename T>
    u32 append(const std::vector<T>& items) {
        if (items.empty()) {
            return 0;
        }
        const usize offset = (m_blob.size() + 15) / 16 * 16;
        m_blob.resize(offset + items.size() * sizeof(T));
        std::memcpy(m_blob.data() + offset, items.data(), items.size() * sizeof(T));
        return static_cast<u32>(offset);
    }

    template <typename T>
    std::vector<u8> finish(const T& header) {
        std::memcpy(m_blob.data(), &header, sizeof(header));
        return std::move(m_blob);
    }

private:
    std::vector<u8> m_blob;
};

inline i8 toSnorm8(f32 value) {
    return static_cast<i8>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
}

inline i16 toSnorm16(f32 value) {
    return static_cast<i16>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

// Client blend modes (EGxBlend, same numbering in M2 and WMO) -> draw classes.
inline pack::ModelBlend toModelBlend(u32 blend) {
    switch (blend) {
    case 0: return pack::ModelBlend::Opaque;
    case 1: return pack::ModelBlend::AlphaKey;
    case 2: return pack::ModelBlend::Alpha;
    case 3: // no-alpha add
    case 4: return pack::ModelBlend::Add;
    case 5: return pack::ModelBlend::Mod;
    case 6: return pack::ModelBlend::Mod2x;
    default: return pack::ModelBlend::Opaque;
    }
}

} // namespace vd::bake
