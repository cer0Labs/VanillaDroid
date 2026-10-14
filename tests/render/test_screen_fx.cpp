// Phase 11: the client's full-screen effects (render/ScreenFx.h, the CPU mirror of shaders/screenfx.glsl): which are
// on, and what they do to a color, against FFXGlow.bls / FFXDeath.bls computed by hand.
#include "render/ScreenFx.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace vd;

TEST_CASE("Screen effects: which are on") {
    // Alive: the glow only with its option, weighted by LightParams.glow (Elwynn at noon: 0.65).
    ScreenFxParams p = screenFxParams(0.65f, false, true, true);
    CHECK(p.glow == doctest::Approx(0.65f));
    CHECK(p.death == 0.0f);
    p = screenFxParams(0.65f, false, false, true);
    CHECK(p.glow == 0.0f);
    CHECK(p.death == 0.0f);
    // A ghost with the death effect: its grading and its own glow (the death set's, 0.8), even without the glow
    // option; without the death effect, only what the glow option says.
    p = screenFxParams(0.8f, true, false, true);
    CHECK(p.glow == doctest::Approx(0.8f));
    CHECK(p.death == 1.0f);
    p = screenFxParams(0.8f, true, false, false);
    CHECK(p.glow == 0.0f);
    CHECK(p.death == 0.0f);
    p = screenFxParams(0.8f, true, true, false);
    CHECK(p.glow == doctest::Approx(0.8f));
    CHECK(p.death == 0.0f);
}

TEST_CASE("Screen effects: the glow and the ghost's grading") {
    // Off: nothing changes.
    const glm::vec3 grass(0.3f, 0.5f, 0.2f);
    CHECK(applyScreenFx(grass, {}) == grass);

    // FFXGlow with the scene as its own blurred image: c + w c^2.
    const glm::vec3 glowed = applyScreenFx(grass, {0.5f, 0.0f});
    CHECK(glowed.x == doctest::Approx(0.3f + 0.5f * 0.09f));
    CHECK(glowed.y == doctest::Approx(0.5f + 0.5f * 0.25f));
    CHECK(glowed.z == doctest::Approx(0.2f + 0.5f * 0.04f));

    // FFXDeath: black and white stay grey (no tint at the ends), middle tones take all of 0x5393A8.
    CHECK(applyScreenFx(glm::vec3(0.0f), {0.8f, 1.0f}) == glm::vec3(0.0f));
    const glm::vec3 white = applyScreenFx(glm::vec3(1.0f), {0.8f, 1.0f});
    CHECK(white.x == doctest::Approx(1.0f)); // the luminance saturates at 1: 4 L (1 - L) = 0
    CHECK(white.z == doctest::Approx(1.0f));
    // A middle grey whose glowed luminance is 0.5 exactly: g + 0.8 g^2 = 0.5 / 1.03 (the weights sum to 1.03).
    const f32 l = 0.5f / 1.03f;
    const f32 g = (-1.0f + std::sqrt(1.0f + 4.0f * 0.8f * l)) / (2.0f * 0.8f);
    const glm::vec3 mid = applyScreenFx(glm::vec3(g), {0.8f, 1.0f});
    CHECK(mid.x == doctest::Approx(0.5f + 83.0f / 255.0f).epsilon(0.001));
    CHECK(mid.y == doctest::Approx(0.5f + 147.0f / 255.0f).epsilon(0.001));
    CHECK(mid.z == doctest::Approx(0.5f + 168.0f / 255.0f).epsilon(0.001));
    // Any hue becomes grey plus the same tint: a red and a green of equal luminance look alike.
    const glm::vec3 red = applyScreenFx(glm::vec3(0.587f, 0.0f, 0.0f) * 0.5f, {0.0f, 1.0f});
    const glm::vec3 green = applyScreenFx(glm::vec3(0.0f, 0.299f, 0.0f) * 0.5f, {0.0f, 1.0f});
    CHECK(red.x == doctest::Approx(green.x));
    CHECK(red.y == doctest::Approx(green.y));
    CHECK(red.z == doctest::Approx(green.z));
    CHECK(red.z > red.x); // grey-blue
}
