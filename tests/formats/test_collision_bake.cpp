#include "bake/CollisionBaker.h"
#include "pack/CollisionFormat.h"
#include "physics/CharacterController.h"
#include "physics/CollisionWorld.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>

using namespace vd;

namespace {

using Vec3 = std::array<f32, 3>;

void quad(bake::TriangleSoup& soup, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, u8 tag = 0) {
    const u32 base = static_cast<u32>(soup.positions.size());
    soup.positions.insert(soup.positions.end(), {a, b, c, d});
    soup.indices.insert(soup.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    soup.tags.insert(soup.tags.end(), {tag, tag});
}

void box(bake::TriangleSoup& soup, const Vec3& min, const Vec3& max) {
    const f32 x0 = min[0], y0 = min[1], z0 = min[2], x1 = max[0], y1 = max[1], z1 = max[2];
    quad(soup, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}); // top
    quad(soup, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}); // bottom
    quad(soup, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1});
    quad(soup, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0});
    quad(soup, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0});
    quad(soup, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1});
}

// Tile 32_32 (X and Y from -533 to 0): lanes of obstacles along +X, a platform and a pool.
std::vector<u8> buildTestTile() {
    bake::CollisionTileInput input;
    input.tx = 32;
    input.ty = 32;
    bake::TriangleSoup& s = input.solid;
    quad(s, {-200, -170, 0}, {-50, -170, 0}, {-50, -50, 0}, {-200, -50, 0}); // floor
    box(s, {-150, -75, 0}, {-149, -65, 5});                                    // wall across lane y = -70
    for (u32 i = 0; i < 10; ++i) {                                             // stairs on lane y = -90
        box(s, {-180.0f + 0.5f * static_cast<f32>(i), -95, 0}, {-150, -85, 0.3f * static_cast<f32>(i + 1)});
    }
    box(s, {-180, -115, 0}, {-170, -105, 1.0f});                                        // too high to step on, lane -110
    box(s, {-180, -102, 0}, {-170, -98, 0.7f});                                         // a flower bed's border, lane -100
    box(s, {-120, -65, 0}, {-119.8f, -55, 1.1f});                                       // a fence across lane -60
    // A 46-degree corner opening west from (-80, -85): a wall along -X and one 46 degrees south of it.
    quad(s, {-90, -85, 0}, {-80, -85, 0}, {-80, -85, 5}, {-90, -85, 5});
    quad(s, {-86.95f, -92.19f, 0}, {-80, -85, 0}, {-80, -85, 5}, {-86.95f, -92.19f, 5});
    quad(s, {-180, -135, 0}, {-178, -135, 5.5f}, {-178, -125, 5.5f}, {-180, -125, 0}); // 70-degree slope, lane -130
    box(s, {-190, -155, 0}, {-180, -145, 5});                                          // platform, lane -150
    // Pool south of the floor: a ramp down to -4, then flat; water at -0.5.
    quad(s, {-200, -180, 0}, {-50, -180, 0}, {-50, -170, 0}, {-200, -170, 0});
    quad(s, {-200, -190, -4}, {-50, -190, -4}, {-50, -180, 0}, {-200, -180, 0});
    quad(s, {-200, -230, -4}, {-50, -230, -4}, {-50, -190, -4}, {-200, -190, -4});
    quad(input.liquid, {-200, -230, -0.5f}, {-50, -230, -0.5f}, {-50, -175, -0.5f}, {-200, -175, -0.5f},
         static_cast<u8>(pack::LiquidKind::River));
    return bake::bakeCollisionTile(input);
}

struct Sim {
    physics::CollisionWorld world;
    physics::CharacterController character;
    physics::CharacterController::Events seen;
    f32 airTime = 0.0f; // longest fall landed

    Sim() {
        REQUIRE(world.addTile(buildTestTile()).ok());
    }
    void run(const physics::CharacterInput& input, f32 seconds) {
        const u32 frames = static_cast<u32>(std::lround(seconds * 60.0f));
        for (u32 i = 0; i < frames; ++i) {
            character.step(world, input, 1.0f / 60.0f);
            const physics::CharacterController::Events& e = character.events();
            seen.jumped |= e.jumped;
            seen.fell |= e.fell;
            seen.startedSwim |= e.startedSwim;
            seen.stoppedSwim |= e.stoppedSwim;
            if (e.landed) {
                seen.landed = true;
                airTime = std::max(airTime, character.fallTimeMs() / 1000.0f);
            }
        }
    }
};

physics::CharacterInput forward(f32 facing) {
    physics::CharacterInput input;
    input.forward = true;
    input.facing = facing;
    return input;
}

} // namespace

