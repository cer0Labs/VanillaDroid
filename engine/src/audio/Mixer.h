#pragma once

#include "audio/SoundAsset.h"
#include "core/SpscQueue.h"
#include "core/Types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

namespace vd::audio {

// What a sound is, for its volume setting (the settings panel's sliders).
enum class Category : u8 {
    Music,
    Ambience,
    Effects,   // spells, combat, footsteps, objects
    Interface, // window and button sounds
    Voices,    // NPC greetings and creature cries
    Count
};
constexpr u32 kCategoryCount = static_cast<u32>(Category::Count);

// A voice to start in a slot (the caller owns the slots: the audio system picks them and steals the least
// important one when all are busy). Its gains are the slot's (setGains), set before starting it.
struct VoiceStart {
    SoundRef sound;
    u32 slot = 0;
    u32 generation = 0; // reported back by finished() once this voice ends
    Category category = Category::Effects;
    bool loop = false;
    f32 fadeInSeconds = 0.0f;
};

// Software mixer: up to kMaxVoices voices decoded while they play (PCM16, IMA ADPCM, MP3 frame by frame), resampled
// linearly to the output rate, with gains per voice (left/right: the 3D panning), per category and master, ramped
// over each render call so that changes never click. Renders interleaved stereo f32.
//
// Threads: render() runs on the audio thread (miniaudio's callback); start()/stop() come from one producer thread
// through a lock-free queue; the gains, pitches and volumes are atomics any thread may write; finished() and the
// counters are read by the game thread. Nothing allocates after construction (the MP3 decoders are a fixed pool).
class Mixer {
public:
    static constexpr u32 kMaxVoices = 48;
    static constexpr u32 kMaxStreams = 4; // voices decoding MP3 at the same time (music, its cross-fade, intros)
    static constexpr u32 kChannels = 2;
    static constexpr u32 kNoGeneration = 0; // generations start at 1

    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    void init(u32 outputRate);
    u32 outputRate() const { return m_outputRate; }

    // --- Commands (one producer thread). False when the queue is full.
    bool start(const VoiceStart& voice);
    // Fades the voice out (0: at once) and frees its slot; ignored once the slot plays another generation.
    bool stop(u32 slot, u32 generation, f32 fadeOutSeconds);
    // Every voice ends at once (the app was paused: what played before is dropped).
    bool stopAll();

    // --- Parameters (any thread), picked up by the next render call and ramped over it.
    void setGains(u32 slot, f32 left, f32 right);
    void setPitch(u32 slot, f32 pitch);
    void setCategoryVolume(Category category, f32 volume);
    f32 categoryVolume(Category category) const;
    void setMasterVolume(f32 volume);
    f32 masterVolume() const { return m_master.load(std::memory_order_relaxed); }
    // Phase 14: the output stage after the master volume: a make-up gain, then a peak limiter (instant attack,
    // kLimiterReleaseSeconds) that keeps every sample within kLimiterCeiling. The 1.12 mix (its volumes and FMOD's
    // rolloff, unchanged) peaks around -11 dBFS with its music near -27 dBFS RMS: right for a PC's amplified speakers,
    // low on a phone's; kOutputGain lifts it to a phone's loudness without clipping.
    static constexpr f32 kOutputGain = 4.0f; // +12 dB
    static constexpr f32 kLimiterCeiling = 0.89f; // -1 dBFS
    static constexpr f32 kLimiterReleaseSeconds = 0.2f;
    void setOutputGain(f32 gain) { m_outputGain.store(std::max(0.0f, gain), std::memory_order_relaxed); }
    f32 outputGain() const { return m_outputGain.load(std::memory_order_relaxed); }
    // The limiter's deepest gain reduction since the last call (1: none), for the statistics.
    f32 takeLimiterGain() { return m_limiterMin.exchange(1.0f, std::memory_order_relaxed); }

    // Generation of the last voice that ended in `slot` (kNoGeneration if none yet).
    u32 finished(u32 slot) const { return m_finished[slot].load(std::memory_order_acquire); }
    // Bytes of its data the voice in `slot` has decoded so far (the streaming prefetch reads ahead of it).
    u32 bytesRead(u32 slot) const { return m_bytesRead[slot].load(std::memory_order_relaxed); }
    u32 activeVoices() const { return m_active.load(std::memory_order_relaxed); }
    // Time spent in render() and the frames it produced since the last call (audio thread cost).
    void takeRenderTime(u64& nanos, u64& frames);

    // --- Audio thread: `frames` interleaved stereo frames into `out` (overwritten).
    void render(f32* out, u32 frames);

private:
    struct Voice;
    struct Stream;
    struct Command {
        enum class Kind : u8 { Start, Stop, StopAll } kind = Kind::Start;
        VoiceStart voice;
        f32 fadeSeconds = 0.0f;
    };

    void apply(const Command& command);
    void finish(u32 slot);
    bool refill(Voice& voice);
    void rewind(Voice& voice);
    // False once the voice has ended (its slot is free again).
    bool mix(u32 slot, f32* out, u32 frames, f32 targetLeft, f32 targetRight, f32 step);

    u32 m_outputRate = 48000;
    std::unique_ptr<Voice[]> m_voices;
    std::unique_ptr<Stream[]> m_streams;
    std::array<bool, kMaxStreams> m_streamUsed{};
    SpscQueue<Command, 256> m_commands;
    std::array<std::atomic<u32>, kMaxVoices> m_finished{};
    std::array<std::atomic<u32>, kMaxVoices> m_bytesRead{};
    std::array<std::atomic<f32>, kMaxVoices> m_gainLeft{};
    std::array<std::atomic<f32>, kMaxVoices> m_gainRight{};
    std::array<std::atomic<f32>, kMaxVoices> m_pitch{};
    std::array<std::atomic<f32>, kCategoryCount> m_categoryVolume{};
    std::atomic<f32> m_master{1.0f};
    std::atomic<f32> m_outputGain{kOutputGain}; // phase 14
    std::atomic<f32> m_limiterMin{1.0f};
    f32 m_envelope = 0.0f; // audio thread: the limiter's peak envelope
    f32 m_release = 0.0f;  // its decay per frame
    std::atomic<u32> m_active{0};
    std::atomic<u64> m_renderNanos{0};
    std::atomic<u64> m_renderFrames{0};
};

} // namespace vd::audio
