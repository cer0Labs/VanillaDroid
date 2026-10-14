// Phase 10: the dynamic quality (app/QualityGovernor: the temperature and the missed frames lower the world's
// resolution, the distances and the effects, and give them back), the quality presets and the settings file
// (app/SettingsFile).
#include "app/QualityGovernor.h"
#include "app/SettingsFile.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

namespace {

using namespace vd;

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

// `seconds` of frames at 60 fps with these times and this thermal state.
void run(QualityGovernor& governor, f32 seconds, f32 frameMs = 16.67f, f32 cpuMs = 4.0f, f32 gpuMs = 6.0f,
         f32 headroom = kNaN, i32 status = -1, bool measuring = true) {
    const u32 frames = static_cast<u32>(seconds * 1000.0f / frameMs + 0.5f);
    for (u32 i = 0; i < frames; ++i) {
        GovernorInput input;
        input.dt = frameMs / 1000.0f;
        input.frameMs = frameMs;
        input.cpuMs = cpuMs;
        input.gpuMs = gpuMs;
        input.thermalHeadroom = headroom;
        input.thermalStatus = status;
        input.measuring = measuring;
        governor.update(input);
    }
}

QualityGovernor makeGovernor(f32 baseScale = 1.0f, bool dynamic = true) {
    QualityGovernor governor;
    QualityGovernor::Config config;
    config.dynamic = dynamic;
    config.baseScale = baseScale;
    governor.configure(config);
    return governor;
}

} // namespace

TEST_CASE("Quality governor: a cool device that keeps 60 fps keeps the player's settings") {
    QualityGovernor governor = makeGovernor();
    run(governor, 120.0f, 16.67f, 4.0f, 6.0f, 0.4f, 0);
    const GovernorOutput& q = governor.output();
    CHECK(q.renderScale == doctest::Approx(1.0f));
    CHECK(q.distanceScale == doctest::Approx(1.0f));
    CHECK(q.effectScale == doctest::Approx(1.0f));
    CHECK(q.thermalLevel == 0);
    CHECK(q.frameInterval == 1);
    CHECK(governor.scaleChanges() == 0);
}

TEST_CASE("Quality governor: the thermal headroom lowers the ceiling a level at a time, then gives it back") {
    QualityGovernor governor = makeGovernor();
    // 0.87: level 2 (from 0.85). A level needs its threshold for 2 s: one after 2 s, the next after 4 s.
    run(governor, 1.0f, 16.67f, 4.0f, 6.0f, 0.87f, 1);
    CHECK(governor.output().thermalLevel == 0);
    run(governor, 1.5f, 16.67f, 4.0f, 6.0f, 0.87f, 1);
    CHECK(governor.output().thermalLevel == 1);
    CHECK(governor.output().renderScale == doctest::Approx(0.9f));
    CHECK(governor.output().distanceScale == doctest::Approx(0.9f));
    CHECK(governor.output().effectScale == doctest::Approx(0.85f));
    run(governor, 2.5f, 16.67f, 4.0f, 6.0f, 0.87f, 1);
    CHECK(governor.output().thermalLevel == 2);
    CHECK(governor.output().renderScale == doctest::Approx(0.8f));
    CHECK(governor.output().distanceScale == doctest::Approx(0.8f));
    CHECK(governor.output().effectScale == doctest::Approx(0.7f));
    // Just under the threshold is not enough to leave (0.07 of margin), even for a long time.
    run(governor, 60.0f, 16.67f, 4.0f, 6.0f, 0.82f, 0);
    CHECK(governor.output().thermalLevel == 2);
    // Clearly below: a level down every 30 s.
    run(governor, 31.0f, 16.67f, 4.0f, 6.0f, 0.5f, 0);
    CHECK(governor.output().thermalLevel == 1);
    run(governor, 31.0f, 16.67f, 4.0f, 6.0f, 0.5f, 0);
    CHECK(governor.output().thermalLevel == 0);
    CHECK(governor.output().renderScale == doctest::Approx(1.0f));
}