TEST_CASE("collision tiles: BVH, welded vertices and ground queries") {
    const std::vector<u8> blob = buildTestTile();
    pack::CollisionTileHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.version == pack::kCollisionFormatVersion);
    CHECK(header.triangleCount > 100);
    CHECK(header.vertexCount < header.triangleCount * 3); // shared corners welded
    CHECK(header.nodeCount > 1);
    CHECK(header.liquidTriangleCount == 2);
    CHECK((header.flags & pack::kCollisionTileHasTerrain) == 0);

    physics::CollisionWorld world;
    REQUIRE(world.addTile(blob).ok());
    CHECK(world.groundHeight(-100.0, -60.0, 10.0, -10.0).value_or(-99.0) == doctest::Approx(0.0));
    CHECK(world.groundHeight(-185.0, -150.0, 10.0, -10.0).value_or(-99.0) == doctest::Approx(5.0));
    CHECK(world.groundHeight(-100.0, -200.0, 10.0, -10.0).value_or(-99.0) == doctest::Approx(-4.0));
    CHECK_FALSE(world.groundHeight(-300.0, -60.0, 10.0, -10.0).has_value());
    const std::optional<physics::LiquidSample> water = world.liquidAt(-100.0, -200.0, 10.0, -10.0);
    REQUIRE(water.has_value());
    CHECK(water->level == doctest::Approx(-0.5f));
    CHECK_FALSE(world.liquidAt(-100.0, -60.0, 10.0, -10.0).has_value());
}

