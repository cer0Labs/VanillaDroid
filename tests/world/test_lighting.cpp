// Phase 11: Light.dbc's parameter sets per condition (clear, storm, underwater, death) and the bands the sky uses,
// against the client's tables in db.vdpk (VD_PACKS_DIR; skipped without it).
#include "pack/PackReader.h"
#include "world/Lighting.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace vd;

namespace {

// Goldshire: no local light reaches it, so only Azeroth's global light (LightParams 12 clear, 13 clear underwater,
// 10 storm, 11 storm underwater, 4 death) counts. Noon = 1440 half-minutes, a keyframe of every band there.
const glm::dvec3 kGoldshire(-9449.0, 64.0, 57.0);
constexpr f32 kNoon = 1440.0f;

glm::vec3 rgb(u32 color) {
    return glm::vec3(static_cast<f32>((color >> 16) & 0xFF), static_cast<f32>((color >> 8) & 0xFF),
                     static_cast<f32>(color & 0xFF)) /
           255.0f;
}

void checkColor(const glm::vec3& actual, u32 expected) {
    const glm::vec3 e = rgb(expected);
    CHECK(actual.x == doctest::Approx(e.x).epsilon(0.002));
    CHECK(actual.y == doctest::Approx(e.y).epsilon(0.002));
    CHECK(actual.z == doctest::Approx(e.z).epsilon(0.002));
}

} // namespace

TEST_CASE("Lighting: parameter sets per condition and the sky bands") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or db.vdpk missing: skipping");
        return;
    }
    WorldLighting lighting;
    REQUIRE(lighting.init(db));
    lighting.setMap(0);

    const LightingState clear = lighting.evaluate(kGoldshire, kNoon);
    checkColor(clear.fog, 0x4d788f);
    checkColor(clear.sky[0], 0x001f49);
    checkColor(clear.shadow, 0x4d4d4d);
    checkColor(clear.sun, 0xfff7de);
    checkColor(clear.sunHalo, 0xffc78a);
    checkColor(clear.cloudEmissive, 0x2b6984);
    checkColor(clear.cloudLayer2, 0x6182b7);
    CHECK(clear.fogEnd == doctest::Approx(500.0f));
    CHECK(clear.celestialGlow == doctest::Approx(1.0f));
    CHECK(clear.cloudDensity == doctest::Approx(0.5f));
    CHECK(clear.skybox == 0);

    LightConditions storm;
    storm.storm = 1.0f;
    const LightingState stormy = lighting.evaluate(kGoldshire, kNoon, storm);
    checkColor(stormy.fog, 0x525452);
    CHECK(stormy.fogEnd == doctest::Approx(10000.0f / 36.0f));
    CHECK(stormy.cloudDensity == doctest::Approx(0.95f));

    storm.storm = 0.5f; // halfway between the two sets
    const LightingState half = lighting.evaluate(kGoldshire, kNoon, storm);
    CHECK(half.fog.x == doctest::Approx((rgb(0x4d788f).x + rgb(0x525452).x) * 0.5f).epsilon(0.002));
    CHECK(half.fogEnd == doctest::Approx((500.0f + 10000.0f / 36.0f) * 0.5f));

    LightConditions underwater;
    underwater.underwater = true;
    const LightingState below = lighting.evaluate(kGoldshire, kNoon, underwater);
    checkColor(below.fog, 0x255c5c);
    CHECK(below.fogEnd == doctest::Approx(6000.0f / 36.0f));
    // Phase 11 (variants): its fog starts half a fog end behind the camera (LightFloatBand: -0.5), so the water next
    // to the camera is already a third fog; the clear set's starts a quarter of the way out.
    CHECK(below.fogStartScale == doctest::Approx(-0.5f));
    CHECK(clear.fogStartScale == doctest::Approx(0.25f));

    LightConditions dead;
    dead.dead = true;
    dead.storm = 1.0f; // the death set ignores the weather
    dead.underwater = true;
    const LightingState ghost = lighting.evaluate(kGoldshire, kNoon, dead);
    checkColor(ghost.fog, 0x00536f);
    checkColor(ghost.sky[0], 0x00536f);
    CHECK(ghost.skybox == 3);
    CHECK(lighting.skyboxModel(ghost.skybox).find("DeathClouds") != std::string_view::npos);
    CHECK(lighting.skyboxModel(0).empty());
    CHECK(lighting.skyboxModel(999).empty());
}
