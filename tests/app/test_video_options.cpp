// Phase 18 (world): the video options' anisotropic filtering and multisampling: off but on Ultra, part of the preset
// (moving one makes the choice Custom), saved and read back within their ranges.

#include "app/QualityGovernor.h"
#include "app/SettingsFile.h"

#include <doctest/doctest.h>

using namespace vd;

TEST_CASE("video options: anisotropic filtering and multisampling only on Ultra") {
    SettingsView v;
    CHECK(v.anisotropy == 1);
    CHECK(v.multisample == 1);
    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High}) {
        applyQualityPreset(v, static_cast<u32>(preset));
        CHECK(v.anisotropy == 1);
        CHECK(v.multisample == 1);
        CHECK(currentQualityPreset(v) == static_cast<u32>(preset));
    }
    applyQualityPreset(v, static_cast<u32>(QualityPreset::Ultra));
    CHECK(v.anisotropy == 4);
    CHECK(v.multisample == 4);
    CHECK(currentQualityPreset(v) == static_cast<u32>(QualityPreset::Ultra));
    v.anisotropy = 16;
    CHECK(currentQualityPreset(v) == static_cast<u32>(QualityPreset::Custom));
    applyQualityPreset(v, static_cast<u32>(QualityPreset::High));
    v.multisample = 2;
    CHECK(currentQualityPreset(v) == static_cast<u32>(QualityPreset::Custom));
}

TEST_CASE("video options: saved and read back; out of range is clamped; the GPU's limits are not saved") {
    SettingsView v;
    v.anisotropy = 8;
    v.multisample = 2;
    v.maxAnisotropy = 2;
    v.maxMultisample = 1;
    const std::string text = formatSettings(v);
    CHECK(text.find("anisotropy") != std::string::npos);
    CHECK(text.find("multisample") != std::string::npos);
    CHECK(text.find("max_anisotropy") == std::string::npos);
    SettingsView read;
    parseSettings(text, read);
    CHECK(read.anisotropy == 8);
    CHECK(read.multisample == 2);
    CHECK(read.maxAnisotropy == SettingsView{}.maxAnisotropy);
    parseSettings("anisotropy = 64\nmultisample = 0\n", read);
    CHECK(read.anisotropy == 16);
    CHECK(read.multisample == 1);
}
