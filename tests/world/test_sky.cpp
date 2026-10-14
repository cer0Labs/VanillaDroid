// Phase 11: the sky's curves of WoW.exe 1.12 (world/Celestial.h) and the sky models' paths
// (pack/SkyFormat.h).
#include "pack/SkyFormat.h"
#include "world/Celestial.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace vd;

namespace {

constexpr f32 hours(f32 h) {
    return h / 24.0f;
}

f32 degrees(f32 radians) {
    return radians * 57.2957795f;
}

f32 polarOf(const glm::vec3& direction) {
    return degrees(std::acos(std::clamp(direction.z, -1.0f, 1.0f)));
}

f32 azimuthOf(const glm::vec3& direction) {
    return degrees(std::atan2(direction.y, direction.x));
}

} // namespace

TEST_CASE("Celestial: the client's curves are linear between keys and wrap past midnight") {
    const std::array<celestial::Key, 3> keys{{{0.25f, 1.0f}, {0.5f, 3.0f}, {0.75f, 1.0f}}};
    CHECK(celestial::sampleDay(keys, 0.25f) == doctest::Approx(1.0f));
    CHECK(celestial::sampleDay(keys, 0.375f) == doctest::Approx(2.0f));
    CHECK(celestial::sampleDay(keys, 0.5f) == doctest::Approx(3.0f));
    // From the last key (0.75) round to the first (0.25 of the next day).
    CHECK(celestial::sampleDay(keys, 0.9f) == doctest::Approx(1.0f));
    const std::array<celestial::Key, 2> ramp{{{0.25f, 0.0f}, {0.75f, 1.0f}}};
    CHECK(celestial::sampleDay(ramp, 0.0f) == doctest::Approx(0.5f)); // half way from 0.75 round to 1.25
    CHECK(celestial::sampleDay(ramp, 1.5f) == doctest::Approx(celestial::sampleDay(ramp, 1.0f))); // clamped
}

TEST_CASE("Celestial: the light comes from the north-west, 37 degrees high at noon and 20 at dawn") {
    const glm::vec3 noon = celestial::lightDirection(0.5f);
    CHECK(glm::length(noon) == doctest::Approx(1.0f));
    CHECK(noon.x == doctest::Approx(0.56472f).epsilon(0.001));
    CHECK(noon.y == doctest::Approx(0.56472f).epsilon(0.001));
    CHECK(noon.z == doctest::Approx(0.60182f).epsilon(0.001));
    CHECK(celestial::lightDirection(0.0f).z ==
          doctest::Approx(0.60182f).epsilon(0.001)); // the moon's, same place
    const glm::vec3 dawn = celestial::lightDirection(0.25f);
    CHECK(dawn.x == doctest::Approx(0.66446f).epsilon(0.001));
    CHECK(dawn.z == doctest::Approx(0.34202f).epsilon(0.001));
    CHECK(celestial::lightDirection(0.75f).z == doctest::Approx(0.34202f).epsilon(0.001));
}

