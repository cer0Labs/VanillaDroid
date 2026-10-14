#include "audio/SoundAsset.h"

#include "audio/Adpcm.h"

#include <cstring>

namespace vd::audio {

bool parseSound(const u8* blob, usize size, SoundRef& out) {
    out = {};
    if (!blob || size < sizeof(pack::SoundHeader)) {
        return false;
    }
    pack::SoundHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.version != pack::kSoundFormatVersion || header.channels < 1 || header.channels > 2 ||
        header.sampleRate < 1000 || header.sampleRate > 96000 || header.dataOffset < sizeof(header) ||
        header.dataOffset > size || header.dataSize > size - header.dataOffset || header.dataSize == 0) {
        return false;
    }
    switch (static_cast<pack::SoundCodec>(header.codec)) {
    case pack::SoundCodec::Pcm16:
        if (static_cast<u64>(header.frameCount) * header.channels * 2 > header.dataSize) {
            return false;
        }
        break;
    case pack::SoundCodec::ImaAdpcm: {
        const u32 perBlock = imaFramesPerBlock(header.blockAlign, header.channels);
        if (perBlock == 0 || perBlock > kMaxBlockFrames || perBlock != header.framesPerBlock ||
            header.blockAlign % (4u * header.channels) != 0) {
            return false;
        }
        const u64 blocks = header.dataSize / header.blockAlign;
        if (static_cast<u64>(header.frameCount) > blocks * perBlock) {
            return false;
        }
        break;
    }
    case pack::SoundCodec::Mp3: break; // validated frame by frame while decoding
    default: return false;
    }
    out.data = blob + header.dataOffset;
    out.size = header.dataSize;
    out.codec = static_cast<pack::SoundCodec>(header.codec);
    out.channels = static_cast<u8>(header.channels);
    out.sampleRate = header.sampleRate;
    out.frameCount = header.frameCount;
    out.blockAlign = header.blockAlign;
    out.framesPerBlock = header.framesPerBlock;
    return true;
}

} // namespace vd::audio