TEST_CASE("Quality governor: the system's status counts where the headroom is unknown; critical caps at 30 fps") {
    QualityGovernor governor = makeGovernor();
    run(governor, 10.0f, 16.67f, 4.0f, 6.0f, kNaN, 3); // severe: level 3
    CHECK(governor.output().thermalLevel == 3);
    CHECK(governor.output().renderScale == doctest::Approx(0.7f));
    CHECK(governor.output().frameInterval == 1);
    run(governor, 3.0f, 16.67f, 4.0f, 6.0f, kNaN, 4); // critical
    CHECK(governor.output().thermalLevel == 4);
    CHECK(governor.output().renderScale == doctest::Approx(0.6f));
    CHECK(governor.output().frameInterval == 2);
}

TEST_CASE("Quality governor: missed frames with the GPU behind lower the resolution; calm brings it back") {
    QualityGovernor governor = makeGovernor();
    // Every other frame late (25 ms), the GPU the slower: -0.1 a second.
    for (u32 second = 0; second < 2; ++second) {
        for (u32 i = 0; i < 48; ++i) {
            GovernorInput input;
            input.dt = (i % 2 ? 25.0f : 16.67f) / 1000.0f;
            input.frameMs = i % 2 ? 25.0f : 16.67f;
            input.cpuMs = 5.0f;
            input.gpuMs = 15.0f;
            governor.update(input);
        }
    }
    CHECK(governor.output().renderScale == doctest::Approx(0.8f));
    CHECK(governor.output().loadLevel == 0);
    // 10 s without misses: one step back up (+0.05), then another after 10 s more.
    run(governor, 10.5f);
    CHECK(governor.output().renderScale == doctest::Approx(0.85f));
    run(governor, 10.5f);
    CHECK(governor.output().renderScale == doctest::Approx(0.9f));
}

TEST_CASE("Quality governor: a step up that brings the misses back waits twice as long before the next try") {
    QualityGovernor governor = makeGovernor();
    run(governor, 2.0f, 25.0f, 5.0f, 15.0f); // 2 s of misses: 0.8
    CHECK(governor.output().renderScale == doctest::Approx(0.8f));
    run(governor, 10.0f); // probe: 0.85
    CHECK(governor.output().renderScale == doctest::Approx(0.85f));
    run(governor, 1.0f, 25.0f, 5.0f, 15.0f); // misses right after it: back down, the next try in 20 s
    CHECK(governor.output().renderScale == doctest::Approx(0.75f));
    run(governor, 12.0f);
    CHECK(governor.output().renderScale == doctest::Approx(0.75f));
    run(governor, 9.0f);
    CHECK(governor.output().renderScale == doctest::Approx(0.8f));
}

TEST_CASE("Quality governor: missed frames with the CPU behind shorten the distances and the effects") {
    QualityGovernor governor = makeGovernor();
    run(governor, 1.0f, 25.0f, 20.0f, 5.0f);
    CHECK(governor.output().renderScale == doctest::Approx(1.0f));
    CHECK(governor.output().loadLevel == 1);
    CHECK(governor.output().distanceScale == doctest::Approx(0.85f));
    CHECK(governor.output().effectScale == doctest::Approx(0.75f));
    run(governor, 10.5f); // calm: the CPU's work comes back (the resolution was never lowered)
    CHECK(governor.output().loadLevel == 0);
    CHECK(governor.output().distanceScale == doctest::Approx(1.0f));
}

TEST_CASE("Quality governor: misses with the GPU and the CPU far from busy come from outside and change nothing") {
    QualityGovernor governor = makeGovernor();
    run(governor, 5.0f, 33.3f, 3.5f, 6.7f); // the soak's minute 19 on the Fold5: frames late, neither busy
    CHECK(governor.output().renderScale == doctest::Approx(1.0f));
    CHECK(governor.output().loadLevel == 0);
    CHECK(governor.scaleChanges() == 0);
}

