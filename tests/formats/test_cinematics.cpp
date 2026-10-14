#include "app/Cinematics.h"
#include "bake/CinematicBaker.h"
#include "formats/M2.h"
#include "pack/CinematicFormat.h"
#include "world/CinematicCamera.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstring>
#include <memory>

namespace {

using vd::f32;
using vd::u32;
using vd::u8;

struct AlignedBlob {
    std::unique_ptr<u8[]> storage;
    u8* data = nullptr;
    vd::usize size = 0;
    explicit AlignedBlob(const std::vector<u8>& bytes) : storage(new u8[bytes.size() + 16]), size(bytes.size()) {
        data = storage.get() + (16 - reinterpret_cast<uintptr_t>(storage.get()) % 16) % 16;
        std::memcpy(data, bytes.data(), bytes.size());
    }
};

} // namespace

TEST_CASE("Cinematic camera: the human intro flies from over Elwynn to just behind Northshire's starting spot") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("Cameras\\FlyByHuman.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    REQUIRE(model.value().cameras.size() == 1);
    CHECK(model.value().cameras[0].positionTangents.size() == model.value().cameras[0].position.values.size());

    vd::Result<std::vector<u8>> baked = vd::bake::bakeCinematicCamera(model.value());
    REQUIRE(baked.ok());
    const AlignedBlob blob(baked.value());
    vd::CinematicCamera camera;
    REQUIRE(camera.bind(blob.data, blob.size).ok());
    CHECK(camera.durationMs() == 87467);

    // CinematicCamera.dbc row 142: origin and facing of the human flyby.
    const glm::dvec3 origin(-8945.52, -118.79, 82.93);
    const f32 facing = 0.19f;
    const vd::CinematicView start = camera.sample(0, origin, facing);
    CHECK(start.eye.x == doctest::Approx(-8921.1).epsilon(1e-4));
    CHECK(start.eye.y == doctest::Approx(543.3).epsilon(1e-3));
    CHECK(start.eye.z == doctest::Approx(144.6).epsilon(1e-3));
    CHECK(start.fovDiagonal == doctest::Approx(0.785f).epsilon(0.01));
    // The end: 6.5 yards behind the new character's spot (-8949.95, -132.49, 83.53), looking north at eye height.
    const vd::CinematicView end = camera.sample(camera.durationMs(), origin, facing);
    CHECK(end.eye.x == doctest::Approx(-8956.4).epsilon(1e-4));
    CHECK(end.eye.y == doctest::Approx(-132.9).epsilon(1e-2));
    CHECK(end.eye.z == doctest::Approx(84.8).epsilon(1e-3));
    CHECK(end.target.x > end.eye.x + 100.0);
    CHECK(std::abs(end.roll) < 0.01f);

    // A bezier flight: smooth (no jump between two close moments) and through every key.
    glm::dvec3 previous = start.eye;
    f32 longest = 0.0f;
    for (u32 ms = 10; ms <= camera.durationMs(); ms += 10) {
        const glm::dvec3 eye = camera.sample(ms, origin, facing).eye;
        longest = std::max(longest, static_cast<f32>(glm::length(eye - previous)));
        previous = eye;
    }
    CHECK(longest < 1.0f); // never more than 100 yards per second
}

TEST_CASE("CinematicFrame: black bars cut wide screens to 2:1") {
    CHECK(vd::Cinematics::barHeight(1024.0f, 768.0f) == 0.0f);  // 4:3
    CHECK(vd::Cinematics::barHeight(2176.0f, 1812.0f) == 0.0f); // the Fold5's inner screen
    CHECK(vd::Cinematics::barHeight(1920.0f, 1080.0f) == doctest::Approx(60.0f));
    CHECK(vd::Cinematics::barHeight(2316.0f, 904.0f) == 0.0f); // wider than 2:1: the whole height
    CHECK(vd::verticalFov(1.0f, 1.0f) == doctest::Approx(2.0f * std::atan(std::tan(0.5f) / std::sqrt(2.0f))));
}
