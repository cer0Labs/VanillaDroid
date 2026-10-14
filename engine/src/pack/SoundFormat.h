#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"

namespace vd::pack {

// sound.vdpk (phase 9, `vd_baker bake --only sound`): every sound file SoundEntries.dbc names, under the client's
// path ("sound/music/zonemusic/forest/dayforest01.mp3", "sound/creature/basilisk/mbasiliskattacka.wav"), so the
// runtime finds one by hashing SoundEntries' DirectoryBase + "\" + File[i]. Each asset is a SoundHeader followed by
// its data:
//
//   Pcm16     interleaved little-endian i16 frames (sounds that would lose too much as ADPCM: none so far)
//   ImaAdpcm  blocks of blockAlign bytes in the WAV IMA ADPCM layout (audio/Adpcm.h), framesPerBlock frames each;
//             the effects, voices and ambience loops (4 bits per sample, decoded by the mixer while playing)
//   Mp3       the original MPEG audio frames with the ID3 tags stripped; the music, streamed and decoded by the
//             mixer one frame (1152 samples) at a time
//
// Stored uncompressed (AssetType::Sound): the mixer reads the mapped pack in place.

constexpr u32 kSoundFormatVersion = 1;

enum class SoundCodec : u16 {
    Pcm16 = 0,
    ImaAdpcm = 1,
    Mp3 = 2,
};

struct SoundHeader {
    u32 version; // kSoundFormatVersion
    u16 codec;   // SoundCodec
    u16 channels; // 1 or 2
    u32 sampleRate;
    u32 frameCount; // PCM frames once decoded
    u32 dataOffset; // from the start of the blob
    u32 dataSize;
    u16 blockAlign;     // ImaAdpcm: bytes per block (every channel)
    u16 framesPerBlock; // ImaAdpcm: frames per block
    u32 reserved;
};
static_assert(sizeof(SoundHeader) == 32, "SoundHeader layout");

// "ground/<map>" (the map directory in lowercase, AssetType::GroundSounds, LZ4): what the ground sounds like under
// the feet, for the footsteps. Per terrain chunk, kGroundCells x kGroundCells cells (8.3 yards) hold the
// TerrainType.dbc SoundID (1 dirt, 2 metal, 3 stone, 4 snow, 5 wood, 6 grass, 7 leaves, 8 sand, 9 soggy; 0 unknown)
// of the texture layer that shows most there: the baker blends the ADT alpha maps and takes the layer's ground effect
// (MCLY effectId -> GroundEffectTexture.dbc -> TerrainType), or its texture's name when the effect does not say.
//   GroundSoundsHeader, then tileCount x kGroundTileBytes: chunk (row * 16 + column, as the terrain's), then cell
//   (row * kGroundCells + column); rows advance along X (south), columns along Y (east), like the terrain's.
constexpr u32 kGroundSoundsVersion = 1;
constexpr u32 kGroundCells = 4;
constexpr u32 kGroundTileBytes = 256 * kGroundCells * kGroundCells;
constexpr u16 kNoGroundTile = 0xFFFF;

struct GroundSoundsHeader {
    u32 version; // kGroundSoundsVersion
    u32 tileCount;
    u16 tiles[64 * 64]; // the slot of tile (ty * 64 + tx), kNoGroundTile where the map has none
};
static_assert(sizeof(GroundSoundsHeader) == 8 + 64 * 64 * 2, "GroundSoundsHeader layout");

} // namespace vd::pack
