#include "physics/Collision.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

using namespace vd;
using namespace vd::physics;

namespace {

constexpr u8 kWallFlag = 1;

// A 20 x 20 floor at z = 0 around the origin and a wall along x = 3 (facing -X), 0..4 high.
void buildRoom(TriangleSet& set) {
    set.reset(glm::dvec3(0.0));
    set.add({-10, -10, 0}, {10, -10, 0}, {10, 10, 0}, 0);
    set.add({-10, -10, 0}, {10, 10, 0}, {-10, 10, 0}, 0);
    set.add({3, -10, 0}, {3, 10, 0}, {3, 10, 4}, kWallFlag);
    set.add({3, -10, 0}, {3, 10, 4}, {3, -10, 4}, kWallFlag);
}

} // namespace

TEST_CASE("raycast hits two-sided triangles and reports the facing normal") {
    TriangleSet set(16);
    buildRoom(set);
    Hit hit;
    REQUIRE(raycast(set, {0, 0, 5}, {0, 0, -10}, hit));
    CHECK(hit.t == doctest::Approx(0.5f));
    CHECK(hit.normal.z == doctest::Approx(1.0f));
    REQUIRE(raycast(set, {0, 0, -5}, {0, 0, 10}, hit)); // from below: same triangle, other side
    CHECK(hit.normal.z == doctest::Approx(-1.0f));
    CHECK_FALSE(raycast(set, {20, 20, 5}, {0, 0, -10}, hit)); // outside the floor
    REQUIRE(raycast(set, {0, 0, 1}, {10, 0, 0}, hit));        // into the wall
    CHECK(hit.t == doctest::Approx(0.3f));
    CHECK(hit.normal.x == doctest::Approx(-1.0f));
    CHECK_FALSE(raycast(set, {0, 0, 1}, {10, 0, 0}, hit, kWallFlag)); // the camera's way through flagged surfaces
}

TEST_CASE("sphere sweeps stop at the first contact and slide free when leaving") {
    TriangleSet set(16);
    buildRoom(set);
    Hit hit;
    // Falling onto the floor: the center stops one radius above it.
    REQUIRE(sweepSphere(set, {0, 0, 3}, 0.5f, {0, 0, -5}, hit));
    CHECK(3.0f - 5.0f * hit.t == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(hit.normal.z == doctest::Approx(1.0f));
    // Into the wall at x = 3.
    REQUIRE(sweepSphere(set, {0, 0, 2}, 0.5f, {5, 0, 0}, hit));
    CHECK(5.0f * hit.t == doctest::Approx(2.5f).epsilon(1e-4));
    CHECK(hit.normal.x == doctest::Approx(-1.0f));
    // Resting on the floor and moving along it: the floor is not in the way.
    CHECK_FALSE(sweepSphere(set, {0, 0, 0.5f}, 0.5f, {2, 0, 0}, hit));
    // Moving up and away from it neither.
    CHECK_FALSE(sweepSphere(set, {0, 0, 0.5f}, 0.5f, {0, 0, 1}, hit));
    // Over the top edge of the wall (4 high): the sphere at 4.3 grazes the edge.
    REQUIRE(sweepSphere(set, {0, 0, 4.3f}, 0.5f, {6, 0, 0}, hit));
    CHECK(hit.point.x == doctest::Approx(3.0f));
    CHECK(hit.point.z == doctest::Approx(4.0f));
    CHECK(hit.normal.z > 0.0f); // pushed up and back, off the edge
    // Well above the wall: free.
    CHECK_FALSE(sweepSphere(set, {0, 0, 4.6f}, 0.5f, {6, 0, 0}, hit));
}

TEST_CASE("depenetration pushes a sphere out of the surfaces it overlaps") {
    TriangleSet set(16);
    buildRoom(set);
    glm::vec3 center(2.8f, 0.0f, 0.3f); // into the floor and the wall
    const glm::vec3 push = depenetrateSphere(set, center, 0.5f);
    CHECK(glm::length(push) > 0.0f);
    CHECK(center.z >= 0.5f - 1e-3f);
    CHECK(center.x <= 2.5f + 1e-3f);
}

TEST_CASE("closest point on a triangle covers faces, edges and corners") {
    const Triangle t{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0};
    CHECK(closestPointOnTriangle({0.2f, 0.2f, 1.0f}, t).z == doctest::Approx(0.0f));
    CHECK(closestPointOnTriangle({0.2f, 0.2f, 1.0f}, t).x == doctest::Approx(0.2f));
    CHECK(closestPointOnTriangle({-1.0f, -1.0f, 0.0f}, t).x == doctest::Approx(0.0f));
    CHECK(closestPointOnTriangle({2.0f, 2.0f, 0.0f}, t).x == doctest::Approx(0.5f));
}
