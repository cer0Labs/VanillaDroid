#include "audio/Adpcm.h"

#include <algorithm>
#include <cstring>

namespace vd::audio {
namespace {

constexpr i16 kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr i8 kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
    i32 predictor = 0;
    i32 index = 0;

    i16 step(u32 nibble) {
        const i32 stepSize = kStepTable[index];
        i32 diff = stepSize >> 3;
        if (nibble & 1) {
            diff += stepSize >> 2;
        }
        if (nibble & 2) {
            diff += stepSize >> 1;
        }
        if (nibble & 4) {
            diff += stepSize;
        }
        predictor = std::clamp(nibble & 8 ? predictor - diff : predictor + diff, -32768, 32767);
        index = std::clamp(index + kIndexTable[nibble], 0, 88);
        return static_cast<i16>(predictor);
    }
};

// The nibble whose reconstruction comes closest to `target` (the decoder's arithmetic, so both stay in step).
u32 closestNibble(const ImaState& state, i32 target) {
    u32 best = 0;
    i32 bestError = 1 << 30;
    for (u32 nibble = 0; nibble < 16; ++nibble) {
        ImaState trial = state;
        const i32 error = std::abs(trial.step(nibble) - target);
        if (error < bestError) {
            bestError = error;
            best = nibble;
        }
    }
    return best;
}

} // namespace

u32 decodeImaBlock(std::span<const u8> block, u32 channels, i16* out) {
    if (channels == 0 || channels > 2 || block.size() < 4 * channels) {
        return 0;
    }
    ImaState state[2];
    for (u32 c = 0; c < channels; ++c) {
        const u8* header = block.data() + 4 * c;
        state[c].predictor = static_cast<i16>(header[0] | (header[1] << 8));
        state[c].index = std::min<i32>(header[2], 88);
        out[c] = static_cast<i16>(state[c].predictor);
    }
    // Groups of 4 bytes per channel = 8 frames.
    const u32 groups = static_cast<u32>(block.size() - 4 * channels) / (4 * channels);
    const u8* data = block.data() + 4 * channels;
    for (u32 g = 0; g < groups; ++g) {
        for (u32 c = 0; c < channels; ++c) {
            const u8* bytes = data + (g * channels + c) * 4;
            i16* dst = out + (1 + g * 8) * channels + c;
            for (u32 b = 0; b < 4; ++b) {
                dst[(b * 2) * channels] = state[c].step(bytes[b] & 0x0F);
                dst[(b * 2 + 1) * channels] = state[c].step(bytes[b] >> 4);
            }
        }
    }
    return 1 + groups * 8;
}

void encodeImaBlock(const i16* samples, u32 frames, u32 channels, u32 blockAlign, u8* out, u8* stepIndex) {
    std::memset(out, 0, blockAlign);
    const u32 capacity = imaFramesPerBlock(blockAlign, channels);
    frames = std::min(frames, capacity);
    if (frames == 0) {
        return;
    }
    ImaState state[2];
    for (u32 c = 0; c < channels; ++c) {
        const i16 first = samples[c];
        state[c].predictor = first;
        state[c].index = stepIndex[c];
        out[4 * c] = static_cast<u8>(first & 0xFF);
        out[4 * c + 1] = static_cast<u8>((static_cast<u16>(first) >> 8) & 0xFF);
        out[4 * c + 2] = static_cast<u8>(state[c].index);
    }
    u8* data = out + 4 * channels;
    for (u32 frame = 1; frame < capacity; ++frame) {
        const u32 group = (frame - 1) / 8;
        const u32 within = (frame - 1) % 8;
        for (u32 c = 0; c < channels; ++c) {
            // Past the end: hold the last sample (silence would click at the end of a looping sound).
            const i32 target = samples[std::min(frame, frames - 1) * channels + c];
            const u32 best = closestNibble(state[c], target);
            state[c].step(best);
            u8& byte = data[(group * channels + c) * 4 + within / 2];
            byte = static_cast<u8>(byte | (within % 2 == 0 ? best : best << 4));
        }
    }
    for (u32 c = 0; c < channels; ++c) {
        stepIndex[c] = static_cast<u8>(state[c].index);
    }
}

u8 bestImaStepIndex(const i16* samples, u32 frames, u32 channels, u32 channel) {
    constexpr u32 kLookahead = 32;
    const u32 n = std::min(frames, kLookahead);
    if (n < 2) {
        return 0;
    }
    u8 best = 0;
    i64 bestError = -1;
    for (i32 index = 0; index <= 88; ++index) {
        ImaState state;
        state.predictor = samples[channel];
        state.index = index;
        i64 error = 0;
        for (u32 i = 1; i < n; ++i) {
            const i32 target = samples[i * channels + channel];
            const i32 d = state.step(closestNibble(state, target)) - target;
            error += static_cast<i64>(d) * d;
        }
        if (bestError < 0 || error < bestError) {
            bestError = error;
            best = static_cast<u8>(index);
        }
    }
    return best;
}

} // namespace vd::audio
