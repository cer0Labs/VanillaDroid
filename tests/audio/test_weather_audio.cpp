// The weather's sound (phase 11): app/GameAudio plays SMSG_WEATHER's loop (SoundEntries "Weather - RainLight"
// ...) on the ambience at the weather's faded volume, cross-fades to another loop, and stops it out of the
// world and on a loading screen. A real AudioSystem on the null backend (nothing reaches the speakers) and
// the baked packs (VD_PACKS_DIR: db.vdpk, sound.vdpk); skipped without them.
#include "app/GameAudio.h"
#include "audio/AudioSystem.h"
#include "audio/SoundData.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

namespace {

using namespace vd;

struct WeatherAudio {
    pack::PackReader db;
    audio::AudioSystem system;
    audio::SoundData data;
    GameAudio sounds;
    u32 now = 1000;
    bool ready = false;

    WeatherAudio() {
        const char* dir = std::getenv("VD_PACKS_DIR");
        if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() ||
            !std::filesystem::exists(std::filesystem::path(dir) / "sound.vdpk")) {
            return;
        }
        audio::AudioConfig config;
        config.output = audio::AudioOutput::Null;
        REQUIRE(system.init(config, dir).ok());
        REQUIRE(data.init(db));
        sounds.init(&system, &data);
        ready = sounds.active();
    }

    void update(u32 weatherSound, f32 volume, bool world = true, bool loading = false) {
        GameAudioFrame f;
        f.nowMs = now;
        f.dt = 1.0f / 60.0f;
        f.listener = glm::dvec3(-9464.0, 62.0, 58.0); // Goldshire
        f.world = world;
        f.loading = loading;
        f.area = 87; // Goldshire
        f.weatherSound = weatherSound;
        f.weatherVolume = volume;
        sounds.update(f);
        system.update();
        now += 16;
    }
    bool played(u32 soundEntry) const {
        for (u32 i = 0; i < 12; ++i) {
            if (sounds.recentSound(i) == soundEntry) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("The weather's loop follows the weather") {
    WeatherAudio a;
    if (!a.ready) {
        MESSAGE("VD_PACKS_DIR with db.vdpk and sound.vdpk not available: skipped");
        return;
    }
    REQUIRE(a.data.entry(8535) != nullptr); // "Weather - RainHeavy"
    // Heavy rain coming in: its loop starts quietly and grows with the weather.
    a.update(8535, 0.1f);
    CHECK(a.sounds.weatherEntry() == 8535u);
    CHECK(a.played(8535));
    CHECK(a.sounds.weatherVolume() == doctest::Approx(0.1f));
    a.update(8535, 0.105f); // a change too small to send
    CHECK(a.sounds.weatherVolume() == doctest::Approx(0.1f));
    a.update(8535, 0.6f);
    CHECK(a.sounds.weatherVolume() == doctest::Approx(0.6f));
    // Medium rain: the other loop cross-fades in.
    a.update(8534, 1.0f);
    CHECK(a.sounds.weatherEntry() == 8534u);
    CHECK(a.played(8534));
    // The sandstorm's loops exist too.
    a.update(8558, 1.0f);
    CHECK(a.played(8558));
    // A loading screen, then out of the world: silence.
    a.update(8558, 1.0f, true, true);
    CHECK(a.sounds.weatherEntry() == 0u);
    a.update(8558, 1.0f);
    CHECK(a.sounds.weatherEntry() == 8558u);
    a.update(8558, 1.0f, false);
    CHECK(a.sounds.weatherEntry() == 0u);
    // Fine: none.
    a.update(0, 0.0f);
    CHECK(a.sounds.weatherEntry() == 0u);
}
