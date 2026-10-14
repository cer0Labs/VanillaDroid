#include "audio/Mixer.h"

#include "audio/Adpcm.h"
#include "audio/Mp3Frame.h"
#include "core/Clock.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace vd::audio {

// An MP3 decoder's state (Mp3Frame.h: its size is only known at run time).
struct Mixer::Stream {
    std::unique_ptr<std::max_align_t[]> state;
};

struct Mixer::Voice {
    bool active = false;
    bool loop = false;
    bool ended = false; // the source ran out: the interpolation tail is still played
    Category category = Category::Effects;
    u32 generation = 0;
    SoundRef sound;
    u32 nextFrame = 0; // Pcm16 / ImaAdpcm: next source frame to decode
    u32 mp3Offset = 0; // Mp3: next byte of the data
    i32 stream = -1;   // Mp3: its decoder
    u32 bufferFrames = 0;
    u32 bufferPos = 0;
    f32 frac = 1.0f; // position between prev and cur (>= 1: fetch the next source frame)
    f32 prev[2] = {};
    f32 cur[2] = {};
    f32 gainLeft = 0.0f; // effective gains reached at the end of the last render call
    f32 gainRight = 0.0f;
    f32 fade = 1.0f; // fade in / out multiplier and its change per output frame
    f32 fadeStep = 0.0f;
    i16 buffer[kMaxBlockFrames * 2] = {};
};

