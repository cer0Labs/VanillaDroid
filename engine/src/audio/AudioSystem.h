#pragma once

#include "audio/AudioDevice.h"
#include "audio/Mixer.h"
#include "audio/SoundAsset.h"
#include "audio/Spatial.h"
#include "core/Clock.h"
#include "core/Result.h"
#include "core/SpscQueue.h"
#include "core/Types.h"
#include "pack/PackReader.h"

#include <glm/vec3.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace vd::audio {

struct AudioConfig {
    AudioOutput output = AudioOutput::Off;
    std::string wavPath; // AudioOutput::Wav
    std::string locale;  // phase 21: sound.<locale>.vdpk on top of sound.vdpk (the voices recorded in that language)
};

// A voice started by play(): valid until it ends or is stopped (playing() tells).
struct VoiceHandle {
    u32 slot = 0;
    u32 generation = 0; // 0: no voice
    explicit operator bool() const { return generation != 0; }
    bool operator==(const VoiceHandle&) const = default;
};

struct PlayParams {
    Category category = Category::Effects;
    f32 volume = 1.0f; // SoundEntries' Volume times the caller's
    bool loop = false;
    f32 pitch = 1.0f;
    f32 fadeInSeconds = 0.0f;
    // Voice stealing: when every voice is busy, the lowest priority (then the oldest) one gives way to a sound of
    // at least its priority. Music and ambience 240+, interface 200, the player's own sounds 150, others 100.
    u8 priority = 100;
    // 3D: attenuated with the distance to the listener (SoundEntries' MinDistance / DistanceCutoff) and panned.
    bool positional = false;
    glm::dvec3 position{0.0};
    f32 minDistance = 8.0f;
    f32 maxDistance = 45.0f;
};

struct AudioStats {
    u32 voices = 0;       // busy voices (game thread's view)
    u32 mixerVoices = 0;  // voices the mixer played in its last render
    u32 started = 0;      // since init
    u32 stolen = 0;       // voices cut short to make room
    u32 dropped = 0;      // sounds not started: no voice free for their priority, queue full, paused
    u32 culled = 0;       // one-shots out of hearing range
    u32 missing = 0;      // asked for a sound the pack does not have
    f32 audioLoad = 0.0f; // mixer time / audio time on the audio thread (last second)
    f32 limiterDb = 0.0f; // phase 14: the output limiter's deepest reduction in the last second (dB, <= 0)
    f32 updateMs = 0.0f;  // game thread: the last update()
};

// The game's audio (phase 9): the sound pack, the device, the mixer and the voices, behind a main-thread API that
// never blocks and never allocates once running: play() picks a voice slot (stealing the least important one when
// all are busy) and posts a command; a small IO thread ("vd-audio-io") touches the sound's pages of the mapped pack
// so that the audio thread never waits on flash, keeps reading ahead of the music being streamed, and forwards the
// commands to the mixer. update() (once per frame) frees the voices that ended and recomputes the 3D gains from
// the listener.
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Opens <packsDir>/sound.vdpk and the output. Off (or a failure to open the device) leaves the system inactive:
    // every call is then a cheap no-op. A missing pack is not an error (nothing to play).
    Result<void> init(const AudioConfig& config, const std::filesystem::path& packsDir);
    void shutdown();
    bool active() const { return m_running; }
    const pack::PackReader& pack() const { return m_pack; }
    u32 outputRate() const { return m_mixer.outputRate(); }

    // A baked sound by assetPathHash() of its client path; invalid when the pack lacks it. Once the IO thread has
    // read every sound's header (a moment after init) this only reads that table: the main thread never faults in a
    // page of the pack (a cold header read cost up to 15 ms on the Mi 11X while the world was streaming in).
    SoundRef find(u64 pathHash) const;

    VoiceHandle play(const SoundRef& sound, const PlayParams& params);
    void stop(VoiceHandle voice, f32 fadeOutSeconds = 0.05f);
    void stopAll(f32 fadeOutSeconds = 0.05f);
    bool playing(VoiceHandle voice) const;
    void setVolume(VoiceHandle voice, f32 volume);
    void setPosition(VoiceHandle voice, const glm::dvec3& position);

    // Where the sounds are heard from (the camera): position and its horizontal right vector.
    void setListener(const glm::dvec3& position, const glm::vec3& right);
    void setCategoryVolume(Category category, f32 volume) { m_mixer.setCategoryVolume(category, volume); }
    f32 categoryVolume(Category category) const { return m_mixer.categoryVolume(category); }
    void setMasterVolume(f32 volume);
    f32 outputGain() const { return m_mixer.outputGain(); } // phase 14: the output stage's make-up gain
    // The app went to the background (true) or came back: the device stops and every voice is dropped (what loops
    // should start again on resume: music, ambience).
    void setPaused(bool paused);
    bool paused() const { return m_paused; }
    // Android audio focus: 1 owned, < 1 ducked (another app talks over), 0 lost.
    void setFocusGain(f32 gain);

    // Once per frame on the game thread.
    void update();
    const AudioStats& stats() const { return m_stats; }

private:
    struct Slot {
        u32 generation = 0;
        bool busy = false;
        bool loop = false;
        u8 priority = 0;
        Category category = Category::Effects;
        f32 volume = 1.0f;
        bool positional = false;
        glm::dvec3 position{0.0};
        f32 minDistance = 0.0f;
        f32 maxDistance = 0.0f;
        u64 startedAt = 0;          // play() order, for stealing the oldest
        Clock::time_point expires{}; // given up on (never told it finished) after this; max() for loops
    };
    struct IoCommand {
        enum class Kind : u8 { Start, Stop, Reset } kind = Kind::Start;
        VoiceStart voice;
        f32 fadeSeconds = 0.0f;
    };
    // A big sound being played: the IO thread keeps its next pages in memory.
    struct Stream {
        const u8* data = nullptr;
        u32 size = 0;
        u32 slot = 0;
        u32 generation = 0;
        u32 touchedUntil = 0;
    };

    static void renderCallback(void* user, f32* out, u32 frames);
    void ioThread();
    bool forward(const IoCommand& command); // IO thread -> mixer, retried briefly while the queue is full
    void refreshSlot(u32 slot, const Slot& s);
    bool slotBusy(u32 slot);

    pack::PackReader m_pack;
    AudioDevice m_device;
    Mixer m_mixer;
    bool m_running = false;
    bool m_paused = false;
    f32 m_master = 1.0f;
    f32 m_focus = 1.0f;
    f32 m_rolloff = kRolloffFactor; // phase 14 (audio/Spatial.h)
    std::array<Slot, Mixer::kMaxVoices> m_slots{};
    u32 m_nextGeneration = 1;
    u64 m_playCounter = 0;
    glm::dvec3 m_listener{0.0};
    glm::vec3 m_listenerRight{0.0f, -1.0f, 0.0f};
    AudioStats m_stats;
    Clock::time_point m_loadWindowStart{};

    // IO thread.
    SpscQueue<IoCommand, 256> m_toIo;
    std::thread m_io;
    std::counting_semaphore<1 << 16> m_wake{0};
    std::atomic<bool> m_stopIo{false};
    std::atomic<u32> m_ioDropped{0};
    std::array<Stream, Mixer::kMaxVoices> m_streams{}; // IO thread only
    // Every sound's header, sorted by path hash: written by the IO thread once, read by find() after the flag.
    struct CachedSound {
        u64 hash = 0;
        SoundRef sound;
    };
    std::vector<CachedSound> m_headers;
    std::atomic<bool> m_headersReady{false};
};

} // namespace vd::audio
