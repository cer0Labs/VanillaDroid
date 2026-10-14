#include "audio/AudioSystem.h"

#include "audio/Spatial.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace vd::audio {
namespace {

constexpr u32 kPageBytes = 4096;
// Sounds up to this size are read whole before they start; bigger ones (music, long ambience loops) only their
// beginning, and then the IO thread stays kStreamAhead bytes in front of the mixer.
constexpr u32 kWholeBytes = 512u << 10;
constexpr u32 kStreamStart = 128u << 10;
constexpr u32 kStreamAhead = 256u << 10;
constexpr auto kIoPeriod = std::chrono::milliseconds(50);
// A voice the mixer never reported back is given up this long after it should have ended.
constexpr f64 kExpirySlackSeconds = 2.0;

// Reads one byte per page so that the pages are in memory (page faults happen here, not on the audio thread).
u32 touchPages(const u8* data, u32 begin, u32 end) {
    u32 sum = 0;
    for (u32 offset = begin; offset < end; offset += kPageBytes) {
        sum += static_cast<const volatile u8*>(data)[offset];
    }
    if (end > begin) {
        sum += static_cast<const volatile u8*>(data)[end - 1];
    }
    return sum;
}

} // namespace

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    shutdown();
}

Result<void> AudioSystem::init(const AudioConfig& config, const std::filesystem::path& packsDir) {
    shutdown();
    if (config.output == AudioOutput::Off) {
        VD_LOG_INFO("Audio: off");
        return {};
    }
    const std::filesystem::path packPath = packsDir / "sound.vdpk";
    if (Result<void> opened = m_pack.open(packPath); !opened) {
        VD_LOG_WARN("Audio: no sound pack (vd_baker bake --only sound): %s", opened.error().message.c_str());
    } else {
        VD_LOG_INFO("Pack sound.vdpk: %u sounds", m_pack.entryCount());
        if (m_pack.locale() != config.locale) {
            if (Result<void> overlay = m_pack.openOverlay(config.locale); !overlay) {
                VD_LOG_WARN("Audio: language overlay not loaded: %s", overlay.error().message.c_str());
            } else if (m_pack.hasOverlay()) {
                VD_LOG_INFO("Pack sound.vdpk: %u sounds in %s on top", m_pack.overlay()->entryCount(),
                            config.locale.c_str());
            }
        }
    }
    if (Result<void> opened = m_device.open(config.output, config.wavPath, &AudioSystem::renderCallback, this);
        !opened) {
        m_pack.close();
        return opened.error();
    }
    m_mixer.init(m_device.sampleRate());
    // Phase 14: VD_ROLLOFF=<factor> replaces the 1.12 rolloff (4) to compare (1: the 1/d law of phase 9);
    // VD_OUTPUT_GAIN=<factor> the output stage's make-up gain (1: the mix as it was).
    if (const char* rolloff = VD_DEV_ENV("VD_ROLLOFF")) {
        m_rolloff = std::clamp(static_cast<f32>(std::atof(rolloff)), 0.1f, 16.0f);
    }
    if (const char* gain = VD_DEV_ENV("VD_OUTPUT_GAIN")) {
        m_mixer.setOutputGain(std::clamp(static_cast<f32>(std::atof(gain)), 0.0f, 16.0f));
    }
    m_stopIo = false;
    m_io = std::thread([this] { ioThread(); });
    if (!m_device.start()) {
        VD_LOG_WARN("Audio: the device did not start");
    }
    m_running = true;
    m_loadWindowStart = Clock::now();
    return {};
}

void AudioSystem::shutdown() {
    if (m_io.joinable()) {
        m_stopIo = true;
        m_wake.release();
        m_io.join();
    }
    m_device.close();
    m_pack.close();
    m_running = false;
    m_slots = {};
    m_streams = {};
    IoCommand pending;
    while (m_toIo.pop(pending)) {
    }
}

void AudioSystem::renderCallback(void* user, f32* out, u32 frames) {
    static_cast<AudioSystem*>(user)->m_mixer.render(out, frames);
}

