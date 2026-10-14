#include "audio/Adpcm.h"
#include "audio/AudioDevice.h"
#include "audio/AudioSystem.h"
#include "audio/Mixer.h"
#include "audio/SoundAsset.h"
#include "audio/Spatial.h"
#include "core/Hash.h"
#include "pack/SoundFormat.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using namespace vd;
using namespace vd::audio;

namespace {

std::vector<i16> sine(u32 frames, u32 channels, f32 cyclesPerFrame, f32 amplitude = 12000.0f) {
    std::vector<i16> out(static_cast<usize>(frames) * channels);
    for (u32 i = 0; i < frames; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            const f32 phase = 6.2831853f * cyclesPerFrame * static_cast<f32>(i) + (c == 1 ? 1.0f : 0.0f);
            out[static_cast<usize>(i) * channels + c] = static_cast<i16>(amplitude * std::sin(phase));
        }
    }
    return out;
}

// A sound blob (pack/SoundFormat.h) around raw codec data.
std::vector<u8> blob(pack::SoundCodec codec, u32 channels, u32 rate, u32 frames, const void* data, usize size,
                     u16 blockAlign = 0, u16 framesPerBlock = 0) {
    pack::SoundHeader header{};
    header.version = pack::kSoundFormatVersion;
    header.codec = static_cast<u16>(codec);
    header.channels = static_cast<u16>(channels);
    header.sampleRate = rate;
    header.frameCount = frames;
    header.dataOffset = sizeof(header);
    header.dataSize = static_cast<u32>(size);
    header.blockAlign = blockAlign;
    header.framesPerBlock = framesPerBlock;
    std::vector<u8> out(sizeof(header) + size);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), data, size);
    return out;
}

std::vector<u8> pcmBlob(const std::vector<i16>& samples, u32 channels, u32 rate) {
    return blob(pack::SoundCodec::Pcm16, channels, rate, static_cast<u32>(samples.size() / channels), samples.data(),
                samples.size() * sizeof(i16));
}

std::vector<u8> imaBlob(const std::vector<i16>& samples, u32 channels, u32 rate, u32 blockAlign) {
    const u32 frames = static_cast<u32>(samples.size() / channels);
    const u32 perBlock = imaFramesPerBlock(blockAlign, channels);
    const u32 blocks = (frames + perBlock - 1) / perBlock;
    std::vector<u8> data(static_cast<usize>(blocks) * blockAlign);
    u8 index[2] = {};
    for (u32 c = 0; c < channels; ++c) {
        index[c] = bestImaStepIndex(samples.data(), frames, channels, c); // as the baker does
    }
    for (u32 b = 0; b < blocks; ++b) {
        encodeImaBlock(samples.data() + static_cast<usize>(b) * perBlock * channels,
                       std::min(perBlock, frames - b * perBlock), channels, blockAlign, data.data() + b * blockAlign,
                       index);
    }
    return blob(pack::SoundCodec::ImaAdpcm, channels, rate, frames, data.data(), data.size(),
                static_cast<u16>(blockAlign), static_cast<u16>(perBlock));
}

SoundRef ref(const std::vector<u8>& b) {
    SoundRef sound;
    REQUIRE(parseSound(b.data(), b.size(), sound));
    return sound;
}

std::vector<f32> render(Mixer& mixer, u32 frames, u32 chunk = 256) {
    std::vector<f32> out(static_cast<usize>(frames) * 2);
    for (u32 done = 0; done < frames; done += chunk) {
        mixer.render(out.data() + static_cast<usize>(done) * 2, std::min(chunk, frames - done));
    }
    return out;
}

f64 snrDb(const std::vector<i16>& reference, const std::vector<i16>& decoded) {
    f64 signal = 0.0, noise = 0.0;
    for (usize i = 0; i < reference.size(); ++i) {
        signal += static_cast<f64>(reference[i]) * reference[i];
        const f64 d = static_cast<f64>(reference[i]) - decoded[i];
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1.0));
}

} // namespace