TEST_CASE("Quality governor: loading frames teach it nothing; off, it is the settings as they are") {
    QualityGovernor governor = makeGovernor();
    run(governor, 5.0f, 80.0f, 70.0f, 10.0f, kNaN, -1, false);
    CHECK(governor.output().renderScale == doctest::Approx(1.0f));
    CHECK(governor.output().loadLevel == 0);

    QualityGovernor off = makeGovernor(0.85f, false);
    run(off, 10.0f, 25.0f, 5.0f, 15.0f, 1.2f, 4);
    CHECK(off.output().renderScale == doctest::Approx(0.85f));
    CHECK(off.output().distanceScale == doctest::Approx(1.0f));
    CHECK(off.output().frameInterval == 1);
}

TEST_CASE("Quality governor: never above the player's resolution, never below the floor") {
    QualityGovernor governor = makeGovernor(0.7f);
    run(governor, 60.0f);
    CHECK(governor.output().renderScale == doctest::Approx(0.7f));
    run(governor, 30.0f, 25.0f, 5.0f, 15.0f);
    CHECK(governor.output().renderScale == doctest::Approx(0.5f));
}

TEST_CASE("Quality presets: each knob set, and a knob moved alone makes the choice custom") {
    CHECK(matchingPreset(qualityPreset(QualityPreset::Low)) == QualityPreset::Low);
    CHECK(matchingPreset(qualityPreset(QualityPreset::Medium)) == QualityPreset::Medium);
    CHECK(matchingPreset(qualityPreset(QualityPreset::High)) == QualityPreset::High);
    CHECK(matchingPreset(qualityPreset(QualityPreset::Ultra)) == QualityPreset::Ultra);
    SettingsView settings; // the defaults are High
    CHECK(currentQualityPreset(settings) == static_cast<u32>(QualityPreset::High));
    applyQualityPreset(settings, static_cast<u32>(QualityPreset::Low));
    CHECK(settings.renderScale == doctest::Approx(0.7f));
    CHECK(settings.viewDistance == doctest::Approx(400.0f));
    CHECK(settings.graphicsQuality == static_cast<u32>(QualityPreset::Low));
    settings.viewDistance = 450.0f;
    CHECK(currentQualityPreset(settings) == static_cast<u32>(QualityPreset::Custom));
    CHECK(std::string(qualityPresetKey(QualityPreset::Low)) == "LOW");
    CHECK(std::string(qualityPresetKey(QualityPreset::Custom)) == "VD_QUALITY_CUSTOM");
}

TEST_CASE("Settings file: what is written reads back the same; bad values clamp, unknown keys are ignored") {
    SettingsView written;
    written.uiScale = 1.25f;
    written.invertCamera = true;
    written.viewDistance = 950.0f;
    written.musicVolume = 0.3f;
    written.minimapZoom = 5;
    written.enemyPlates = true;
    written.graphicsQuality = static_cast<u32>(QualityPreset::Custom);
    written.renderScale = 0.75f;
    written.environmentDetail = 1.2f;
    written.spellDetail = 0.5f;
    written.dynamicQuality = false;
    written.limitFps = true;
    const std::string text = formatSettings(written);
    SettingsView read;
    parseSettings(text, read);
    CHECK(read == written);

    SettingsView odd;
    parseSettings("# comment\nview_distance = 99999\nrender_scale=0.1\n  ui_scale =  1.1  \r\nnot_a_setting = 3\n"
                  "music_volume = loud\n",
                  odd);
    CHECK(odd.viewDistance == doctest::Approx(1600.0f));
    CHECK(odd.renderScale == doctest::Approx(0.5f));
    CHECK(odd.uiScale == doctest::Approx(1.1f));
    CHECK(odd.musicVolume == doctest::Approx(SettingsView{}.musicVolume)); // unreadable: kept
}