SoundRef AudioSystem::find(u64 pathHash) const {
    if (m_headersReady.load(std::memory_order_acquire)) {
        const auto it = std::lower_bound(m_headers.begin(), m_headers.end(), pathHash,
                                         [](const CachedSound& c, u64 hash) { return c.hash < hash; });
        return it != m_headers.end() && it->hash == pathHash ? it->sound : SoundRef{};
    }
    SoundRef sound;
    if (const std::optional<pack::PackAsset> asset = m_pack.find(pathHash);
        asset && asset->type == pack::AssetType::Sound && asset->compression == pack::Compression::None) {
        parseSound(asset->data, asset->size, sound);
    }
    return sound;
}

bool AudioSystem::slotBusy(u32 slot) {
    Slot& s = m_slots[slot];
    if (!s.busy) {
        return false;
    }
    if (m_mixer.finished(slot) == s.generation || Clock::now() > s.expires) {
        s.busy = false;
    }
    return s.busy;
}

void AudioSystem::refreshSlot(u32 slot, const Slot& s) {
    if (!s.positional) {
        m_mixer.setGains(slot, s.volume, s.volume);
        return;
    }
    const glm::dvec3 offset = s.position - m_listener;
    const f32 distance = static_cast<f32>(glm::length(offset));
    const f32 gain = s.volume * distanceGain(distance, s.minDistance, s.maxDistance, m_rolloff);
    const f32 side = glm::dot(glm::vec3(offset), m_listenerRight);
    const StereoGains pan = panGains(panFor(side, distance));
    m_mixer.setGains(slot, gain * pan.left, gain * pan.right);
}

VoiceHandle AudioSystem::play(const SoundRef& sound, const PlayParams& params) {
    if (!m_running || !sound.valid()) {
        if (m_running) {
            ++m_stats.missing;
        }
        return {};
    }
    if (m_paused) {
        ++m_stats.dropped;
        return {};
    }
    if (params.positional && !params.loop &&
        glm::length(params.position - m_listener) >= static_cast<f64>(params.maxDistance)) {
        ++m_stats.culled; // a one-shot nobody would hear
        return {};
    }
    // A free slot, else the least important busy one (lowest priority, then the oldest) if this sound matters as
    // much.
    i32 chosen = -1;
    i32 victim = -1;
    for (u32 i = 0; i < Mixer::kMaxVoices; ++i) {
        if (!slotBusy(i)) {
            chosen = static_cast<i32>(i);
            break;
        }
        const Slot& s = m_slots[i];
        if (s.priority <= params.priority &&
            (victim < 0 || s.priority < m_slots[victim].priority ||
             (s.priority == m_slots[victim].priority && s.startedAt < m_slots[victim].startedAt))) {
            victim = static_cast<i32>(i);
        }
    }
    if (chosen < 0) {
        if (victim < 0) {
            ++m_stats.dropped;
            return {};
        }
        chosen = victim;
        ++m_stats.stolen;
    }
    const u32 slot = static_cast<u32>(chosen);
    Slot& s = m_slots[slot];
    s.generation = m_nextGeneration++;
    if (m_nextGeneration == 0) {
        m_nextGeneration = 1;
    }
    s.busy = true;
    s.loop = params.loop;
    s.priority = params.priority;
    s.category = params.category;
    s.volume = std::max(0.0f, params.volume);
    s.positional = params.positional;
    s.position = params.position;
    s.minDistance = params.minDistance;
    s.maxDistance = params.maxDistance;
    s.startedAt = ++m_playCounter;
    const f32 pitch = std::clamp(params.pitch, 0.25f, 4.0f);
    s.expires = params.loop ? Clock::time_point::max()
                            : Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(
                                                 sound.seconds() / pitch + params.fadeInSeconds + kExpirySlackSeconds));
    refreshSlot(slot, s);
    m_mixer.setPitch(slot, pitch);

    IoCommand command;
    command.kind = IoCommand::Kind::Start;
    command.voice.sound = sound;
    command.voice.slot = slot;
    command.voice.generation = s.generation;
    command.voice.category = params.category;
    command.voice.loop = params.loop;
    command.voice.fadeInSeconds = params.fadeInSeconds;
    if (!m_toIo.push(command)) {
        s.busy = false;
        ++m_stats.dropped;
        return {};
    }
    m_wake.release();
    ++m_stats.started;
    return {slot, s.generation};
}

