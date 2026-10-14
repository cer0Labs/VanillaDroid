#include "bake/SoundBaker.h"

#include "audio/Adpcm.h"
#include "audio/Mp3Frame.h"
#include "audio/SoundAsset.h"
#include "formats/Wav.h"
#include "pack/SoundFormat.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

namespace vd::bake {
namespace {

std::vector<u8> withHeader(pack::SoundHeader header, std::span<const u8> data) {
    header.version = pack::kSoundFormatVersion;
    header.dataOffset = sizeof(pack::SoundHeader);
    header.dataSize = static_cast<u32>(data.size());
    std::vector<u8> blob(sizeof(header) + data.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), data.data(), data.size());
    return blob;
}

// Size of an ID3v2 tag at the start (header + body + optional footer), 0 if none.
usize id3v2Size(std::span<const u8> bytes) {
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "ID3", 3) != 0) {
        return 0;
    }
    const usize body = (static_cast<usize>(bytes[6] & 0x7F) << 21) | (static_cast<usize>(bytes[7] & 0x7F) << 14) |
                       (static_cast<usize>(bytes[8] & 0x7F) << 7) | static_cast<usize>(bytes[9] & 0x7F);
    const usize footer = (bytes[5] & 0x10) ? 10 : 0;
    return std::min(bytes.size(), 10 + body + footer);
}

} // namespace

Result<std::vector<u8>> bakeWavSound(std::span<const u8> wavBytes) {
    Result<formats::WavInfo> wav = formats::parseWav(wavBytes);
    if (!wav) {
        return wav.error();
    }
    Result<std::vector<i16>> samples = formats::decodeWav(wav.value());
    if (!samples) {
        return samples.error();
    }
    const u32 channels = wav.value().channels;
    const u32 frames = static_cast<u32>(samples.value().size() / channels);
    if (frames == 0) {
        return Error{"no samples"};
    }
    const u32 blockAlign = kSoundBlockBytesPerChannel * channels;
    const u32 perBlock = audio::imaFramesPerBlock(blockAlign, channels);
    const u32 blocks = (frames + perBlock - 1) / perBlock;
    std::vector<u8> data(static_cast<usize>(blocks) * blockAlign);
    u8 stepIndex[2] = {};
    for (u32 c = 0; c < channels; ++c) {
        stepIndex[c] = audio::bestImaStepIndex(samples.value().data(), frames, channels, c);
    }
    for (u32 b = 0; b < blocks; ++b) {
        const u32 first = b * perBlock;
        audio::encodeImaBlock(samples.value().data() + static_cast<usize>(first) * channels,
                              std::min(perBlock, frames - first), channels, blockAlign,
                              data.data() + static_cast<usize>(b) * blockAlign, stepIndex);
    }
    pack::SoundHeader header{};
    header.codec = static_cast<u16>(pack::SoundCodec::ImaAdpcm);
    header.channels = static_cast<u16>(channels);
    header.sampleRate = wav.value().sampleRate;
    header.frameCount = frames;
    header.blockAlign = static_cast<u16>(blockAlign);
    header.framesPerBlock = static_cast<u16>(perBlock);
    std::vector<u8> blob = withHeader(header, data);
    audio::SoundRef check;
    if (!audio::parseSound(blob.data(), blob.size(), check)) {
        return Error{"baked sound fails its own validation"};
    }
    return blob;
}

Result<std::vector<u8>> bakeMp3Sound(std::span<const u8> mp3Bytes) {
    std::span<const u8> audio = mp3Bytes.subspan(id3v2Size(mp3Bytes));
    if (audio.size() >= 128 && std::memcmp(audio.data() + audio.size() - 128, "TAG", 3) == 0) {
        audio = audio.first(audio.size() - 128); // ID3v1
    }
    // Decode every frame once: the frame count, a constant format, and the byte range the frames cover (junk
    // before the first frame and after the last one is dropped).
    auto decoder = std::make_unique<std::max_align_t[]>(vdMp3DecoderSize() / sizeof(std::max_align_t) + 1);
    vdMp3DecoderInit(decoder.get());
    auto pcm = std::make_unique<i16[]>(VD_MP3_MAX_FRAME_SAMPLES * 2);
    usize offset = 0;
    usize firstFrame = audio.size();
    usize end = 0;
    u64 frames = 0;
    u32 channels = 0;
    u32 sampleRate = 0;
    while (offset < audio.size()) {
        int consumed = 0, frameChannels = 0, frameRate = 0;
        const int remaining = static_cast<int>(std::min<usize>(audio.size() - offset, 1 << 20));
        const int samples = vdMp3DecodeFrame(decoder.get(), audio.data() + offset, remaining, pcm.get(), &consumed,
                                             &frameChannels, &frameRate);
        if (consumed <= 0) {
            break; // no further frame
        }
        if (samples > 0) {
            if (channels == 0) {
                channels = static_cast<u32>(frameChannels);
                sampleRate = static_cast<u32>(frameRate);
            } else if (static_cast<u32>(frameChannels) != channels || static_cast<u32>(frameRate) != sampleRate) {
                return Error{"MP3 changes format mid-stream"};
            }
            firstFrame = std::min(firstFrame, offset);
            frames += static_cast<u64>(samples);
            end = offset + static_cast<usize>(consumed);
        }
        offset += static_cast<usize>(consumed);
    }
    if (frames == 0 || channels < 1 || channels > 2 || firstFrame >= end) {
        return Error{"no MPEG audio frames"};
    }
    pack::SoundHeader header{};
    header.codec = static_cast<u16>(pack::SoundCodec::Mp3);
    header.channels = static_cast<u16>(channels);
    header.sampleRate = sampleRate;
    header.frameCount = static_cast<u32>(frames);
    std::vector<u8> blob = withHeader(header, audio.subspan(firstFrame, end - firstFrame));
    audio::SoundRef check;
    if (!audio::parseSound(blob.data(), blob.size(), check)) {
        return Error{"baked sound fails its own validation"};
    }
    return blob;
}

} // namespace vd::bake
