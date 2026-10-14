// Weather (phase 11), the world's side without a GPU: the fade from the server's weather (smooth ramps,
// instant changes, a type change taking the old weather away first, the loop following the weather shown),
// the ground grid around the camera (only what comes into the window is sampled, nearest first, within a
// budget; the shader's indexing finds the same points), the particles at each "Weather Intensity", hiding
// indoors and under water, the drops' motion blur, and the option in the quality presets, the settings file
// and the texts (ui.vdpk).
#include "TestData.h"
#include "app/Dialogs.h"
#include "app/QualityGovernor.h"
#include "app/SettingsFile.h"
#include "core/Text.h"
#include "world/WeatherEffect.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace {

using namespace vd;
using game::WeatherType;

void run(WeatherFade& fade, f32 seconds, f32 step = 0.1f) {
    for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += step) {
        fade.update(step);
    }
}

struct FakeGround {
    u32 calls = 0;
    bool none = false;
    static f32 sample(const void* self, f64 x, f64 y, f64 top, f64 bottom) {
        auto* ground = const_cast<FakeGround*>(static_cast<const FakeGround*>(self));
        ++ground->calls;
        static_cast<void>(top);
        static_cast<void>(bottom);
        return ground->none ? std::numeric_limits<f32>::quiet_NaN() : height(x, y);
    }
    static f32 height(f64 x, f64 y) { return static_cast<f32>(10.0 + x * 0.01 - y * 0.02); }
};

WeatherFrame frameAt(const glm::dvec3& camera, f32 dt = 1.0f / 60.0f) {
    WeatherFrame f;
    f.dt = dt;
    f.camera = camera;
    f.cameraForward = glm::vec3(1.0f, 0.0f, 0.0f);
    f.cameraRight = glm::vec3(0.0f, -1.0f, 0.0f);
    return f;
}

game::WeatherState weather(WeatherType type, f32 grade, bool instant, u32 sound = 0) {
    return {type, grade, sound, instant};
}

} // namespace

TEST_CASE("The weather fades in, out and between types") {
    WeatherFade fade;
    CHECK(fade.type() == WeatherType::Fine);
    // Smooth: from nothing, kRate a second.
    fade.set(WeatherType::Rain, 0.6f, 8534, false);
    run(fade, 1.0f);
    CHECK(fade.type() == WeatherType::Rain);
    CHECK(fade.intensity() == doctest::Approx(WeatherFade::kRate).epsilon(0.02));
    CHECK(fade.sound() == 8534u);
    // The loop fades in too.
    CHECK(fade.soundVolume() == doctest::Approx(WeatherFade::kRate / 0.6f).epsilon(0.02));
    run(fade, 6.0f);
    CHECK(fade.intensity() == doctest::Approx(0.6f));
    CHECK(fade.settled());
    CHECK(fade.soundVolume() == doctest::Approx(1.0f));

    // Heavier, same type: the new loop at once, the particles ramp.
    fade.set(WeatherType::Rain, 1.0f, 8535, false);
    CHECK(fade.sound() == 8535u);
    run(fade, 2.0f);
    CHECK(fade.intensity() == doctest::Approx(0.8f).epsilon(0.02));

    // Another type: the rain goes first (its loop with it), then the snow comes.
    fade.set(WeatherType::Snow, 0.5f, 8536, false);
    run(fade, 4.0f);
    CHECK(fade.type() == WeatherType::Rain);
    CHECK(fade.intensity() == doctest::Approx(0.4f).epsilon(0.02));
    CHECK(fade.sound() == 8535u);
    CHECK(fade.soundVolume() == doctest::Approx(0.4f).epsilon(0.02));
    run(fade, 4.2f);
    CHECK(fade.type() == WeatherType::Snow);
    CHECK(fade.sound() == 8536u);
    CHECK(fade.intensity() < 0.05f);
    run(fade, 6.0f);
    CHECK(fade.intensity() == doctest::Approx(0.5f));

    // Fine: it fades away, then nothing is shown.
    fade.set(WeatherType::Fine, 0.0001f, 0, false);
    run(fade, 2.0f);
    CHECK(fade.type() == WeatherType::Snow);
    run(fade, 4.0f);
    CHECK(fade.type() == WeatherType::Fine);
    CHECK(fade.intensity() == 0.0f);
    CHECK(fade.soundVolume() == 0.0f);

    // At once (the first weather of a new world).
    fade.set(WeatherType::Sandstorm, 0.9f, 8557, true);
    CHECK(fade.type() == WeatherType::Sandstorm);
    CHECK(fade.intensity() == doctest::Approx(0.9f));
    CHECK(fade.soundVolume() == doctest::Approx(1.0f));
    fade.set(WeatherType::Fine, 0.0f, 0, true);
    CHECK(fade.type() == WeatherType::Fine);
    CHECK(fade.intensity() == 0.0f);
}

