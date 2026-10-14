// Phase 14 (bodyfx): the units' marks and shadows (world/BodyDecals): the footprint and ripple rings, their
// fades, the ripple rule of WoW.exe and the blob shadow's quad.
#include "world/BodyDecals.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

using namespace vd;

namespace {

f32 alphaOf(const ParticleVertex& v) {
    return static_cast<f32>(v.color >> 24) / 255.0f;
}

Footprint print(u32 texture, f64 x) {
    Footprint f;
    f.position = glm::dvec3(x, 0.0, 10.0);
    f.width = 0.3f;
    f.length = 0.4f;
    f.texture = texture;
    return f;
}

} // namespace

TEST_CASE("Footprints: the client's rings and fade") {
    // 0x69A425: at most half opaque (127), linear to nothing at 6 s.
    CHECK(FootprintPool::opacity(0.0f) == doctest::Approx(127.0f / 255.0f));
    CHECK(FootprintPool::opacity(3.0f) == doctest::Approx(127.0f / 255.0f));
    CHECK(FootprintPool::opacity(4.5f) == doctest::Approx(0.25f));
    CHECK(FootprintPool::opacity(6.0f) == doctest::Approx(0.0f));

    FootprintPool pool;
    for (u32 i = 0; i < 70; ++i) {
        pool.add(print(0, i), true);
    }
    CHECK(pool.alive() == FootprintPool::kPlayerSlots); // the player's ring: 64
    for (u32 i = 0; i < 600; ++i) {
        pool.add(print(1, i), false);
    }
    CHECK(pool.alive() == FootprintPool::kPlayerSlots + FootprintPool::kOtherSlots);
    // Fewer slots for the others (quality): the rest go at once.
    pool.setOtherSlots(100);
    CHECK(pool.alive() <= FootprintPool::kPlayerSlots + 100);

    // Only the prints of one texture are built, as many as fit.
    std::vector<ParticleVertex> vertices(4 * 1000);
    const glm::dvec3 camera(0.0, 0.0, 12.0);
    CHECK(pool.build(0, camera, vertices) == FootprintPool::kPlayerSlots);
    CHECK(pool.build(1, camera, std::span<ParticleVertex>(vertices.data(), 4 * 10)) == 10);
    CHECK(alphaOf(vertices[0]) == doctest::Approx(127.0f / 255.0f).epsilon(0.01));

    pool.update(5.0f);
    CHECK(pool.alive() > 0);
    pool.update(1.5f);
    CHECK(pool.alive() == 0);
    CHECK(pool.build(0, camera, vertices) == 0);
}

TEST_CASE("Footprints: a print lies on its ground, along the foot, mirrored for the right") {
    FootprintPool pool;
    Footprint f = print(0, 100.0);
    f.facing = 0.0f; // north (+X)
    pool.add(f, true);
    f.right = true;
    f.position.x = 102.0;
    pool.add(f, true);
    std::vector<ParticleVertex> v(8);
    const glm::dvec3 camera(100.0, 0.0, 10.0);
    REQUIRE(pool.build(0, camera, v) == 2);
    // (-x, +y) is the toe's left corner: half a length ahead, half a width to the left (+Y), just above the
    // ground.
    CHECK(v[0].position[0] == doctest::Approx(0.2f));
    CHECK(v[0].position[1] == doctest::Approx(0.15f));
    CHECK(v[0].position[2] > 0.0f);
    CHECK(v[0].position[2] < 0.1f);
    CHECK(v[3].position[0] == doctest::Approx(-0.2f));
    CHECK(v[3].position[1] == doctest::Approx(-0.15f));
    // The right foot's texture is mirrored.
    CHECK(v[0].uv[0] == 0);
    CHECK(v[4].uv[0] == 65535);
    CHECK(v[0].uv[1] == v[4].uv[1]);
}

