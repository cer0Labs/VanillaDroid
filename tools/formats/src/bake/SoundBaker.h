#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <span>
#include <vector>

namespace vd::bake {

// Bump when the baked sound blobs change for the same input (the bake cache keys on it).
constexpr u32 kSoundEncoderVersion = 2; // 2: the first block starts at the best step index

// Bytes per IMA ADPCM block of a channel: 1017 frames (46 ms at 22 kHz) per block.
constexpr u32 kSoundBlockBytesPerChannel = 512;

// A client WAV (PCM 8/16 bit or IMA ADPCM) -> SoundHeader + IMA ADPCM blocks (pack/SoundFormat.h).
Result<std::vector<u8>> bakeWavSound(std::span<const u8> wavBytes);

// A client MP3 -> SoundHeader + its MPEG audio frames (ID3 tags dropped); decoded once to validate it and count
// its frames.
Result<std::vector<u8>> bakeMp3Sound(std::span<const u8> mp3Bytes);

} // namespace vd::bake