TEST_CASE("IMA ADPCM: block layout and round trip") {
    CHECK(imaFramesPerBlock(512, 1) == 1017);
    CHECK(imaFramesPerBlock(1024, 2) == 1017);
    CHECK(imaFramesPerBlock(4, 1) == 0);

    for (const u32 channels : {1u, 2u}) {
        CAPTURE(channels);
        const u32 blockAlign = 512 * channels;
        const u32 perBlock = imaFramesPerBlock(blockAlign, channels);
        const std::vector<i16> source = sine(perBlock * 3 + 100, channels, 440.0f / 22050.0f);
        const std::vector<u8> b = imaBlob(source, channels, 22050, blockAlign);
        const SoundRef sound = ref(b);
        CHECK(sound.frameCount == perBlock * 3 + 100);
        // Decode every block back.
        std::vector<i16> decoded;
        std::vector<i16> block(perBlock * channels);
        for (u32 offset = 0; offset < sound.size; offset += blockAlign) {
            const u32 frames = decodeImaBlock({sound.data + offset, blockAlign}, channels, block.data());
            CHECK(frames == perBlock);
            decoded.insert(decoded.end(), block.begin(), block.begin() + frames * channels);
        }
        decoded.resize(source.size());
        // The first sample of a block is stored exactly; 4-bit ADPCM of a pure tone stays well above 30 dB.
        CHECK(decoded[0] == source[0]);
        CHECK(decoded[static_cast<usize>(perBlock) * channels] == source[static_cast<usize>(perBlock) * channels]);
        CHECK(snrDb(source, decoded) > 30.0);
    }
    // A truncated block decodes nothing.
    i16 out[4];
    const u8 shortBlock[3] = {1, 2, 3};
    CHECK(decodeImaBlock(shortBlock, 1, out) == 0);
}

TEST_CASE("Sound blobs: header validation") {
    const std::vector<i16> samples = sine(100, 1, 0.01f);
    std::vector<u8> good = pcmBlob(samples, 1, 22050);
    SoundRef sound;
    CHECK(parseSound(good.data(), good.size(), sound));
    CHECK(sound.frameCount == 100);
    CHECK(sound.seconds() == doctest::Approx(100.0 / 22050.0));
    // Truncated data, wrong version, bad channel count, nothing.
    CHECK_FALSE(parseSound(good.data(), good.size() - 2, sound));
    CHECK_FALSE(sound.valid());
    std::vector<u8> bad = good;
    bad[0] = 99;
    CHECK_FALSE(parseSound(bad.data(), bad.size(), sound));
    bad = good;
    bad[6] = 3; // channels
    CHECK_FALSE(parseSound(bad.data(), bad.size(), sound));
    CHECK_FALSE(parseSound(nullptr, 0, sound));
    CHECK_FALSE(parseSound(good.data(), 10, sound));
    // ADPCM with a block layout that does not add up.
    std::vector<u8> ima = imaBlob(sine(2000, 1, 0.01f), 1, 22050, 512);
    pack::SoundHeader header;
    std::memcpy(&header, ima.data(), sizeof(header));
    header.framesPerBlock = 1000;
    std::memcpy(ima.data(), &header, sizeof(header));
    CHECK_FALSE(parseSound(ima.data(), ima.size(), sound));
}

TEST_CASE("Sound paths: DirectoryBase + File as the client joins them") {
    CHECK(joinSoundPath("Sound\\Spells", "Frost.wav") == "Sound\\Spells\\Frost.wav");
    CHECK(joinSoundPath("Sound\\Character\\HumanMalePirate\\", "\\Aggro.wav") ==
          "Sound\\Character\\HumanMalePirate\\Aggro.wav");
    CHECK(joinSoundPath("", "x.wav") == "x.wav");
    CHECK(soundPathHash("Sound\\Spells", "Frost.wav") == assetPathHash("sound/spells/frost.wav"));
    CHECK(soundPathHash("Sound\\A\\", "B.wav") == assetPathHash(joinSoundPath("Sound\\A\\", "B.wav")));
    CHECK(soundPathHash("SOUND/a", "b.WAV") == assetPathHash("sound/a/b.wav"));
}

