#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <memory>
#include <string>
#include <string_view>

namespace vd::audio {

// Where the mix goes.
enum class AudioOutput : u8 {
    Off,    // nothing runs: no device, no mixer (the desktop default: the development PC stays silent)
    Null,   // miniaudio's null backend: the mixer runs in real time and its output is thrown away
    Device, // the system's output (AAudio, OpenSL ES as fallback; WASAPI on the desktop)
    Wav,    // the null backend, and the mix is written to a WAV file (16-bit stereo) to inspect it
};

// "off", "null", "device" or "wav:<file>". False for anything else.
bool parseAudioOutput(std::string_view text, AudioOutput& output, std::string& wavPath);
const char* audioOutputName(AudioOutput output);

// The miniaudio playback device: 2 channels f32 at its native rate (48 kHz on the null backend), pulling the mix
// from a render function on its own audio thread. Opened and closed on the game thread (not per frame: opening
// and starting the device block briefly).
class AudioDevice {
public:
    // Called on the audio thread: fill `frames` interleaved stereo frames.
    using RenderFn = void (*)(void* user, f32* out, u32 frames);

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // `output` must not be Off. The device is created stopped: start() it.
    Result<void> open(AudioOutput output, const std::string& wavPath, RenderFn render, void* user);
    void close(); // stops it; finishes the WAV file
    bool isOpen() const;

    bool start();
    void stop(); // the app goes to the background: the stream is released until start()
    bool running() const;

    u32 sampleRate() const;
    const char* backendName() const;
    u64 framesRendered() const; // since open()

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vd::audio
