#pragma once

#include "core/Types.h"

#include <span>

namespace vd::audio {

// IMA ADPCM in the WAV (Microsoft IMA, WAVE_FORMAT_IMA_ADPCM) block layout: per channel a 4-byte header (the first
// sample as i16, the step index, a zero byte), then groups of 4 bytes = 8 samples per channel, channels interleaved
// group by group, low nibble first. Each block decodes on its own (sounds loop and start at any block). The baker
// encodes the client's sound effects with it (4 bits per sample) and the mixer decodes them while playing.

constexpr u32 imaFramesPerBlock(u32 blockAlign, u32 channels) {
    return channels == 0 || blockAlign <= 4 * channels ? 0 : (blockAlign / channels - 4) * 2 + 1;
}

// Decodes one block of `channels` (1 or 2) interleaved channels into `out` (framesPerBlock * channels samples).
// Returns the frames written (0 if the block is too short for its header).
u32 decodeImaBlock(std::span<const u8> block, u32 channels, i16* out);

// Encodes `frames` (at most imaFramesPerBlock(blockAlign, channels)) interleaved samples into one block of
// `blockAlign` bytes; a short last block is padded with silence. `stepIndex` (one per channel) carries the
// quantizer state from block to block, as encoders do: start it at 0.
void encodeImaBlock(const i16* samples, u32 frames, u32 channels, u32 blockAlign, u8* out, u8* stepIndex);

// The step index that encodes the first frames of `channel` best: where a sound starts, the quantizer has no
// history and a small step smears the attack (encoders start the first block with it instead of 0).
u8 bestImaStepIndex(const i16* samples, u32 frames, u32 channels, u32 channel);

} // namespace vd::audio
