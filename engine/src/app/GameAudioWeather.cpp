#include "app/GameAudio.h"

#include "app/GameAudioInternal.h"

#include <cmath>

namespace vd {
namespace {

using namespace gameaudio;

constexpr f32 kWeatherFade = 2.0f;        // s: one loop into the next (light, medium, heavy; another weather)
constexpr f32 kWeatherVolumeStep = 0.01f; // smaller changes are not sent to the mixer

} // namespace

// Phase 11: the weather's loop. SMSG_WEATHER names it (vmangos Weather::GetSound: SoundEntries "Weather -
// RainLight"
// ... "Weather - SandstormHeavy", a loop of Sound\Ambience\Weather in the ambience category, 2D, at the row's
// volume 0.69); the world fades it in and out with the weather (WeatherEffect::soundVolume), a new loop
// cross-fades. Out of the world, or on a loading screen, it stops.
void GameAudio::weatherSounds(const GameAudioFrame& f) {
    const u32 entry = f.world && !f.login && !f.loading ? f.weatherSound : 0u;
    const f32 volume = std::fmax(0.0f, std::fmin(f.weatherVolume, 1.0f));
    if (entry != m_weatherEntry) {
        m_audio->stop(m_weather, kWeatherFade);
        m_weather = entry != 0 ? playSound(entry, audio::Category::Ambience, kPriorityAmbience, nullptr, true,
                                           kWeatherFade, volume)
                               : audio::VoiceHandle{};
        m_weatherEntry = entry;
        m_weatherVolume = volume;
        return;
    }
    if (m_weather && std::fabs(volume - m_weatherVolume) >= kWeatherVolumeStep) {
        const audio::SoundEntry* e = m_data->entry(entry);
        m_audio->setVolume(m_weather, (e ? e->volume : 1.0f) * volume);
        m_weatherVolume = volume;
    }
}

} // namespace vd
