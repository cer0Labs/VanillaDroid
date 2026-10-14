#include "audio/AudioDevice.h"

#include "core/Log.h"
#include "platform/Thread.h"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace vd::audio {
namespace {

constexpr u32 kChannels = 2;
constexpr u32 kNullRate = 48000;
constexpr u32 kWavChunkFrames = 1024;

void writeU16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void writeU32(u8* p, u32 v) {
    for (u32 i = 0; i < 4; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}

// A 16-bit PCM stereo WAV header for `frames` frames.
void wavHeader(u8 (&header)[44], u32 rate, u64 frames) {
    const u32 dataBytes = static_cast<u32>(std::min<u64>(frames * kChannels * 2, 0xFFFFFFF0u - 36));
    std::memcpy(header, "RIFF", 4);
    writeU32(header + 4, 36 + dataBytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    writeU32(header + 16, 16);
    writeU16(header + 20, 1); // PCM
    writeU16(header + 22, kChannels);
    writeU32(header + 24, rate);
    writeU32(header + 28, rate * kChannels * 2);
    writeU16(header + 32, kChannels * 2);
    writeU16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    writeU32(header + 40, dataBytes);
}

} // namespace

bool parseAudioOutput(std::string_view text, AudioOutput& output, std::string& wavPath) {
    if (text == "off") {
        output = AudioOutput::Off;
    } else if (text == "null") {
        output = AudioOutput::Null;
    } else if (text == "device") {
        output = AudioOutput::Device;
    } else if (text.substr(0, 4) == "wav:" && text.size() > 4) {
        output = AudioOutput::Wav;
        wavPath = std::string(text.substr(4));
    } else {
        return false;
    }
    return true;
}

const char* audioOutputName(AudioOutput output) {
    switch (output) {
    case AudioOutput::Off: return "off";
    case AudioOutput::Null: return "null";
    case AudioOutput::Device: return "device";
    case AudioOutput::Wav: return "wav";
    }
    return "?";
}

struct AudioDevice::Impl {
    ma_context context;
    ma_device device;
    bool contextReady = false;
    bool deviceReady = false;
    bool threadNamed = false;
    RenderFn render = nullptr;
    void* user = nullptr;
    std::FILE* wav = nullptr;
    u64 wavFrames = 0;
    u64 wavHeaderFrames = 0;
    i16 wavScratch[kWavChunkFrames * kChannels] = {};
    std::atomic<u64> frames{0};

    static void callback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
        auto* impl = static_cast<Impl*>(device->pUserData);
        if (!impl->threadNamed) {
            impl->threadNamed = true;
            setCurrentThreadName("vd-audio-mix");
        }
        auto* out = static_cast<f32*>(output);
        impl->render(impl->user, out, frameCount);
        impl->frames.fetch_add(frameCount, std::memory_order_relaxed);
        if (impl->wav) {
            // Development output (wav:<file>): the null backend's thread may block on the disk.
            for (u32 done = 0; done < frameCount;) {
                const u32 n = std::min<u32>(kWavChunkFrames, frameCount - done);
                for (u32 i = 0; i < n * kChannels; ++i) {
                    const f32 s = std::clamp(out[done * kChannels + i], -1.0f, 1.0f);
                    impl->wavScratch[i] = static_cast<i16>(s * 32767.0f);
                }
                std::fwrite(impl->wavScratch, sizeof(i16), n * kChannels, impl->wav);
                impl->wavFrames += n;
                done += n;
            }
            // The header follows every second or so: a run killed before close() still leaves a valid file.
            if (impl->wavFrames - impl->wavHeaderFrames >= device->sampleRate) {
                u8 header[44];
                wavHeader(header, device->sampleRate, impl->wavFrames);
                std::fseek(impl->wav, 0, SEEK_SET);
                std::fwrite(header, 1, sizeof(header), impl->wav);
                std::fseek(impl->wav, 0, SEEK_END);
                std::fflush(impl->wav);
                impl->wavHeaderFrames = impl->wavFrames;
            }
        }
    }
};

AudioDevice::AudioDevice() = default;

AudioDevice::~AudioDevice() {
    close();
}

Result<void> AudioDevice::open(AudioOutput output, const std::string& wavPath, RenderFn render, void* user) {
    close();
    if (output == AudioOutput::Off || !render) {
        return Error{"audio output is off"};
    }
    m_impl = std::make_unique<Impl>();
    Impl& impl = *m_impl;
    impl.render = render;
    impl.user = user;

    // The null backend for null / wav (never the PC's speakers); the platform's backends otherwise.
    const ma_backend nullBackend[] = {ma_backend_null};
    const bool silent = output != AudioOutput::Device;
    ma_context_config contextConfig = ma_context_config_init();
    if (ma_context_init(silent ? nullBackend : nullptr, silent ? 1 : 0, &contextConfig, &impl.context) != MA_SUCCESS) {
        m_impl.reset();
        return Error{"miniaudio: no audio backend"};
    }
    impl.contextReady = true;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = silent ? kNullRate : 0; // 0: the device's native rate, no resampling in miniaudio
    config.dataCallback = &Impl::callback;
    config.pUserData = &impl;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE; // the mixer overwrites the buffer
    config.noClip = MA_TRUE;                    // and clips it
    config.aaudio.usage = ma_aaudio_usage_game;
    config.aaudio.contentType = ma_aaudio_content_type_music;
    config.opensl.streamType = ma_opensl_stream_type_media;
    if (ma_device_init(&impl.context, &config, &impl.device) != MA_SUCCESS) {
        close();
        return Error{"miniaudio: could not open the playback device"};
    }
    impl.deviceReady = true;
    if (output == AudioOutput::Wav) {
        impl.wav = std::fopen(wavPath.c_str(), "wb");
        if (!impl.wav) {
            close();
            return Error{"cannot write " + wavPath};
        }
        u8 header[44];
        wavHeader(header, impl.device.sampleRate, 0);
        std::fwrite(header, 1, sizeof(header), impl.wav);
    }
    VD_LOG_INFO("Audio: %s output, %s backend, %u Hz, period %u frames", audioOutputName(output), backendName(),
                impl.device.sampleRate, impl.device.playback.internalPeriodSizeInFrames);
    return {};
}

void AudioDevice::close() {
    if (!m_impl) {
        return;
    }
    Impl& impl = *m_impl;
    const u32 rate = impl.deviceReady ? impl.device.sampleRate : 0;
    if (impl.deviceReady) {
        ma_device_uninit(&impl.device); // stops it: the callback is not running any more
    }
    if (impl.wav) {
        u8 header[44];
        wavHeader(header, rate, impl.wavFrames);
        std::fseek(impl.wav, 0, SEEK_SET);
        std::fwrite(header, 1, sizeof(header), impl.wav);
        std::fclose(impl.wav);
        VD_LOG_INFO("Audio: %.1f s of mix written", static_cast<f64>(impl.wavFrames) / std::max(1u, rate));
    }
    if (impl.contextReady) {
        ma_context_uninit(&impl.context);
    }
    m_impl.reset();
}

bool AudioDevice::isOpen() const {
    return m_impl && m_impl->deviceReady;
}

bool AudioDevice::start() {
    return isOpen() && ma_device_start(&m_impl->device) == MA_SUCCESS;
}

void AudioDevice::stop() {
    if (isOpen()) {
        ma_device_stop(&m_impl->device);
    }
}

bool AudioDevice::running() const {
    return isOpen() && ma_device_is_started(&m_impl->device);
}

u32 AudioDevice::sampleRate() const {
    return isOpen() ? m_impl->device.sampleRate : 0;
}

const char* AudioDevice::backendName() const {
    return m_impl && m_impl->contextReady ? ma_get_backend_name(m_impl->context.backend) : "none";
}

u64 AudioDevice::framesRendered() const {
    return m_impl ? m_impl->frames.load(std::memory_order_relaxed) : 0;
}

} // namespace vd::audio