TEST_CASE("The ground around the camera") {
    WeatherGround ground;
    FakeGround fake;
    const glm::dvec3 camera(-9500.3, 30.7, 50.0);
    constexpr u32 kPoints = WeatherGround::kSide * WeatherGround::kSide;
    // Within the budget, nearest to the camera first.
    CHECK(ground.update(camera, &FakeGround::sample, &fake, 10) == 10);
    CHECK(ground.unknown() == kPoints - 10);
    const i32 cx = static_cast<i32>(std::floor(camera.x / WeatherGround::kSpacing));
    const i32 cy = static_cast<i32>(std::floor(camera.y / WeatherGround::kSpacing));
    CHECK(ground.at(cx, cy) == doctest::Approx(FakeGround::height(cx * 1.5, cy * 1.5)));
    CHECK(ground.at(cx + 10, cy) == WeatherGround::kNone); // far: not yet
    // The rest; then a still camera samples only the slow refresh.
    ground.update(camera, &FakeGround::sample, &fake, kPoints);
    CHECK(ground.unknown() == 0u);
    CHECK(ground.at(cx + 10, cy - 7) == doctest::Approx(FakeGround::height((cx + 10) * 1.5, (cy - 7) * 1.5)));
    CHECK(ground.at(cx + 40, cy) == WeatherGround::kNone); // out of the window
    fake.calls = 0;
    CHECK(ground.update(camera, &FakeGround::sample, &fake, kPoints) == WeatherGround::kRefreshPerFrame);
    // One step north: one row of the window comes in.
    fake.calls = 0;
    const glm::dvec3 moved = camera + glm::dvec3(WeatherGround::kSpacing, 0.0, 0.0);
    CHECK(ground.update(moved, &FakeGround::sample, &fake, kPoints) ==
          WeatherGround::kSide + WeatherGround::kRefreshPerFrame);
    CHECK(ground.at(cx + 17, cy) == doctest::Approx(FakeGround::height((cx + 17) * 1.5, cy * 1.5)));
    // Much higher: the column the sampler looked down moved with the camera, everything again.
    CHECK(ground.update(moved + glm::dvec3(0.0, 0.0, 20.0), &FakeGround::sample, &fake, kPoints) ==
          kPoints + WeatherGround::kRefreshPerFrame);
    // Where the sampler finds nothing: none.
    fake.none = true;
    ground.clear();
    ground.update(camera, &FakeGround::sample, &fake, kPoints);
    CHECK(ground.at(cx, cy) == WeatherGround::kNone);
    CHECK(ground.unknown() == 0u);
}

TEST_CASE("The shader finds the ground's points where the grid put them") {
    // shaders/weather.glsl: floor((relative + phase) / spacing) & 31 is the world point's slot.
    WeatherGround ground;
    FakeGround fake;
    const glm::dvec3 camera(-9500.3, -30.7, 50.0);
    ground.update(camera, &FakeGround::sample, &fake, 2000);
    const glm::vec2 phase = WeatherGround::phase(camera);
    for (const glm::vec2 relative :
         {glm::vec2(5.2f, -3.1f), glm::vec2(-20.0f, 19.5f), glm::vec2(0.0f, 0.0f)}) {
        const i32 gx = static_cast<i32>(std::floor((relative.x + phase.x) / WeatherGround::kSpacing));
        const i32 gy = static_cast<i32>(std::floor((relative.y + phase.y) / WeatherGround::kSpacing));
        const u32 slot = (static_cast<u32>(gy) & 31u) * 32u + (static_cast<u32>(gx) & 31u);
        const i32 wx = static_cast<i32>(std::floor((camera.x + relative.x) / WeatherGround::kSpacing));
        const i32 wy = static_cast<i32>(std::floor((camera.y + relative.y) / WeatherGround::kSpacing));
        CHECK(ground.heights()[slot] == doctest::Approx(FakeGround::height(wx * 1.5, wy * 1.5)));
    }
}

