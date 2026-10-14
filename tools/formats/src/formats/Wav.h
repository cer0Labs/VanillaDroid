#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <span>
#include <vector>

namespace vd::formats {

// RIFF/WAVE codecs read here. The 1.12 client ships only PCM (7,154 files, `vd_baker scan-sounds`): 16-bit, mono or
// stereo, 22 or 44 kHz. IMA ADPCM is read too (the baker's own output).
constexpr u16 kWavePcm = 0x0001;
constexpr u16 kWaveImaAdpcm = 0x0011;

// A WAV file of the client: its format chunk and where the samples are (a view into the file bytes).
struct WavInfo {
    u16 format = 0; // kWave*
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 bitsPerSample = 0;
    u16 blockAlign = 0;     // bytes per frame (PCM) or per block (ADPCM)
    u16 framesPerBlock = 0; // ADPCM
    u32 frameCount = 0;     // whole frames in `data`
    std::span<const u8> data;
};

// Reads the chunks of a WAV file (fmt, data and, for ADPCM, the frame count of fact). Rejects anything it could
// not decode: other codecs, more than two channels, a truncated fmt chunk.
Result<WavInfo> parseWav(std::span<const u8> bytes);

// The samples as interleaved signed 16-bit frames (PCM 8/16 bit, IMA ADPCM).
Result<std::vector<i16>> decodeWav(const WavInfo& wav);

} // namespace vd::formats