Mixer::Mixer() : m_voices(std::make_unique<Voice[]>(kMaxVoices)), m_streams(std::make_unique<Stream[]>(kMaxStreams)) {
    const usize words = (vdMp3DecoderSize() + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    for (u32 s = 0; s < kMaxStreams; ++s) {
        m_streams[s].state = std::make_unique<std::max_align_t[]>(words);
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        m_pitch[i].store(1.0f, std::memory_order_relaxed);
    }
    for (u32 c = 0; c < kCategoryCount; ++c) {
        m_categoryVolume[c].store(1.0f, std::memory_order_relaxed);
    }
}

Mixer::~Mixer() = default;

void Mixer::init(u32 outputRate) {
    m_outputRate = std::max(8000u, outputRate);
    m_release = std::exp(-1.0f / (kLimiterReleaseSeconds * static_cast<f32>(m_outputRate)));
    m_envelope = 0.0f;
}

bool Mixer::start(const VoiceStart& voice) {
    if (voice.slot >= kMaxVoices || !voice.sound.valid()) {
        return false;
    }
    Command command;
    command.kind = Command::Kind::Start;
    command.voice = voice;
    return m_commands.push(command);
}

bool Mixer::stop(u32 slot, u32 generation, f32 fadeOutSeconds) {
    if (slot >= kMaxVoices) {
        return false;
    }
    Command command;
    command.kind = Command::Kind::Stop;
    command.voice.slot = slot;
    command.voice.generation = generation;
    command.fadeSeconds = fadeOutSeconds;
    return m_commands.push(command);
}

bool Mixer::stopAll() {
    Command command;
    command.kind = Command::Kind::StopAll;
    return m_commands.push(command);
}

void Mixer::setGains(u32 slot, f32 left, f32 right) {
    if (slot < kMaxVoices) {
        m_gainLeft[slot].store(left, std::memory_order_relaxed);
        m_gainRight[slot].store(right, std::memory_order_relaxed);
    }
}

void Mixer::setPitch(u32 slot, f32 pitch) {
    if (slot < kMaxVoices) {
        m_pitch[slot].store(std::clamp(pitch, 0.25f, 4.0f), std::memory_order_relaxed);
    }
}

void Mixer::setCategoryVolume(Category category, f32 volume) {
    m_categoryVolume[static_cast<u32>(category)].store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

f32 Mixer::categoryVolume(Category category) const {
    return m_categoryVolume[static_cast<u32>(category)].load(std::memory_order_relaxed);
}

void Mixer::setMasterVolume(f32 volume) {
    m_master.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Mixer::takeRenderTime(u64& nanos, u64& frames) {
    nanos = m_renderNanos.exchange(0, std::memory_order_relaxed);
    frames = m_renderFrames.exchange(0, std::memory_order_relaxed);
}

void Mixer::finish(u32 slot) {
    Voice& voice = m_voices[slot];
    if (voice.stream >= 0) {
        m_streamUsed[static_cast<u32>(voice.stream)] = false;
        voice.stream = -1;
    }
    voice.active = false;
    m_finished[slot].store(voice.generation, std::memory_order_release);
}

void Mixer::apply(const Command& command) {
    if (command.kind == Command::Kind::StopAll) {
        for (u32 slot = 0; slot < kMaxVoices; ++slot) {
            if (m_voices[slot].active) {
                finish(slot);
            }
        }
        return;
    }
    const u32 slot = command.voice.slot;
    Voice& voice = m_voices[slot];
    if (command.kind == Command::Kind::Stop) {
        if (!voice.active || voice.generation != command.voice.generation) {
            return;
        }
        if (command.fadeSeconds <= 0.0f) {
            finish(slot);
        } else {
            voice.fadeStep = -std::max(voice.fade, 1e-3f) / (command.fadeSeconds * static_cast<f32>(m_outputRate));
        }
        return;
    }
    if (voice.active) {
        finish(slot); // stolen for a new sound
    }
    const VoiceStart& start = command.voice;
    voice.generation = start.generation;
    voice.sound = start.sound;
    voice.category = start.category;
    voice.loop = start.loop;
    voice.ended = false;
    voice.nextFrame = 0;
    voice.mp3Offset = 0;
    voice.bufferFrames = 0;
    voice.bufferPos = 0;
    voice.frac = 1.0f;
    voice.prev[0] = voice.prev[1] = voice.cur[0] = voice.cur[1] = 0.0f;
    voice.stream = -1;
    if (voice.sound.codec == pack::SoundCodec::Mp3) {
        for (u32 s = 0; s < kMaxStreams; ++s) {
            if (!m_streamUsed[s]) {
                m_streamUsed[s] = true;
                voice.stream = static_cast<i32>(s);
                vdMp3DecoderInit(m_streams[s].state.get());
                break;
            }
        }
        if (voice.stream < 0) {
            m_finished[slot].store(voice.generation, std::memory_order_release); // no decoder free: never starts
            return;
        }
    }
    const f32 category = m_categoryVolume[static_cast<u32>(voice.category)].load(std::memory_order_relaxed);
    const f32 master = m_master.load(std::memory_order_relaxed);
    voice.gainLeft = m_gainLeft[slot].load(std::memory_order_relaxed) * category * master;
    voice.gainRight = m_gainRight[slot].load(std::memory_order_relaxed) * category * master;
    if (start.fadeInSeconds > 0.0f) {
        voice.fade = 0.0f;
        voice.fadeStep = 1.0f / (start.fadeInSeconds * static_cast<f32>(m_outputRate));
    } else {
        voice.fade = 1.0f;
        voice.fadeStep = 0.0f;
    }
    m_bytesRead[slot].store(0, std::memory_order_relaxed);
    voice.active = true;
}

void Mixer::rewind(Voice& voice) {
    voice.nextFrame = 0;
    voice.mp3Offset = 0;
    if (voice.stream >= 0) {
        vdMp3DecoderInit(m_streams[static_cast<u32>(voice.stream)].state.get());
    }
}

bool Mixer::refill(Voice& voice) {
    const SoundRef& sound = voice.sound;
    const u32 channels = sound.channels;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        switch (sound.codec) {
        case pack::SoundCodec::Pcm16:
            if (voice.nextFrame < sound.frameCount) {
                const u32 frames = std::min(kMaxBlockFrames, sound.frameCount - voice.nextFrame);
                std::memcpy(voice.buffer, sound.data + static_cast<usize>(voice.nextFrame) * channels * 2,
                            static_cast<usize>(frames) * channels * 2);
                voice.nextFrame += frames;
                voice.bufferFrames = frames;
                voice.bufferPos = 0;
                return true;
            }
            break;
        case pack::SoundCodec::ImaAdpcm:
            if (voice.nextFrame < sound.frameCount) {
                const usize offset = static_cast<usize>(voice.nextFrame / sound.framesPerBlock) * sound.blockAlign;
                if (offset + sound.blockAlign > sound.size) {
                    break;
                }
                const u32 frames = std::min(
                    decodeImaBlock(std::span<const u8>(sound.data + offset, sound.blockAlign), channels, voice.buffer),
                    sound.frameCount - voice.nextFrame);
                if (frames == 0) {
                    break;
                }
                voice.nextFrame += frames;
                voice.bufferFrames = frames;
                voice.bufferPos = 0;
                return true;
            }
            break;
        case pack::SoundCodec::Mp3: {
            void* decoder = m_streams[static_cast<u32>(voice.stream)].state.get();
            while (voice.mp3Offset < sound.size) {
                int consumed = 0, frameChannels = 0, frameRate = 0;
                const int samples = vdMp3DecodeFrame(decoder, sound.data + voice.mp3Offset,
                                                     static_cast<int>(sound.size - voice.mp3Offset), voice.buffer,
                                                     &consumed, &frameChannels, &frameRate);
                if (consumed <= 0) {
                    voice.mp3Offset = sound.size;
                    break;
                }
                voice.mp3Offset += static_cast<u32>(consumed);
                if (samples > 0 && static_cast<u32>(frameChannels) == channels) {
                    voice.bufferFrames = static_cast<u32>(samples);
                    voice.bufferPos = 0;
                    return true;
                }
            }
            break;
        }
        }
        if (!voice.loop) {
            return false;
        }
        rewind(voice);
    }
    return false;
}

bool Mixer::mix(u32 slot, f32* out, u32 frames, f32 targetLeft, f32 targetRight, f32 step) {
    Voice& voice = m_voices[slot];
    const f32 inverseFrames = 1.0f / static_cast<f32>(frames);
    const f32 deltaLeft = (targetLeft - voice.gainLeft) * inverseFrames;
    const f32 deltaRight = (targetRight - voice.gainRight) * inverseFrames;
    f32 gainLeft = voice.gainLeft;
    f32 gainRight = voice.gainRight;
    const u32 channels = voice.sound.channels;
    constexpr f32 kScale = 1.0f / 32768.0f;
    for (u32 i = 0; i < frames; ++i) {
        while (voice.frac >= 1.0f) {
            voice.prev[0] = voice.cur[0];
            voice.prev[1] = voice.cur[1];
            if (voice.ended) {
                finish(slot);
                return false;
            }
            if (voice.bufferPos >= voice.bufferFrames && !refill(voice)) {
                voice.ended = true;
                voice.cur[0] = voice.cur[1] = 0.0f;
            } else {
                const i16* frame = voice.buffer + static_cast<usize>(voice.bufferPos++) * channels;
                voice.cur[0] = static_cast<f32>(frame[0]) * kScale;
                voice.cur[1] = channels == 2 ? static_cast<f32>(frame[1]) * kScale : voice.cur[0];
            }
            voice.frac -= 1.0f;
        }
        const f32 left = voice.prev[0] + (voice.cur[0] - voice.prev[0]) * voice.frac;
        const f32 right = voice.prev[1] + (voice.cur[1] - voice.prev[1]) * voice.frac;
        const f32 fade = voice.fade;
        out[2 * i] += left * gainLeft * fade;
        out[2 * i + 1] += right * gainRight * fade;
        gainLeft += deltaLeft;
        gainRight += deltaRight;
        voice.frac += step;
        if (voice.fadeStep != 0.0f) {
            voice.fade += voice.fadeStep;
            if (voice.fade >= 1.0f) {
                voice.fade = 1.0f;
                voice.fadeStep = 0.0f;
            } else if (voice.fade <= 0.0f) {
                finish(slot); // faded out: stopped
                return false;
            }
        }
    }
    voice.gainLeft = targetLeft;
    voice.gainRight = targetRight;
    m_bytesRead[slot].store(voice.sound.codec == pack::SoundCodec::Mp3
                                ? voice.mp3Offset
                                : (voice.sound.codec == pack::SoundCodec::ImaAdpcm
                                       ? voice.nextFrame / std::max<u32>(1, voice.sound.framesPerBlock) *
                                             voice.sound.blockAlign
                                       : voice.nextFrame * channels * 2),
                            std::memory_order_relaxed);
    return true;
}

void Mixer::render(f32* out, u32 frames) {
    VD_PROFILE_ZONE("Mixer::render");
    const Clock::time_point begin = Clock::now();
    std::memset(out, 0, static_cast<usize>(frames) * kChannels * sizeof(f32));
    Command command;
    while (m_commands.pop(command)) {
        apply(command);
    }
    if (frames == 0) {
        return;
    }
    const f32 master = m_master.load(std::memory_order_relaxed);
    f32 categories[kCategoryCount];
    for (u32 c = 0; c < kCategoryCount; ++c) {
        categories[c] = m_categoryVolume[c].load(std::memory_order_relaxed) * master;
    }
    const f32 rate = static_cast<f32>(m_outputRate);
    u32 active = 0;
    for (u32 slot = 0; slot < kMaxVoices; ++slot) {
        Voice& voice = m_voices[slot];
        if (!voice.active) {
            continue;
        }
        const f32 volume = categories[static_cast<u32>(voice.category)];
        const f32 step = static_cast<f32>(voice.sound.sampleRate) * m_pitch[slot].load(std::memory_order_relaxed) / rate;
        if (mix(slot, out, frames, m_gainLeft[slot].load(std::memory_order_relaxed) * volume,
                m_gainRight[slot].load(std::memory_order_relaxed) * volume, step)) {
            ++active;
        }
    }
    // Phase 14: the output stage, make-up gain and peak limiter (the envelope is never below the frame's peak, so
    // nothing leaves above the ceiling).
    const f32 outputGain = m_outputGain.load(std::memory_order_relaxed);
    f32 envelope = m_envelope;
    f32 deepest = 1.0f;
    for (u32 i = 0; i < frames; ++i) {
        const f32 left = out[2 * i] * outputGain;
        const f32 right = out[2 * i + 1] * outputGain;
        const f32 peak = std::max(std::fabs(left), std::fabs(right));
        envelope = std::max(peak, envelope * m_release);
        const f32 gain = envelope > kLimiterCeiling ? kLimiterCeiling / envelope : 1.0f;
        deepest = std::min(deepest, gain);
        out[2 * i] = std::clamp(left * gain, -1.0f, 1.0f);
        out[2 * i + 1] = std::clamp(right * gain, -1.0f, 1.0f);
    }
    m_envelope = envelope < 1e-9f ? 0.0f : envelope; // no denormals in long silences
    if (deepest < m_limiterMin.load(std::memory_order_relaxed)) {
        m_limiterMin.store(deepest, std::memory_order_relaxed);
    }
    m_active.store(active, std::memory_order_relaxed);
    m_renderFrames.fetch_add(frames, std::memory_order_relaxed);
    m_renderNanos.fetch_add(static_cast<u64>(secondsBetween(begin, Clock::now()) * 1e9), std::memory_order_relaxed);
}

} // namespace vd::audio