TEST_CASE("Particles at each Weather Intensity") {
    CHECK(weatherParticleCount(WeatherType::Rain, 1.0f, 3, 1.0f) == kWeatherMaxParticles);
    CHECK(weatherParticleCount(WeatherType::Rain, 1.0f, 0, 1.0f) == kWeatherMaxParticles / 4);
    CHECK(weatherParticleCount(WeatherType::Snow, 0.5f, 3, 1.0f) == kWeatherMaxParticles / 2);
    // Hot phone: the dynamic quality's effects at half.
    CHECK(weatherParticleCount(WeatherType::Sandstorm, 1.0f, 1, 0.5f) == kWeatherMaxParticles / 4);
    CHECK(weatherParticleCount(WeatherType::Rain, 2.0f, 9, 2.0f) == kWeatherMaxParticles); // clamped
    CHECK(weatherParticleCount(WeatherType::Fine, 1.0f, 3, 1.0f) == 0u);
    CHECK(weatherParticleCount(WeatherType::Rain, 0.0f, 3, 1.0f) == 0u);
}

TEST_CASE("The weather around the camera: light, indoors, under water") {
    WeatherEffect effect;
    FakeGround fake;
    glm::dvec3 camera(-9500.0, 30.0, 50.0);
    effect.update(frameAt(camera), &FakeGround::sample, &fake);
    CHECK(effect.uniforms() == nullptr); // fine
    CHECK(effect.storm() == 0.0f);

    effect.set(weather(WeatherType::Rain, 1.0f, true, 8535));
    effect.update(frameAt(camera), &FakeGround::sample, &fake);
    REQUIRE(effect.uniforms() != nullptr);
    const WeatherUniforms& u = *effect.uniforms();
    CHECK(u.time.y == 1u);
    CHECK(u.particles.life.x == kWeatherMaxParticles);
    CHECK(u.particles.velocity.z < -10.0f);         // falling
    CHECK(effect.storm() == doctest::Approx(1.0f)); // Light.dbc's storm sets
    CHECK(effect.sound() == 8535u);
    CHECK(effect.soundVolume() == doctest::Approx(1.0f));
    CHECK(effect.stats().groundSampled == WeatherEffect::kGroundBudget);
    // The camera's z for the heights, the ground's phase, every phase within its box.
    CHECK(u.cameraUp.w == doctest::Approx(50.0f));
    CHECK(u.ground.z == WeatherGround::kSpacing);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(u.particles.phase[i] >= 0.0f);
        CHECK(u.particles.phase[i] < u.particles.box[i]);
    }

    // Into a building: gone in a quarter of a second, the sound muffled (the storm light stays: it is
    // outside).
    WeatherFrame inside = frameAt(camera);
    inside.indoors = true;
    effect.update(inside, &FakeGround::sample, &fake);
    REQUIRE(effect.uniforms() != nullptr);
    CHECK(effect.visibility() < 1.0f);
    for (u32 i = 0; i < 20; ++i) {
        effect.update(inside, &FakeGround::sample, &fake);
    }
    CHECK(effect.uniforms() == nullptr);
    CHECK(effect.soundVolume() == doctest::Approx(WeatherEffect::kIndoorVolume));
    CHECK(effect.storm() == doctest::Approx(1.0f));
    // Out again, then under water: at once nothing, and silence.
    for (u32 i = 0; i < 20; ++i) {
        effect.update(frameAt(camera), &FakeGround::sample, &fake);
    }
    CHECK(effect.uniforms() != nullptr);
    WeatherFrame under = frameAt(camera);
    under.underwater = true;
    effect.update(under, &FakeGround::sample, &fake);
    CHECK(effect.uniforms() == nullptr);
    CHECK(effect.soundVolume() == 0.0f);

    // The option and the dynamic quality.
    WeatherFrame low = frameAt(camera);
    low.density = 1;
    low.effectScale = 0.5f;
    effect.update(low, &FakeGround::sample, &fake);
    REQUIRE(effect.uniforms() != nullptr);
    CHECK(effect.uniforms()->particles.life.x == weatherParticleCount(WeatherType::Rain, 1.0f, 1, 0.5f));

    // Snow and sand bring their mists; sand rounds its grains off.
    effect.set(weather(WeatherType::Snow, 1.0f, true));
    effect.update(frameAt(camera), &FakeGround::sample, &fake);
    REQUIRE(effect.uniforms() != nullptr);
    CHECK(effect.uniforms()->time.y == 2u);
    CHECK(effect.uniforms()->mist.life.x > 0u);
    CHECK(effect.uniforms()->particles.fades.z > 0.0f); // flakes settle
    effect.set(weather(WeatherType::Sandstorm, 1.0f, true));
    effect.update(frameAt(camera), &FakeGround::sample, &fake);
    REQUIRE(effect.uniforms() != nullptr);
    CHECK(effect.uniforms()->time.y == 3u);
    CHECK(effect.uniforms()->particles.fades.z == 0.0f); // grains never land
    CHECK(effect.uniforms()->particles.fades.w == 1.0f);

    // Another map: fine at once.
    effect.reset();
    effect.update(frameAt(camera), &FakeGround::sample, &fake);
    CHECK(effect.uniforms() == nullptr);
}