TEST_CASE("Ripples: WoW.exe's rule for wakes and rings") {
    const glm::dvec3 surface(0.0, 0.0, 5.0);
    f32 next = 0.0f;
    // A human (scale 1, 2 yd tall) swimming at 4.72 yd/s in shallow water: a wake along its way, a third of a
    // yard across and growing ~1.9-2.8 yd/s, 0.6-0.7 s long, the next in 0.13 s.
    Ripple wake =
        rippleFor(RippleKind::Moving, surface, 1.0f, 1.0f, 4.72f, 1.5f, 2.0f, {0.0f, 0.0f, 0.0f}, next);
    CHECK(wake.wake);
    CHECK(wake.facing == doctest::Approx(1.0f));
    CHECK(wake.size == doctest::Approx(1.0f / 3.0f));
    CHECK(wake.life == doctest::Approx(0.6f));
    CHECK(wake.opacity == doctest::Approx(1.0f));
    CHECK(wake.growth == doctest::Approx(4.72f / 2.5f));
    CHECK(next == doctest::Approx(0.25f * 2.5f / 4.72f));
    // Standing: small faint rings, slowly growing, every 0.4-0.45 s.
    Ripple ring =
        rippleFor(RippleKind::Idle, surface, 1.0f, 1.0f, 0.0f, 1.5f, 2.0f, {0.5f, 1.0f, 1.0f}, next);
    CHECK_FALSE(ring.wake);
    CHECK(ring.size == doctest::Approx(1.0f / 3.0f * 0.6f));
    CHECK(ring.opacity == doctest::Approx(0.8f));
    CHECK(ring.growth == doctest::Approx(1.5f * 0.25f));
    CHECK(next == doctest::Approx(0.45f));
    // Deeper than its height: smaller and fainter, down to half at twice its height.
    Ripple deep =
        rippleFor(RippleKind::Enter, surface, 0.0f, 3.0f, 0.0f, 3.0f, 2.0f, {0.5f, 0.0f, 0.0f}, next);
    CHECK(deep.opacity == doctest::Approx(0.75f));
    CHECK(deep.size == doctest::Approx(1.0f * 0.75f));
    CHECK(makesRipples(3.9f, 2.0f));
    CHECK_FALSE(makesRipples(4.1f, 2.0f));
    CHECK_FALSE(makesRipples(-0.1f, 2.0f));
    CHECK(makesRipples(0.9f, 0.2f)); // at least a yard

    // 0x68F5C0: in over 40 % of its life, out over the rest.
    Ripple r;
    r.life = 1.0f;
    r.opacity = 0.5f;
    CHECK(RipplePool::opacity(r, 0.2f) == doctest::Approx(0.25f));
    CHECK(RipplePool::opacity(r, 0.4f) == doctest::Approx(0.5f));
    CHECK(RipplePool::opacity(r, 0.7f) == doctest::Approx(0.25f));
    CHECK(RipplePool::opacity(r, 1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Ripples: rings of 32 for the player and 96 for the others, flat on the water") {
    RipplePool pool;
    Ripple r;
    r.center = glm::dvec3(10.0, 0.0, 3.0);
    r.size = 0.5f;
    r.growth = 1.0f;
    r.life = 1.0f;
    for (u32 i = 0; i < 40; ++i) {
        pool.add(r, true);
    }
    CHECK(pool.alive() == RipplePool::kPlayerSlots);
    r.wake = true;
    for (u32 i = 0; i < 100; ++i) {
        pool.add(r, false);
    }
    CHECK(pool.alive() == RipplePool::kPlayerSlots + RipplePool::kOtherSlots);
    pool.update(0.5f);
    std::vector<ParticleVertex> v(4 * 200);
    CHECK(pool.build(false, glm::dvec3(10.0, 0.0, 0.0), v) == RipplePool::kPlayerSlots);
    // Half a yard grown by 0.5 s at 1 yd/s: a yard each way, all corners at the surface.
    for (u32 i = 0; i < 4; ++i) {
        CHECK(std::abs(v[i].position[0]) == doctest::Approx(1.0f));
        CHECK(v[i].position[2] == doctest::Approx(3.02f));
    }
    CHECK(pool.build(true, glm::dvec3(10.0, 0.0, 0.0), v) == RipplePool::kOtherSlots);
    pool.update(0.6f);
    CHECK(pool.alive() == 0);
}

TEST_CASE("Blob shadow: the box of the sequence, turned and scaled, on the floor's plane") {
    ShadowQuad s;
    s.feet = glm::dvec3(100.0, 50.0, 20.0);
    s.yaw = 1.5707963f; // facing +Y
    s.scale = 2.0f;
    s.boxMin = glm::vec2(-1.0f, -0.5f);
    s.boxMax = glm::vec2(0.5f, 0.5f);
    s.floorZ = 19.9;
    s.opacity = 0.5f;
    std::array<ParticleVertex, 4> v{};
    REQUIRE(buildShadow(s, glm::dvec3(100.0, 50.0, 30.0), v));
    // The box's middle is a quarter yard behind the feet, times 2: half a yard towards -Y.
    f32 cx = 0.0f, cy = 0.0f;
    for (const ParticleVertex& c : v) {
        cx += c.position[0] * 0.25f;
        cy += c.position[1] * 0.25f;
        CHECK(c.position[2] == doctest::Approx(-10.1f + decalLift(10.1f)).epsilon(0.01));
        CHECK(alphaOf(c) == doctest::Approx(0.5f).epsilon(0.01));
    }
    CHECK(cx == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(cy == doctest::Approx(-0.5f));
    // 3 yd long along +Y, 2 yd wide along X (the quad's u runs along -Y, its v along +X).
    CHECK(std::abs(v[0].position[1] - v[2].position[1]) == doctest::Approx(3.0f));
    CHECK(std::abs(v[0].position[0] - v[1].position[0]) == doctest::Approx(2.0f));
    // On a slope the corners follow the plane.
    s.yaw = 0.0f;
    s.boxMin = glm::vec2(-1.0f);
    s.boxMax = glm::vec2(1.0f);
    s.scale = 1.0f;
    s.normal = glm::vec3(-0.6f, 0.0f, 0.8f); // rising towards +X
    REQUIRE(buildShadow(s, glm::dvec3(100.0, 50.0, 30.0), v));
    // (-x, +y) is a yard ahead of the feet, (+x, -y) a yard behind: 2 yd apart along the slope, 1.5 yd up.
    CHECK(v[0].position[2] - v[3].position[2] == doctest::Approx(1.5f));
    // Facing a diagonal, the turned box's bounding rectangle (0x6D7CB2): a 2 x 2 square becomes 2.83 x 2.83.
    s.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    s.yaw = 0.7853982f;
    REQUIRE(buildShadow(s, glm::dvec3(100.0, 50.0, 30.0), v));
    CHECK(std::abs(v[0].position[0] - v[1].position[0]) == doctest::Approx(2.828f).epsilon(0.001));
    CHECK(std::abs(v[0].position[1] - v[2].position[1]) == doctest::Approx(2.828f).epsilon(0.001));
}