TEST_CASE("Spatial: distance rolloff and panning") {
    CHECK(distanceGain(0.0f, 8.0f, 45.0f) == 1.0f);
    CHECK(distanceGain(8.0f, 8.0f, 45.0f) == 1.0f);
    // Phase 14: the 1.12 rolloff (FMOD 3, SoundRolloffFactor 4): min / (min + 4 (d - min)).
    CHECK(distanceGain(16.0f, 8.0f, 45.0f) == doctest::Approx(0.2f));
    CHECK(distanceGain(10.0f, 8.0f, 45.0f) == doctest::Approx(0.5f));
    CHECK(distanceGain(16.0f, 8.0f, 45.0f, 1.0f) == doctest::Approx(0.5f)); // the 1/d law
    CHECK(distanceGain(45.0f, 8.0f, 45.0f) == 0.0f);
    CHECK(distanceGain(100.0f, 8.0f, 45.0f) == 0.0f);
    // Fades to 0 over the last quarter of the range instead of cutting off.
    const f32 nearCutoff = distanceGain(44.0f, 8.0f, 45.0f);
    CHECK(nearCutoff > 0.0f);
    CHECK(nearCutoff < 8.0f / (8.0f + 4.0f * 36.0f));
    f32 previous = 1.0f;
    for (f32 d = 0.0f; d < 45.0f; d += 0.5f) {
        const f32 g = distanceGain(d, 8.0f, 45.0f);
        CHECK(g <= previous + 1e-6f); // never louder further away
        previous = g;
    }
    const StereoGains center = panGains(0.0f);
    CHECK(center.left == doctest::Approx(1.0f));
    CHECK(center.right == doctest::Approx(1.0f));
    const StereoGains right = panGains(1.0f);
    CHECK(right.left == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(right.right == doctest::Approx(1.0f));
    const StereoGains left = panGains(-1.0f);
    CHECK(left.left == doctest::Approx(1.0f));
    CHECK(left.right == doctest::Approx(0.0f).epsilon(0.001));
    // Bearing: a sound 10 yd to the right is panned fully right; on top of the listener, centered.
    CHECK(panFor(10.0f, 10.0f) == doctest::Approx(1.0f));
    CHECK(panFor(-10.0f, 10.0f) == doctest::Approx(-1.0f));
    CHECK(panFor(0.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(panFor(0.5f, 0.5f) == doctest::Approx(0.25f)); // within 2 yd it moves towards the center
    CHECK(panFor(0.0f, 0.0f) == 0.0f);
}

TEST_CASE("Mixer: a PCM voice at the output rate comes out sample for sample, one frame late") {
    Mixer mixer;
    mixer.init(22050);
    mixer.setOutputGain(1.0f); // the mix itself (phase 14: the output stage apart)
    const std::vector<i16> samples = sine(1000, 1, 0.013f);
    const std::vector<u8> b = pcmBlob(samples, 1, 22050);
    mixer.setGains(3, 1.0f, 0.5f);
    VoiceStart start;
    start.sound = ref(b);
    start.slot = 3;
    start.generation = 7;
    REQUIRE(mixer.start(start));
    CHECK(mixer.finished(3) == Mixer::kNoGeneration);
    const std::vector<f32> out = render(mixer, 1100);
    CHECK(out[0] == 0.0f);
    for (u32 i = 1; i < 1000; ++i) {
        const f32 expected = static_cast<f32>(samples[i - 1]) / 32768.0f;
        REQUIRE(out[2 * i] == doctest::Approx(expected).epsilon(1e-5));
        REQUIRE(out[2 * i + 1] == doctest::Approx(expected * 0.5f).epsilon(1e-5));
    }
    CHECK(out[2 * 1050] == 0.0f);
    CHECK(mixer.finished(3) == 7);
    CHECK(mixer.activeVoices() == 0);
}

TEST_CASE("Mixer: resampling, categories, master, loops, stops with fades") {
    Mixer mixer;
    mixer.init(44100);
    mixer.setOutputGain(1.0f); // the mix itself (phase 14: the output stage apart)
    // A constant (DC) sound makes the gains easy to read: 0.25 full scale.
    const std::vector<i16> dc(2205, 8192);
    const std::vector<u8> b = pcmBlob(dc, 1, 22050);
    const SoundRef sound = ref(b);

    SUBCASE("22 kHz plays twice as long at 44.1 kHz") {
        mixer.setGains(0, 1.0f, 1.0f);
        REQUIRE(mixer.start({sound, 0, 1, Category::Effects, false, 0.0f}));
        const std::vector<f32> out = render(mixer, 5000);
        CHECK(out[2 * 2000] == doctest::Approx(0.25f));
        CHECK(out[2 * 4400] == doctest::Approx(0.25f));
        CHECK(out[2 * 4420] == 0.0f);
        CHECK(mixer.finished(0) == 1);
    }
    SUBCASE("category and master volumes multiply; two voices add") {
        mixer.setGains(0, 1.0f, 1.0f);
        mixer.setGains(1, 0.5f, 0.5f);
        mixer.setCategoryVolume(Category::Music, 0.5f);
        mixer.setMasterVolume(0.8f);
        REQUIRE(mixer.start({sound, 0, 1, Category::Effects, true, 0.0f}));
        REQUIRE(mixer.start({sound, 1, 2, Category::Music, true, 0.0f}));
        const std::vector<f32> out = render(mixer, 2000);
        CHECK(out[2 * 1500] == doctest::Approx(0.25f * 0.8f + 0.25f * 0.5f * 0.5f * 0.8f));
        CHECK(mixer.activeVoices() == 2);
        // Volume changes ramp over one render call instead of jumping.
        mixer.setMasterVolume(0.0f);
        std::vector<f32> ramp(2 * 256);
        mixer.render(ramp.data(), 256);
        CHECK(ramp[0] > 0.2f);
        CHECK(ramp[2 * 128] < ramp[0]);
        CHECK(ramp[2 * 255] < 0.01f);
        mixer.render(ramp.data(), 256);
        CHECK(ramp[2 * 100] == 0.0f);
    }
    SUBCASE("a loop keeps playing until stopped; the stop fades it out") {
        mixer.setGains(5, 1.0f, 1.0f);
        REQUIRE(mixer.start({sound, 5, 9, Category::Ambience, true, 0.0f}));
        std::vector<f32> out = render(mixer, 44100); // 1 s, the sound is 0.1 s
        CHECK(out[2 * 44000] == doctest::Approx(0.25f));
        CHECK(mixer.finished(5) == Mixer::kNoGeneration);
        REQUIRE(mixer.stop(5, 8, 0.0f)); // another generation: ignored
        render(mixer, 256);
        CHECK(mixer.finished(5) == Mixer::kNoGeneration);
        REQUIRE(mixer.stop(5, 9, 0.01f)); // 441 frames of fade
        out = render(mixer, 1000);
        CHECK(out[0] == doctest::Approx(0.25f).epsilon(0.02));
        CHECK(out[2 * 220] == doctest::Approx(0.125f).epsilon(0.05));
        CHECK(out[2 * 600] == 0.0f);
        CHECK(mixer.finished(5) == 9);
    }
    SUBCASE("fade in") {
        mixer.setGains(2, 1.0f, 1.0f);
        REQUIRE(mixer.start({sound, 2, 3, Category::Music, true, 0.1f})); // 4410 frames
        const std::vector<f32> out = render(mixer, 8000);
        CHECK(out[2 * 10] < 0.01f);
        CHECK(out[2 * 2205] == doctest::Approx(0.125f).epsilon(0.02));
        CHECK(out[2 * 6000] == doctest::Approx(0.25f));
    }
    SUBCASE("starting a busy slot replaces its voice and reports the old one finished") {
        mixer.setGains(4, 1.0f, 1.0f);
        REQUIRE(mixer.start({sound, 4, 10, Category::Effects, true, 0.0f}));
        render(mixer, 256);
        REQUIRE(mixer.start({sound, 4, 11, Category::Effects, true, 0.0f}));
        render(mixer, 256);
        CHECK(mixer.finished(4) == 10);
        REQUIRE(mixer.stopAll());
        render(mixer, 16);
        CHECK(mixer.finished(4) == 11);
    }
}

TEST_CASE("Mixer: the output stage lifts the mix 12 dB and its limiter never lets it clip (phase 14)") {
    Mixer mixer;
    mixer.init(48000);
    CHECK(mixer.outputGain() == doctest::Approx(Mixer::kOutputGain));
    // A quiet 0.1 DC sound comes out four times as loud, untouched by the limiter.
    const std::vector<i16> quiet(4800, 3277);
    const std::vector<u8> a = pcmBlob(quiet, 1, 48000);
    mixer.setGains(0, 1.0f, 1.0f);
    REQUIRE(mixer.start({ref(a), 0, 1, Category::Effects, false, 0.0f}));
    std::vector<f32> out = render(mixer, 2400);
    CHECK(out[2 * 100] == doctest::Approx(0.4f).epsilon(0.01));
    CHECK(mixer.takeLimiterGain() == 1.0f);
    // Three loud voices (0.75 each, x4): held at the ceiling, never above, the limiter working ~20 dB.
    const std::vector<i16> loud(48000, 24576);
    const std::vector<u8> b = pcmBlob(loud, 1, 48000);
    for (u32 slot = 1; slot <= 3; ++slot) {
        mixer.setGains(slot, 1.0f, 1.0f);
        REQUIRE(mixer.start({ref(b), slot, 1, Category::Effects, false, 0.0f}));
    }
    out = render(mixer, 24000);
    f32 peak = 0.0f;
    for (const f32 v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak <= Mixer::kLimiterCeiling + 1e-5f);
    CHECK(peak == doctest::Approx(Mixer::kLimiterCeiling).epsilon(0.01));
    CHECK(mixer.takeLimiterGain() < 0.1f);
    // Then quiet again: the limiter lets go within a few times its release.
    mixer.stopAll();
    mixer.setGains(4, 1.0f, 1.0f);
    REQUIRE(mixer.start({ref(a), 4, 2, Category::Effects, true, 0.0f}));
    out = render(mixer, static_cast<u32>(Mixer::kLimiterReleaseSeconds * 48000.0f * 6.0f));
    CHECK(out[out.size() - 2] == doctest::Approx(0.4f).epsilon(0.02));
}

TEST_CASE("Mixer: IMA ADPCM voices decode block after block, also looping") {
    Mixer mixer;
    mixer.init(22050);
    mixer.setOutputGain(1.0f); // the mix itself (phase 14: the output stage apart)
    const std::vector<i16> source = sine(3000, 2, 0.02f);
    const std::vector<u8> b = imaBlob(source, 2, 22050, 1024);
    mixer.setGains(0, 1.0f, 1.0f);
    REQUIRE(mixer.start({ref(b), 0, 1, Category::Effects, true, 0.0f}));
    const std::vector<f32> out = render(mixer, 7000, 333);
    // Frame i of the output is source frame i - 1 (mod 3000 when looping), within ADPCM error.
    f64 error = 0.0;
    for (u32 i = 1; i < 7000; ++i) {
        const u32 f = (i - 1) % 3000;
        for (u32 c = 0; c < 2; ++c) {
            error = std::max(error, std::abs(static_cast<f64>(out[2 * i + c]) - source[f * 2 + c] / 32768.0));
        }
    }
    CHECK(error < 0.03);
    CHECK(mixer.finished(0) == Mixer::kNoGeneration);
}

TEST_CASE("Audio device: the null backend runs the mixer in real time (never the speakers)") {
    AudioOutput output = AudioOutput::Device;
    std::string wav;
    CHECK(parseAudioOutput("null", output, wav));
    CHECK(output == AudioOutput::Null);
    CHECK(parseAudioOutput("wav:mix.wav", output, wav));
    CHECK(output == AudioOutput::Wav);
    CHECK(wav == "mix.wav");
    CHECK(parseAudioOutput("off", output, wav));
    CHECK(output == AudioOutput::Off);
    CHECK_FALSE(parseAudioOutput("speakers", output, wav));

    struct Capture {
        Mixer mixer;
        std::vector<f32> frames;
        std::atomic<u32> count{0};
    } capture;
    capture.frames.resize(48000 * 2);
    AudioDevice device;
    REQUIRE(device.open(
                    AudioOutput::Null, {},
                    [](void* user, f32* out, u32 frames) {
                        auto* c = static_cast<Capture*>(user);
                        c->mixer.render(out, frames);
                        const u32 at = c->count.load();
                        const u32 n = std::min<u32>(frames, static_cast<u32>(c->frames.size() / 2) - at);
                        std::memcpy(c->frames.data() + static_cast<usize>(at) * 2, out, static_cast<usize>(n) * 2 * 4);
                        c->count.store(at + n);
                    },
                    &capture)
                .ok());
    CHECK(std::string(device.backendName()) == "Null");
    CHECK(device.sampleRate() == 48000);
    capture.mixer.init(device.sampleRate());
    capture.mixer.setOutputGain(1.0f); // the mix itself (phase 14: the output stage apart)
    // A 0.25 DC sound for 0.1 s, at the device's rate.
    const std::vector<i16> dc(4800, 8192);
    const std::vector<u8> b = pcmBlob(dc, 1, 48000);
    capture.mixer.setGains(0, 1.0f, 1.0f);
    REQUIRE(capture.mixer.start({ref(b), 0, 1, Category::Effects, false, 0.0f}));
    REQUIRE(device.start());
    for (u32 wait = 0; wait < 200 && capture.count.load() < 9600; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    device.close();
    REQUIRE(capture.count.load() >= 9600);
    CHECK(capture.frames[2 * 1] == doctest::Approx(0.25f));
    CHECK(capture.frames[2 * 4000 + 1] == doctest::Approx(0.25f));
    CHECK(capture.frames[2 * 9000] == 0.0f);
    CHECK(capture.mixer.finished(0) == 1);
    CHECK(device.framesRendered() == 0); // closed
}

TEST_CASE("Audio system: voices, priorities, stealing, culling, pause (null backend)") {
    AudioSystem system;
    AudioConfig config;
    config.output = AudioOutput::Null;
    REQUIRE(system.init(config, std::filesystem::temp_directory_path() / "vd-no-packs").ok()); // no sound pack
    REQUIRE(system.active());
    CHECK_FALSE(system.find(assetPathHash("sound/none.wav")).valid());
    const std::vector<i16> dc(2205, 8192);
    const std::vector<u8> b = pcmBlob(dc, 1, 22050);
    const SoundRef sound = ref(b);

    // Every voice busy with low-priority loops.
    PlayParams low;
    low.loop = true;
    low.priority = 50;
    std::vector<VoiceHandle> voices;
    for (u32 i = 0; i < Mixer::kMaxVoices; ++i) {
        voices.push_back(system.play(sound, low));
        REQUIRE(voices.back());
    }
    system.update();
    CHECK(system.stats().voices == Mixer::kMaxVoices);
    // A lower priority sound finds no room; an equal or higher one takes the oldest of the lowest.
    PlayParams lower = low;
    lower.priority = 10;
    CHECK_FALSE(system.play(sound, lower));
    CHECK(system.stats().dropped == 1);
    PlayParams ui;
    ui.priority = 200;
    const VoiceHandle stolen = system.play(sound, ui);
    REQUIRE(stolen);
    CHECK(stolen.slot == voices[0].slot);
    CHECK(system.stats().stolen == 1);
    CHECK_FALSE(system.playing(voices[0]));
    CHECK(system.playing(stolen));
    // A positional one-shot beyond its cutoff never starts.
    system.setListener(glm::dvec3(0.0), glm::vec3(0.0f, -1.0f, 0.0f));
    PlayParams far;
    far.positional = true;
    far.position = glm::dvec3(100.0, 0.0, 0.0);
    far.priority = 255;
    CHECK_FALSE(system.play(sound, far));
    CHECK(system.stats().culled == 1);
    // Stopping frees the voices once the mixer has faded them out.
    system.stopAll(0.01f);
    for (u32 wait = 0; wait < 200 && system.stats().voices > 0; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        system.update();
    }
    CHECK(system.stats().voices == 0);
    // Paused (the app in the background): nothing starts; back in front, sounds play again.
    system.setPaused(true);
    CHECK_FALSE(system.play(sound, ui));
    system.setPaused(false);
    const VoiceHandle again = system.play(sound, ui);
    CHECK(again);
    for (u32 wait = 0; wait < 200 && system.stats().mixerVoices == 0; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        system.update();
    }
    CHECK(system.stats().mixerVoices >= 1);
    system.shutdown();
    CHECK_FALSE(system.active());
}
