#include "formats/Wav.h"

#include "audio/Adpcm.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace vd::formats {
namespace {

u16 readU16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 readU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

} // namespace

Result<WavInfo> parseWav(std::span<const u8> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return Error{"not a RIFF/WAVE file"};
    }
    WavInfo info;
    bool haveFormat = false;
    bool haveData = false;
    u32 factFrames = 0;
    usize offset = 12;
    while (offset + 8 <= bytes.size()) {
        const u8* chunk = bytes.data() + offset;
        const u32 size = readU32(chunk + 4);
        const usize available = bytes.size() - offset - 8;
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || size > available) {
                return Error{"truncated fmt chunk"};
            }
            info.format = readU16(chunk + 8);
            info.channels = readU16(chunk + 10);
            info.sampleRate = readU32(chunk + 12);
            info.blockAlign = readU16(chunk + 20);
            info.bitsPerSample = readU16(chunk + 22);
            if (size >= 20 && info.format == kWaveImaAdpcm) {
                info.framesPerBlock = readU16(chunk + 26); // cbSize at 24, wSamplesPerBlock at 26
            }
            haveFormat = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            // Some files declare more data than they hold: keep what is there.
            info.data = bytes.subspan(offset + 8, std::min<usize>(size, available));
            haveData = true;
        } else if (std::memcmp(chunk, "fact", 4) == 0 && size >= 4 && size <= available) {
            factFrames = readU32(chunk + 8);
        }
        offset += 8 + size + (size & 1);
    }
    if (!haveFormat || !haveData) {
        return Error{"missing fmt or data chunk"};
    }
    if (info.channels < 1 || info.channels > 2 || info.sampleRate < 1000 || info.sampleRate > 96000 ||
        info.blockAlign == 0) {
        return Error{"unsupported channels / rate / block align"};
    }
    switch (info.format) {
    case kWavePcm:
        if (info.bitsPerSample != 8 && info.bitsPerSample != 16) {
            return Error{"unsupported PCM bit depth " + std::to_string(info.bitsPerSample)};
        }
        // The client's stereo files say blockAlign 2 (one channel's worth): the frame size comes from the format.
        info.blockAlign = static_cast<u16>(info.channels * info.bitsPerSample / 8);
        info.frameCount = static_cast<u32>(info.data.size() / info.blockAlign);
        break;
    case kWaveImaAdpcm: {
        const u32 perBlock = audio::imaFramesPerBlock(info.blockAlign, info.channels);
        if (info.bitsPerSample != 4 || perBlock == 0) {
            return Error{"unsupported IMA ADPCM layout"};
        }
        info.framesPerBlock = static_cast<u16>(perBlock);
        const usize blocks = info.data.size() / info.blockAlign;
        info.frameCount = static_cast<u32>(blocks * perBlock);
        if (factFrames != 0) {
            info.frameCount = std::min(info.frameCount, factFrames);
        }
        break;
    }
    default: return Error{"unsupported WAV codec " + std::to_string(info.format)};
    }
    return info;
}

Result<std::vector<i16>> decodeWav(const WavInfo& wav) {
    std::vector<i16> out(static_cast<usize>(wav.frameCount) * wav.channels);
    const u8* data = wav.data.data();
    switch (wav.format) {
    case kWavePcm:
        for (usize i = 0; i < out.size(); ++i) {
            out[i] = wav.bitsPerSample == 8 ? static_cast<i16>((static_cast<i32>(data[i]) - 128) << 8)
                                            : static_cast<i16>(readU16(data + 2 * i));
        }
        return out;
    case kWaveImaAdpcm: {
        std::vector<i16> block(static_cast<usize>(wav.framesPerBlock) * wav.channels);
        usize written = 0;
        for (usize offset = 0; offset + wav.blockAlign <= wav.data.size() && written < wav.frameCount;
             offset += wav.blockAlign) {
            const u32 frames =
                audio::decodeImaBlock(wav.data.subspan(offset, wav.blockAlign), wav.channels, block.data());
            const usize take = std::min<usize>(frames, wav.frameCount - written);
            std::memcpy(out.data() + written * wav.channels, block.data(), take * wav.channels * sizeof(i16));
            written += take;
        }
        out.resize(written * wav.channels);
        return out;
    }
    default: return Error{"unsupported WAV codec"};
    }
}

} // namespace vd::formats
