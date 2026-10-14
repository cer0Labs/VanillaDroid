#pragma once

#include "core/Hash.h"
#include "core/Types.h"
#include "pack/SoundFormat.h"

#include <string>
#include <string_view>

namespace vd::audio {

// Most frames one ADPCM block or one MP3 frame decodes to (the mixer's per-voice buffer).
constexpr u32 kMaxBlockFrames = 1152;

// A baked sound ready to play: its format and its codec data, in place in the mapped sound pack.
struct SoundRef {
    const u8* data = nullptr; // the codec data (after the header)
    u32 size = 0;
    pack::SoundCodec codec = pack::SoundCodec::Pcm16;
    u8 channels = 0;
    u32 sampleRate = 0;
    u32 frameCount = 0;
    u16 blockAlign = 0;     // ImaAdpcm
    u16 framesPerBlock = 0; // ImaAdpcm

    bool valid() const { return data != nullptr; }
    f32 seconds() const { return sampleRate ? static_cast<f32>(frameCount) / static_cast<f32>(sampleRate) : 0.0f; }
};

// SoundEntries' DirectoryBase + "\\" + File as one path: a separator between them unless the directory already ends
// with one (a few rows do), repeated separators collapsed. The baker stores the sounds under this path and the
// runtime finds them by its hash (soundPathHash, no allocation).
inline std::string joinSoundPath(std::string_view directory, std::string_view file) {
    static constexpr char kSeparator = '\\';
    std::string path;
    path.reserve(directory.size() + 1 + file.size());
    const auto append = [&path](std::string_view part) {
        for (const char c : part) {
            const bool separator = c == kSeparator || c == '/';
            if (separator && (path.empty() || path.back() == kSeparator)) {
                continue;
            }
            path.push_back(separator ? kSeparator : c);
        }
    };
    append(directory);
    if (!path.empty() && path.back() != kSeparator) {
        path.push_back(kSeparator);
    }
    append(file);
    return path;
}

inline u64 soundPathHash(std::string_view directory, std::string_view file) {
    u64 hash = kFnvOffset;
    char last = 0;
    bool any = false;
    const auto add = [&](char c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(c));
        hash *= kFnvPrime;
        last = normalizeAssetPathChar(c);
        any = true;
    };
    const auto append = [&](std::string_view part) {
        for (const char c : part) {
            const bool separator = c == '\\' || c == '/';
            if (separator && (!any || last == '/')) {
                continue;
            }
            add(c);
        }
    };
    append(directory);
    if (any && last != '/') {
        add('/');
    }
    append(file);
    return hash;
}

// Checks a sound blob (pack/SoundFormat.h: version, codec, channels, rate, the data inside the blob, the block
// layout) and points `out` at it. False when it is not something the mixer can play.
bool parseSound(const u8* blob, usize size, SoundRef& out);

} // namespace vd::audio