void AudioSystem::stop(VoiceHandle voice, f32 fadeOutSeconds) {
    if (!voice || voice.slot >= Mixer::kMaxVoices) {
        return;
    }
    Slot& s = m_slots[voice.slot];
    if (!s.busy || s.generation != voice.generation) {
        return;
    }
    IoCommand command;
    command.kind = IoCommand::Kind::Stop;
    command.voice.slot = voice.slot;
    command.voice.generation = voice.generation;
    command.fadeSeconds = std::max(0.0f, fadeOutSeconds);
    if (m_toIo.push(command)) {
        m_wake.release();
        const Clock::time_point giveUp = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                            std::chrono::duration<f64>(fadeOutSeconds + 1.0));
        s.expires = std::min(s.expires, giveUp);
    }
}

void AudioSystem::stopAll(f32 fadeOutSeconds) {
    for (u32 i = 0; i < Mixer::kMaxVoices; ++i) {
        if (m_slots[i].busy) {
            stop({i, m_slots[i].generation}, fadeOutSeconds);
        }
    }
}

bool AudioSystem::playing(VoiceHandle voice) const {
    if (!voice || voice.slot >= Mixer::kMaxVoices) {
        return false;
    }
    const Slot& s = m_slots[voice.slot];
    return s.busy && s.generation == voice.generation && m_mixer.finished(voice.slot) != voice.generation &&
           Clock::now() <= s.expires;
}

void AudioSystem::setVolume(VoiceHandle voice, f32 volume) {
    if (voice && voice.slot < Mixer::kMaxVoices && m_slots[voice.slot].generation == voice.generation) {
        m_slots[voice.slot].volume = std::max(0.0f, volume);
        refreshSlot(voice.slot, m_slots[voice.slot]);
    }
}

void AudioSystem::setPosition(VoiceHandle voice, const glm::dvec3& position) {
    if (voice && voice.slot < Mixer::kMaxVoices && m_slots[voice.slot].generation == voice.generation) {
        m_slots[voice.slot].position = position;
    }
}

void AudioSystem::setListener(const glm::dvec3& position, const glm::vec3& right) {
    m_listener = position;
    const f32 length = glm::length(right);
    if (length > 1e-4f) {
        m_listenerRight = right / length;
    }
}

void AudioSystem::setMasterVolume(f32 volume) {
    m_master = std::clamp(volume, 0.0f, 1.0f);
    m_mixer.setMasterVolume(m_master * m_focus);
}

void AudioSystem::setFocusGain(f32 gain) {
    m_focus = std::clamp(gain, 0.0f, 1.0f);
    m_mixer.setMasterVolume(m_master * m_focus);
}

void AudioSystem::setPaused(bool paused) {
    if (!m_running || paused == m_paused) {
        return;
    }
    m_paused = paused;
    if (paused) {
        // Every voice is forgotten here; the mixer drops them when it resumes (Reset is ahead of anything new).
        IoCommand reset;
        reset.kind = IoCommand::Kind::Reset;
        if (m_toIo.push(reset)) {
            m_wake.release();
        }
        for (Slot& s : m_slots) {
            s.busy = false;
        }
        m_device.stop();
        VD_LOG_INFO("Audio: paused");
    } else {
        m_device.start();
        VD_LOG_INFO("Audio: resumed");
    }
}

void AudioSystem::update() {
    if (!m_running) {
        return;
    }
    VD_PROFILE_ZONE("AudioSystem::update");
    const Clock::time_point begin = Clock::now();
    u32 busy = 0;
    for (u32 i = 0; i < Mixer::kMaxVoices; ++i) {
        if (!slotBusy(i)) {
            continue;
        }
        ++busy;
        if (m_slots[i].positional) {
            refreshSlot(i, m_slots[i]);
        }
    }
    m_stats.voices = busy;
    m_stats.mixerVoices = m_mixer.activeVoices();
    m_stats.dropped += m_ioDropped.exchange(0, std::memory_order_relaxed);
    if (secondsBetween(m_loadWindowStart, begin) >= 1.0) {
        u64 nanos = 0, frames = 0;
        m_mixer.takeRenderTime(nanos, frames);
        m_stats.audioLoad = frames > 0 ? static_cast<f32>(static_cast<f64>(nanos) * 1e-9 /
                                                          (static_cast<f64>(frames) / m_mixer.outputRate()))
                                       : 0.0f;
        m_stats.limiterDb = 20.0f * std::log10(std::max(m_mixer.takeLimiterGain(), 1e-6f));
        m_loadWindowStart = begin;
    }
    m_stats.updateMs = static_cast<f32>(secondsBetween(begin, Clock::now()) * 1000.0);
}