TEST_CASE("The drops' motion blur follows the camera's motion") {
    WeatherEffect effect;
    effect.set(weather(WeatherType::Rain, 1.0f, true));
    glm::dvec3 camera(-9500.0, 30.0, 50.0);
    const auto rotate = [](const WeatherUniforms& u, const glm::vec3& v) {
        return glm::vec3(glm::dot(glm::vec3(u.blur[0]), v), glm::dot(glm::vec3(u.blur[1]), v),
                         glm::dot(glm::vec3(u.blur[2]), v));
    };
    // Still: no rotation, the drops' own tail.
    for (u32 i = 0; i < 5; ++i) {
        effect.update(frameAt(camera), nullptr, nullptr);
    }
    REQUIRE(effect.uniforms() != nullptr);
    const glm::vec3 fall = glm::vec3(effect.uniforms()->particles.velocity);
    CHECK(glm::length(rotate(*effect.uniforms(), glm::vec3(1.0f, 2.0f, 3.0f)) - glm::vec3(1.0f, 2.0f, 3.0f)) <
          1e-3f);
    CHECK(effect.uniforms()->rain.y == doctest::Approx(2.0f));
    // Running north at 7 yd/s: the fall turns towards (fall - camera velocity), a rotation, and the tail
    // stretches.
    const f32 dt = 1.0f / 60.0f;
    for (u32 i = 0; i < 120; ++i) {
        camera.x += 7.0 * dt;
        effect.update(frameAt(camera, dt), nullptr, nullptr);
    }
    const WeatherUniforms& u = *effect.uniforms();
    const glm::vec3 seen = glm::normalize(fall - glm::vec3(7.0f, 0.0f, 0.0f));
    CHECK(glm::length(rotate(u, glm::normalize(fall)) - seen) < 0.01f);
    CHECK(glm::length(rotate(u, glm::vec3(0.0f, 1.0f, 0.0f))) == doctest::Approx(1.0f)); // a rotation
    CHECK(u.rain.y > 2.0f);
    CHECK(u.rain.y <= 4.0f);
    // A teleport is no motion.
    camera.x += 500.0;
    effect.update(frameAt(camera, dt), nullptr, nullptr);
    CHECK(glm::length(rotate(*effect.uniforms(), glm::vec3(0.0f, 0.0f, 1.0f)) - glm::vec3(0.0f, 0.0f, 1.0f)) <
          1e-3f);
}

TEST_CASE("Weather Intensity in the presets and the settings file") {
    SettingsView settings;
    CHECK(settings.weatherDensity == 3u);
    const u32 expected[kQualityPresets] = {1, 2, 3, 3};
    for (u32 preset = 0; preset < kQualityPresets; ++preset) {
        applyQualityPreset(settings, preset);
        CHECK(settings.weatherDensity == expected[preset]);
        CHECK(currentQualityPreset(settings) == preset);
    }
    applyQualityPreset(settings, static_cast<u32>(QualityPreset::High));
    settings.weatherDensity = 1; // moved on its own
    CHECK(currentQualityPreset(settings) == static_cast<u32>(QualityPreset::Custom));
    SettingsView read;
    parseSettings(formatSettings(settings), read);
    CHECK(read.weatherDensity == 1u);
    parseSettings("weather_density = 7\n", read);
    CHECK(read.weatherDensity == 3u); // clamped
}

TEST_CASE("Weather Intensity is the client's text, in English and Spanish") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    CHECK(text::get("WEATHER_DETAIL") == "Weather Intensity"); // GlobalStrings.lua
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    // The esES client's own text (phase 21), or ours in locale/esES/weather.lang.
    CHECK(text::get("WEATHER_DETAIL") == (test::esESClientTexts() ? "Intensidad del tiempo" : "Intensidad del clima"));
    text::unload();
}
