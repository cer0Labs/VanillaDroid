// Collision that moves (phase 8): a transport's model-space collision blob (as `vd_baker bake --only objects`
// writes it) placed and turned in the world: the queries find its deck where it is now, and only there.
#include "bake/CollisionBaker.h"
#include "pack/CollisionFormat.h"
#include "physics/CharacterController.h"
#include "physics/CollisionWorld.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace vd;

namespace {

// A deck 40 yards long (model X), 8 wide (Y), its top 2 yards up, with a mast in the middle.
std::vector<u8> deckBlob() {
    bake::CollisionTileInput input; // model space: tile 0_0, no terrain
    bake::TriangleSoup& s = input.solid;
    const auto quad = [&](std::array<f32, 3> a, std::array<f32, 3> b, std::array<f32, 3> c,
                          std::array<f32, 3> d) {
        const u32 base = static_cast<u32>(s.positions.size());
        s.positions.insert(s.positions.end(), {a, b, c, d});
        s.indices.insert(s.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        s.tags.insert(s.tags.end(), {0, 0});
    };
    quad({-20, -4, 2}, {20, -4, 2}, {20, 4, 2}, {-20, 4, 2});
    quad({-0.5f, 0, 2}, {0.5f, 0, 2}, {0.5f, 0, 12}, {-0.5f, 0, 12});
    return bake::bakeCollisionTile(input);
}

} // namespace

TEST_CASE("Moving collision objects: placed, turned and moved") {
    physics::CollisionWorld world;
    const u32 ship = world.addObject(deckBlob());
    REQUIRE(ship != physics::CollisionWorld::kNoObject);
    CHECK(world.objectCount() == 1);
    CHECK(world.addObject(std::vector<u8>(16, 0)) == physics::CollisionWorld::kNoObject);
    // Not placed: nothing.
    CHECK_FALSE(world.groundHeight(1000.0, 2000.0, 100.0, -100.0));

    // At (1000, 2000, 5), along X: the deck at z 7 from x 980 to 1020.
    world.placeObject(ship, glm::dvec3(1000.0, 2000.0, 5.0), 0.0f);
    std::optional<f64> ground = world.groundHeight(1015.0, 2002.0, 20.0, -20.0);
    REQUIRE(ground);
    CHECK(*ground == doctest::Approx(7.0));
    CHECK_FALSE(world.groundHeight(1000.0, 2010.0, 20.0, -20.0)); // off its side
    f64 height = 0.0;
    CHECK(world.objectBelow(1015.0, 2002.0, 20.0, -20.0, &height) == ship);
    CHECK(height == doctest::Approx(7.0));
    CHECK(world.objectBelow(1000.0, 2010.0, 20.0, -20.0) == physics::CollisionWorld::kNoObject);

    // Turned 90 degrees (towards +Y) and moved: the deck now runs along Y.
    world.placeObject(ship, glm::dvec3(1100.0, 2000.0, 0.0), 1.5707964f);
    CHECK_FALSE(world.groundHeight(1115.0, 2000.0, 20.0, -20.0));
    ground = world.groundHeight(1100.0, 2015.0, 20.0, -20.0);
    REQUIRE(ground);
    CHECK(*ground == doctest::Approx(2.0));
    CHECK_FALSE(world.groundHeight(1015.0, 2002.0, 20.0, -20.0)); // it left

    // A character stands on it, and walks (towards -Y) into the mast.
    physics::CharacterController character;
    character.place(glm::dvec3(1100.0, 2010.0, 2.0), -1.5707964f);
    physics::CharacterInput input;
    input.facing = -1.5707964f;
    input.forward = true;
    for (u32 i = 0; i < 120; ++i) {
        character.step(world, input, 1.0f / 60.0f);
    }
    CHECK(character.mode() == physics::MotionMode::Ground);
    CHECK(character.position().z == doctest::Approx(2.0).epsilon(0.01));
    CHECK(character.position().y > 2000.0); // the mast (at y 2000) stopped it short
    CHECK(character.position().y < 2001.0);

    world.removeObject(ship);
    CHECK(world.objectCount() == 0);
    CHECK_FALSE(world.groundHeight(1100.0, 2015.0, 20.0, -20.0));
}
