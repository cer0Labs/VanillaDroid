#include "audio/Mixer.h"
#include "audio/SoundAsset.h"
#include "bake/SoundBaker.h"
#include "formats/Wav.h"
#include "pack/SoundFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace vd;

namespace {

void put16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}
void put32(std::vector<u8>& out, u32 v) {
    put16(out, static_cast<u16>(v));
    put16(out, static_cast<u16>(v >> 16));
}

// A PCM WAV as the client writes them, with its blockAlign quirk (stereo files say 2) when asked.
std::vector<u8> pcmWav(const std::vector<i16>& samples, u16 channels, u32 rate, u16 blockAlign) {
    std::vector<u8> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    put32(out, static_cast<u32>(36 + samples.size() * 2));
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(out, 16);
    put16(out, 1);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * 2);
    put16(out, blockAlign);
    put16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    put32(out, static_cast<u32>(samples.size() * 2));
    for (const i16 s : samples) {
        put16(out, static_cast<u16>(s));
    }
    return out;
}

std::vector<i16> tone(u32 frames, u32 channels) {
    std::vector<i16> out(static_cast<usize>(frames) * channels);
    for (u32 i = 0; i < frames; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            out[static_cast<usize>(i) * channels + c] =
                static_cast<i16>(10000.0f * std::sin(0.05f * static_cast<f32>(i) * (1.0f + 0.3f * static_cast<f32>(c))));
        }
    }
    return out;
}

// Plays a baked sound through the mixer at its own rate: every decoded frame (one frame late), until it ends.
std::vector<f32> playAll(const audio::SoundRef& sound) {
    audio::Mixer mixer;
    mixer.init(sound.sampleRate);
    mixer.setOutputGain(1.0f); // the mix itself (phase 14: the output stage apart)
    mixer.setGains(0, 1.0f, 1.0f);
    REQUIRE(mixer.start({sound, 0, 1, audio::Category::Effects, false, 0.0f}));
    std::vector<f32> out;
    std::vector<f32> chunk(2 * 4096);
    while (mixer.finished(0) != 1 && out.size() < 2ull * (sound.frameCount + 8192)) {
        mixer.render(chunk.data(), 4096);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    CHECK(mixer.finished(0) == 1);
    return out;
}

} // namespace

TEST_CASE("WAV: chunks, the client's stereo blockAlign quirk, rejected files") {
    const std::vector<i16> stereo = tone(500, 2);
    const std::vector<u8> file = pcmWav(stereo, 2, 22050, 2); // the info points into it
    Result<formats::WavInfo> wav = formats::parseWav(file);
    REQUIRE(wav);
    CHECK(wav.value().channels == 2);
    CHECK(wav.value().frameCount == 500); // not 1000: the frame size comes from the format
    Result<std::vector<i16>> decoded = formats::decodeWav(wav.value());
    REQUIRE(decoded);
    CHECK(decoded.value() == stereo);

    std::vector<u8> bad = pcmWav(stereo, 2, 22050, 4);
    bad[8] = 'X'; // not WAVE
    CHECK_FALSE(formats::parseWav(bad));
    bad = pcmWav(stereo, 3, 22050, 6);
    CHECK_FALSE(formats::parseWav(bad)); // 3 channels
    bad = pcmWav(stereo, 2, 22050, 4);
    bad.resize(30); // fmt chunk cut short
    CHECK_FALSE(formats::parseWav(bad));
    bad = pcmWav(stereo, 2, 22050, 4);
    bad[20] = 0x55; // another codec
    CHECK_FALSE(formats::parseWav(bad));
}

TEST_CASE("Sound bake: WAV -> IMA ADPCM, played back by the mixer") {
    for (const u16 channels : {u16{1}, u16{2}}) {
        CAPTURE(channels);
        const std::vector<i16> source = tone(5000, channels);
        Result<std::vector<u8>> baked = bake::bakeWavSound(pcmWav(source, channels, 22050, 2));
        REQUIRE(baked);
        audio::SoundRef sound;
        REQUIRE(audio::parseSound(baked.value().data(), baked.value().size(), sound));
        CHECK(sound.codec == pack::SoundCodec::ImaAdpcm);
        CHECK(sound.channels == channels);
        CHECK(sound.sampleRate == 22050);
        CHECK(sound.frameCount == 5000);
        CHECK(sound.blockAlign == bake::kSoundBlockBytesPerChannel * channels);
        // About 4 bits per sample.
        CHECK(sound.size < source.size() * sizeof(i16) / 3);
        const std::vector<f32> out = playAll(sound);
        f64 worst = 0.0;
        for (u32 i = 1; i <= 5000; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                const f64 expected = source[static_cast<usize>(i - 1) * channels + c] / 32768.0;
                const f32 played = out[2 * i + (channels == 2 ? c : 0)];
                worst = std::max(worst, std::abs(played - expected));
            }
        }
        CHECK(worst < 0.03);
    }
    CHECK_FALSE(bake::bakeWavSound(std::vector<u8>{1, 2, 3}));
    CHECK_FALSE(bake::bakeMp3Sound(std::vector<u8>(4096, 0x00))); // no MPEG frame in it
}

TEST_CASE("Sound bake: real client files (VD_WOW_DIR)") {
    VD_REQUIRE_MPQ(mpq);
    // An effect: the frost precast loop.
    Result<std::vector<u8>> wavBytes = mpq->read("Sound\\Spells\\PreCastFrostMagicLow.wav");
    REQUIRE(wavBytes);
    Result<formats::WavInfo> wav = formats::parseWav(wavBytes.value());
    REQUIRE(wav);
    Result<std::vector<u8>> baked = bake::bakeWavSound(wavBytes.value());
    REQUIRE(baked);
    audio::SoundRef sound;
    REQUIRE(audio::parseSound(baked.value().data(), baked.value().size(), sound));
    CHECK(sound.frameCount == wav.value().frameCount);
    CHECK(sound.sampleRate == wav.value().sampleRate);
    CHECK(baked.value().size() < wavBytes.value().size() / 3);

    // Music: Elwynn's first day track, kept as MP3 and decoded by the mixer frame by frame.
    Result<std::vector<u8>> mp3Bytes = mpq->read("Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3");
    REQUIRE(mp3Bytes);
    Result<std::vector<u8>> mp3 = bake::bakeMp3Sound(mp3Bytes.value());
    REQUIRE(mp3);
    audio::SoundRef music;
    REQUIRE(audio::parseSound(mp3.value().data(), mp3.value().size(), music));
    CHECK(music.codec == pack::SoundCodec::Mp3);
    CHECK(music.sampleRate >= 22050);
    CHECK(music.seconds() > 30.0f);
    CHECK(music.size <= mp3Bytes.value().size());
    const std::vector<f32> out = playAll(music);
    // Played to the end: as many frames as the baker counted (+1 for the interpolation, + the last render call).
    usize last = 0;
    f64 energy = 0.0;
    for (usize i = 0; i < out.size() / 2; ++i) {
        if (out[2 * i] != 0.0f || out[2 * i + 1] != 0.0f) {
            last = i;
        }
        energy += static_cast<f64>(out[2 * i]) * out[2 * i];
    }
    CHECK(last <= music.frameCount + 1);
    CHECK(last + music.sampleRate > music.frameCount); // within a second of the end (trailing silence)
    CHECK(energy / static_cast<f64>(out.size() / 2) > 1e-4); // music, not silence
}