TEST_CASE("character: runs at its speed, stopped by walls, climbs stairs, not steps or steep slopes") {
    Sim sim;
    sim.character.place({-190.0, -60.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 1.0f);
    CHECK(sim.character.position().x == doctest::Approx(-183.0).epsilon(1e-3)); // 7 yd/s
    CHECK(sim.character.position().z == doctest::Approx(0.0));
    CHECK((sim.character.flags() & physics::moveflag::kForward) != 0);
    CHECK_FALSE(sim.seen.fell);

    sim.character.place({-160.0, -70.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 3.0f);
    CHECK(sim.character.position().x == doctest::Approx(-150.0 - sim.character.radius).epsilon(1e-3));

    // Along the wall at an angle: slides past its end instead of sticking to it.
    sim.character.place({-160.0, -72.0, 0.0}, 0.3f);
    sim.run(forward(0.3f), 5.0f);
    CHECK(sim.character.position().y > -64.0);

    sim.character.place({-185.0, -90.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 3.0f);
    CHECK(sim.character.position().z == doctest::Approx(3.0));
    CHECK(sim.character.mode() == physics::MotionMode::Ground);

    sim.character.place({-185.0, -110.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 2.0f);
    CHECK(sim.character.position().x < -180.0);
    CHECK(sim.character.position().z == doctest::Approx(0.0));

    // A 0.7 yd threshold: under the client's step rise for a human (0.73), walked up without jumping.
    sim.character.place({-185.0, -100.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 2.0f);
    CHECK(sim.character.position().x > -178.0);
    CHECK(sim.character.position().z == doctest::Approx(0.7));
    CHECK(sim.character.mode() == physics::MotionMode::Ground);

    sim.character.place({-185.0, -130.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 2.0f);
    CHECK(sim.character.position().x < -179.5);
    CHECK(sim.character.position().z < 1.5);
}

TEST_CASE("character: falls off ledges, jumps like the client and lands") {
    Sim sim;
    sim.character.place({-185.0, -150.0, 5.0}, 0.0f);
    sim.run(forward(0.0f), 2.0f);
    CHECK(sim.seen.fell);
    CHECK(sim.seen.landed);
    CHECK(sim.character.position().z == doctest::Approx(0.0));
    CHECK(sim.airTime == doctest::Approx(std::sqrt(2.0f * 5.0f / physics::kGravity)).epsilon(0.05));

    // A standing jump: up 1.64 yards, down after ~0.825 s, on the same spot.
    Sim jump;
    jump.character.place({-100.0, -100.0, 0.0}, 0.0f);
    jump.run({}, 0.1f);
    physics::CharacterInput input;
    input.jump = true;
    f32 top = 0.0f;
    jump.character.step(jump.world, input, 1.0f / 60.0f);
    CHECK(jump.character.events().jumped);
    CHECK(jump.character.jump().zspeed == doctest::Approx(-physics::kJumpVelocity));
    CHECK((jump.character.flags() & physics::moveflag::kJumping) != 0);
    input.jump = false;
    for (u32 i = 0; i < 120 && jump.character.mode() == physics::MotionMode::Air; ++i) {
        jump.character.step(jump.world, input, 1.0f / 60.0f);
        top = std::max(top, static_cast<f32>(jump.character.position().z));
    }
    CHECK(jump.character.mode() == physics::MotionMode::Ground);
    CHECK(top == doctest::Approx(physics::kJumpVelocity * physics::kJumpVelocity / (2.0f * physics::kGravity)).epsilon(0.03));
    CHECK(jump.character.fallTimeMs() == doctest::Approx(2000.0f * physics::kJumpVelocity / physics::kGravity).epsilon(0.03));
    CHECK(jump.character.position().x == doctest::Approx(-100.0));
}

TEST_CASE("character: a jump against a fence carries it over (the momentum outlives the contact)") {
    Sim sim;
    sim.character.place({-123.0, -60.0, 0.0}, 0.0f);
    sim.run(forward(0.0f), 1.0f); // pressed against the fence
    CHECK(sim.character.position().x == doctest::Approx(-120.0 - sim.character.radius).epsilon(1e-3));
    physics::CharacterInput input = forward(0.0f);
    input.jump = true;
    sim.character.step(sim.world, input, 1.0f / 60.0f);
    REQUIRE(sim.character.events().jumped);
    CHECK(sim.character.jump().xySpeed == doctest::Approx(7.0f)); // what the client reports: its run speed
    input.jump = false;
    sim.run(input, 1.5f);
    CHECK(sim.character.position().x > -119.0); // over it
    CHECK(sim.character.mode() == physics::MotionMode::Ground);
    CHECK(sim.character.position().z == doctest::Approx(0.0));

    // A standing jump next to it (no momentum) comes down where it went up, not pushed back.
    sim.character.place({-121.0, -60.0, 0.0}, 0.0f);
    sim.run({}, 0.2f);
    input = physics::CharacterInput{};
    input.jump = true;
    sim.character.step(sim.world, input, 1.0f / 60.0f);
    sim.run({}, 1.5f);
    CHECK(sim.character.position().x == doctest::Approx(-121.0).epsilon(1e-4));
}

TEST_CASE("character: a jump into an acute corner goes up and comes down along the crease") {
    Sim sim;
    const f32 intoCorner = std::atan2(2.0f, 5.0f);
    sim.character.place({-85.0, -87.0, 0.0}, intoCorner);
    sim.run(forward(intoCorner), 1.5f); // wedged in the corner
    const glm::dvec3 wedged = sim.character.position();
    CHECK(wedged.x > -82.0);
    physics::CharacterInput input = forward(intoCorner);
    input.jump = true;
    sim.character.step(sim.world, input, 1.0f / 60.0f);
    REQUIRE(sim.character.events().jumped);
    input.jump = false;
    f64 top = 0.0;
    for (u32 i = 0; i < 90; ++i) {
        sim.run(input, 1.0f / 60.0f);
        top = std::max(top, sim.character.position().z);
    }
    CHECK(top > 1.2); // it did go up
    CHECK(sim.character.mode() == physics::MotionMode::Ground);
    CHECK(sim.character.position().z == doctest::Approx(0.0));
    CHECK(std::hypot(sim.character.position().x - wedged.x, sim.character.position().y - wedged.y) < 0.1);
}

TEST_CASE("character: swims in deep liquid and walks out of it") {
    Sim sim;
    const f32 south = -1.5707963f;
    sim.character.place({-150.0, -172.0, 0.0}, south);
    sim.run(forward(south), 4.0f);
    CHECK(sim.seen.startedSwim);
    CHECK(sim.character.mode() == physics::MotionMode::Swim);
    CHECK((sim.character.flags() & physics::moveflag::kSwimming) != 0);
    // Floating: the feet at the swim depth under the surface.
    CHECK(sim.character.position().z == doctest::Approx(-0.5 - sim.character.swimDepth()).epsilon(1e-3));
    sim.run(forward(-south), 5.0f);
    CHECK(sim.seen.stoppedSwim);
    CHECK(sim.character.mode() == physics::MotionMode::Ground);
    CHECK(sim.character.position().y > -178.0);

    // Falling into it: the fall ends in the water, and START_SWIM reports the whole fall (sqrt(2 * 12 / g) s).
    Sim fall;
    fall.character.place({-100.0, -200.0, 10.0}, south);
    fall.run({}, 2.0f);
    CHECK(fall.seen.startedSwim);
    CHECK(fall.character.mode() == physics::MotionMode::Swim);
    CHECK(fall.character.endedFallTimeMs() == doctest::Approx(1000.0f * std::sqrt(2.0f * (10.5f + fall.character.swimDepth()) / physics::kGravity)).epsilon(0.05));
}

TEST_CASE("collision tiles from terrain: heights and holes") {
    auto adt = std::make_unique<formats::Adt>();
    adt->chunks.resize(pack::kTileChunks);
    for (formats::AdtChunk& chunk : adt->chunks) {
        chunk.position[2] = 10.0f;
    }
    adt->chunks[0].holes = 0x1; // quads (0..1, 0..1) of the north-west chunk
    bake::CollisionTileInput input;
    input.tx = 31;
    input.ty = 31;
    input.adt = adt.get();
    physics::CollisionWorld world;
    REQUIRE(world.addTile(bake::bakeCollisionTile(input)).ok());
    const f64 corner = pack::tileCornerX(31);
    CHECK(world.groundHeight(corner - 20.0, corner - 20.0, 20.0, 0.0).value_or(-1.0) == doctest::Approx(10.0));
    CHECK_FALSE(world.groundHeight(corner - 1.0, corner - 1.0, 20.0, 0.0).has_value()); // in the hole
    CHECK(world.groundHeight(corner - 10.0, corner - 1.0, 20.0, 0.0).has_value());      // past it
}