TEST_CASE("Celestial: the sun rises and sets in the north-west, nearly overhead at noon") {
    const celestial::Body noon = celestial::sun(0.5f);
    CHECK(polarOf(noon.direction) == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(azimuthOf(noon.direction) == doctest::Approx(45.0f).epsilon(0.001));
    CHECK(noon.size == doctest::Approx(1.0f));
    CHECK(polarOf(celestial::sun(hours(5.5f)).direction) == doctest::Approx(100.0f).epsilon(0.001));
    CHECK(polarOf(celestial::sun(hours(2.0f)).direction) ==
          doctest::Approx(100.0f).epsilon(0.001)); // all night
    // Up between 06:00 and 06:30, down between 20:00 and 21:00.
    CHECK(celestial::sun(hours(6.0f)).direction.z < 0.0f);
    CHECK(celestial::sun(hours(6.5f)).direction.z > 0.0f);
    CHECK(celestial::sun(hours(20.0f)).direction.z > 0.0f);
    CHECK(celestial::sun(hours(21.0f)).direction.z < 0.0f);
    // Twice as big near the horizon.
    CHECK(celestial::sun(hours(6.0f)).size == doctest::Approx(2.0f));
    CHECK(celestial::sun(hours(6.75f)).size == doctest::Approx(1.0f));
    CHECK(celestial::sun(hours(21.0f)).size == doctest::Approx(2.0f));
}

TEST_CASE("Celestial: the White Lady at midnight, the Blue Child on its 1.7-day cycle") {
    const celestial::Body lady = celestial::whiteLady(0.0f);
    CHECK(polarOf(lady.direction) == doctest::Approx(35.0f).epsilon(0.001));
    CHECK(azimuthOf(lady.direction) == doctest::Approx(45.0f).epsilon(0.001));
    CHECK(lady.size == doctest::Approx(1.75f));
    CHECK(celestial::whiteLady(0.5f).direction.z < 0.0f);                     // below the horizon by day
    CHECK(celestial::whiteLady(hours(4.0f)).size == doctest::Approx(2.625f)); // 1.5 x 1.75 when low
    CHECK(celestial::whiteLady(hours(23.0f)).direction.z > 0.0f);
    CHECK(celestial::whiteLady(hours(3.0f)).direction.z > 0.0f);
    CHECK(celestial::whiteLady(hours(4.0f)).direction.z < 0.0f);

    const celestial::Body child = celestial::blueChild(0.0f, 0);
    CHECK(polarOf(child.direction) == doctest::Approx(35.0f).epsilon(0.001));
    CHECK(azimuthOf(child.direction) == doctest::Approx(135.0f).epsilon(0.001));
    CHECK(child.size == doctest::Approx(1.0f));
    CHECK(celestial::blueChild(0.0f, 1).direction.z < 0.0f);      // 1 day into its 1.7: down
    const celestial::Body again = celestial::blueChild(0.0f, 17); // 10 cycles later
    CHECK(again.direction.x == doctest::Approx(child.direction.x).epsilon(0.001));
    CHECK(again.direction.z == doctest::Approx(child.direction.z).epsilon(0.001));
}

TEST_CASE("Celestial: the stars' and the glares' fades over the day") {
    CHECK(celestial::starsAlpha(0.0f) == doctest::Approx(1.0f));
    CHECK(celestial::starsAlpha(hours(3.0f)) == doctest::Approx(1.0f));
    CHECK(celestial::starsAlpha(hours(3.75f)) == doctest::Approx(0.5f));
    CHECK(celestial::starsAlpha(0.5f) == doctest::Approx(0.0f));
    CHECK(celestial::starsAlpha(hours(23.25f)) == doctest::Approx(0.5f));

    CHECK(celestial::sunGlareFade(hours(6.5f)) == doctest::Approx(0.0f));
    CHECK(celestial::sunGlareFade(hours(7.0f)) == doctest::Approx(0.5f));
    CHECK(celestial::sunGlareFade(0.5f) == doctest::Approx(1.0f));
    CHECK(celestial::sunGlareFade(hours(20.25f)) == doctest::Approx(0.5f));
    CHECK(celestial::sunGlareFade(0.0f) == doctest::Approx(0.0f));
    CHECK(celestial::moonGlareFade(0.0f) == doctest::Approx(1.0f));
    CHECK(celestial::moonGlareFade(0.5f) == doctest::Approx(0.0f));
    CHECK(celestial::moonGlareFade(hours(2.625f)) == doctest::Approx(0.5f));

    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    CHECK(celestial::glareFacing(forward, forward) == doctest::Approx(1.0f));
    CHECK(celestial::glareFacing(forward, glm::vec3(0.0f, 1.0f, 0.0f)) == doctest::Approx(0.0f));
    CHECK(celestial::glareFacing(forward,
                                 glm::normalize(glm::vec3(0.85f, std::sqrt(1.0f - 0.85f * 0.85f), 0.0f))) ==
          doctest::Approx(0.5f).epsilon(0.001));
    CHECK(celestial::kSunGlare.scaleAt == doctest::Approx(20.0f));
    CHECK(celestial::kMoonGlare.bodyScale == doctest::Approx(2.0f));
}

TEST_CASE("Celestial: the clouds' light and the server's date") {
    CHECK(celestial::cloudsLitBySun(0.5f));
    CHECK_FALSE(celestial::cloudsLitBySun(0.0f));
    CHECK(celestial::cloudLightFade(0.5f) == doctest::Approx(1.0f));
    CHECK(celestial::cloudLightFade(0.0f) == doctest::Approx(1.0f));
    CHECK(celestial::cloudLightFade(hours(4.75f)) == doctest::Approx(0.0f));

    CHECK(celestial::daysSince2000(0, 0, 0) == 0);
    CHECK(celestial::daysSince2000(0, 1, 0) == 31);
    CHECK(celestial::daysSince2000(1, 0, 0) == 366);    // 2000 was a leap year
    CHECK(celestial::daysSince2000(4, 10, 22) == 1788); // 2004-11-23
    CHECK(celestial::daysSince2000(26, 9, 1) == 9771);  // 2026-10-02
}

TEST_CASE("Sky models: their path in models.vdpk from LightSkybox.dbc and MOSB references") {
    CHECK(pack::skyModelPath("Environments\\Stars\\DeathClouds.mdx") ==
          "sky/environments/stars/deathclouds.m2");
    CHECK(pack::skyModelPath("ENVIRONMENTS\\STARS\\STRATHOLMESKYBOX.MDX") ==
          "sky/environments/stars/stratholmeskybox.m2");
    CHECK(pack::skyModelPath("Environments\\Stars\\stars.mdl") == "sky/environments/stars/stars.m2");
    CHECK(pack::skyModelPath("Environments\\Stars\\Stars.m2") == "sky/environments/stars/stars.m2");
    CHECK(pack::skyModelPath("THE TEMPLE OF ATAL'HAKKAR").empty()); // AZ_SunkenTemple's MOSB
    CHECK(pack::skyModelPath("").empty());
}