bool AudioSystem::forward(const IoCommand& command) {
    for (u32 attempt = 0; attempt < 50; ++attempt) {
        bool pushed = false;
        switch (command.kind) {
        case IoCommand::Kind::Start: pushed = m_mixer.start(command.voice); break;
        case IoCommand::Kind::Stop:
            pushed = m_mixer.stop(command.voice.slot, command.voice.generation, command.fadeSeconds);
            break;
        case IoCommand::Kind::Reset: pushed = m_mixer.stopAll(); break;
        }
        if (pushed) {
            return true;
        }
        if (m_stopIo.load(std::memory_order_relaxed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // the mixer drains the queue every period
    }
    m_ioDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void AudioSystem::ioThread() {
    setCurrentThreadName("vd-audio-io");
    VD_PROFILE_THREAD_NAME("vd-audio-io");
    u32 sink = 0;
    {
        // Every sound's header, read here once (one page each, ~7,000 of them, in the background): find() on the
        // main thread then never touches the mapped pack, whose cold pages cost milliseconds each to fault in from
        // flash while the world streams (music starts on entering the world: 5-15 ms frames on the Mi 11X).
        VD_PROFILE_ZONE("AudioIo::readHeaders");
        m_headers.reserve(m_pack.entryCount());
        for (u32 i = 0; i < m_pack.entryCount() && !m_stopIo.load(std::memory_order_relaxed); ++i) {
            const pack::PackAsset asset = m_pack.entry(i);
            CachedSound cached;
            if (asset.type == pack::AssetType::Sound && asset.compression == pack::Compression::None &&
                parseSound(asset.data, asset.size, cached.sound)) {
                cached.hash = m_pack.entryHash(i);
                m_headers.push_back(cached); // in the pack's order: sorted by hash
            }
        }
        m_headersReady.store(true, std::memory_order_release);
    }
    while (!m_stopIo.load(std::memory_order_relaxed)) {
        static_cast<void>(m_wake.try_acquire_for(kIoPeriod));
        IoCommand command;
        while (!m_stopIo.load(std::memory_order_relaxed) && m_toIo.pop(command)) {
            if (command.kind == IoCommand::Kind::Reset) {
                m_streams = {}; // paused: the mixer drops every voice when the device runs again
            }
            if (command.kind == IoCommand::Kind::Start) {
                VD_PROFILE_ZONE("AudioIo::prefetch");
                const SoundRef& sound = command.voice.sound;
                const u32 whole = sound.size <= kWholeBytes ? sound.size : kStreamStart;
                sink += touchPages(sound.data, 0, whole);
                Stream& stream = m_streams[command.voice.slot];
                stream = {};
                if (whole < sound.size) {
                    stream = {sound.data, sound.size, command.voice.slot, command.voice.generation, whole};
                }
            }
            forward(command);
        }
        // Read ahead of the streamed sounds.
        for (Stream& stream : m_streams) {
            if (!stream.data) {
                continue;
            }
            if (m_mixer.finished(stream.slot) == stream.generation) {
                stream = {};
                continue;
            }
            const u32 target = std::min(stream.size, std::max(stream.touchedUntil, m_mixer.bytesRead(stream.slot)) +
                                                         kStreamAhead);
            if (target > stream.touchedUntil) {
                VD_PROFILE_ZONE("AudioIo::readAhead");
                sink += touchPages(stream.data, stream.touchedUntil, target);
                stream.touchedUntil = target;
            }
        }
    }
    static_cast<void>(sink);
}

} // namespace vd::audio
