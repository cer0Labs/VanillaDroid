// Phase 11 (f11/interiors): the light of interiors as pure functions (world/InteriorLighting.h): the packed light an
// instance carries, the lamps' attenuation, the light grid, the WMO fogs, the night glow and the terrain layers'
// scrolling.
#include "world/InteriorLighting.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <vector>

using namespace vd;

TEST_CASE("Interior light: packing keeps the colors and the direction") {
    InstanceLight light;
    light.ambient = glm::vec3(0.07f, 0.06f, 0.05f);
    light.direct = glm::vec3(1.0f, 0.74f, 0.49f);
    const glm::vec3 directions[] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},
                                    glm::normalize(glm::vec3(-0.3f, 0.8f, -0.5f)),
                                    glm::normalize(glm::vec3(0.6f, -0.6f, 0.2f))};
    for (const glm::vec3& direction : directions) {
        light.direction = direction;
        const std::array<u32, 2> packed = packInstanceLight(light);
        CHECK((packed[0] | packed[1]) != 0); // never mistaken for "outdoors"
        const InstanceLight back = unpackInstanceLight(packed);
        CHECK(glm::length(back.ambient - light.ambient) < 0.01f);
        CHECK(glm::length(back.direct - light.direct) < 0.01f);
        CHECK(glm::dot(back.direction, direction) > 0.995f);
    }
    // A black interior (a cellar without lamps) is still an interior.
    InstanceLight black;
    black.direction = glm::vec3(0.0f, 0.0f, 1.0f);
    black.ambient = glm::vec3(0.0f);
    const std::array<u32, 2> dark = packInstanceLight(black);
    CHECK((dark[0] | dark[1]) != 0);
}

TEST_CASE("Interior light: half from all around, half from the lamp") {
    const glm::vec3 baked(0.6f, 0.4f, 0.2f);
    const glm::vec3 ambient(0.07f, 0.06f, 0.05f);
    const InstanceLight light = interiorLight(baked, ambient, glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(light.direction.y == doctest::Approx(1.0f));
    // Facing the lamp: the whole baked light over the ambient; away from it: the fill only.
    const glm::vec3 front = light.ambient + light.direct;
    const glm::vec3 back = light.ambient;
    CHECK(front.x == doctest::Approx(ambient.x + baked.x));
    CHECK(back.x == doctest::Approx(ambient.x + baked.x * kInteriorFill));
    // No lamp: from above.
    CHECK(interiorLight(baked, ambient, glm::vec3(0.0f)).direction.z == doctest::Approx(1.0f));
}

TEST_CASE("Interior light: lamp attenuation and strength") {
    pack::ModelLight lamp{};
    lamp.color = 0xFFA0FFFFu; // RGBA (255, 255, 160): Goldshire's lanterns
    lamp.intensity = 1.0f;
    lamp.attenStart = 5.0f;
    lamp.attenEnd = 7.0f;
    CHECK(lightAttenuation(lamp, 1.0f) == doctest::Approx(1.0f));
    CHECK(lightAttenuation(lamp, 6.0f) == doctest::Approx(0.5f));
    CHECK(lightAttenuation(lamp, 9.0f) == doctest::Approx(0.0f));
    CHECK(lightStrength(lamp, 6.0f) > lightStrength(lamp, 6.5f));
    lamp.attenEnd = 0.0f; // not attenuated
    CHECK(lightAttenuation(lamp, 1000.0f) == doctest::Approx(1.0f));
}

TEST_CASE("Interior light: the grid interpolates between cell centers and clamps outside") {
    pack::ModelGroupLighting group{};
    group.gridMin[0] = 0.0f;
    group.gridMin[1] = 0.0f;
    group.gridMin[2] = 0.0f;
    group.cellSize = 2.0f;
    group.gridSize[0] = 2;
    group.gridSize[1] = 1;
    group.gridSize[2] = 1;
    group.firstCell = 1;
    const std::vector<u32> cells = {0xFFFFFFFFu, 0xFF000000u, 0xFF0000FFu}; // [pad], black, red
    CHECK(gridLight(group, cells, glm::vec3(1.0f, 1.0f, 1.0f)).x == doctest::Approx(0.0f)); // first center
    CHECK(gridLight(group, cells, glm::vec3(3.0f, 1.0f, 1.0f)).x == doctest::Approx(1.0f)); // second center
    CHECK(gridLight(group, cells, glm::vec3(2.0f, 1.0f, 1.0f)).x == doctest::Approx(0.5f)); // half way
    CHECK(gridLight(group, cells, glm::vec3(50.0f, -9.0f, 4.0f)).x == doctest::Approx(1.0f)); // clamped
    // Without a grid: the group's average.
    pack::ModelGroupLighting flat{};
    flat.average = 0xFF336699u;
    CHECK(gridLight(flat, cells, glm::vec3(0.0f)).z == doctest::Approx(0x33 / 255.0f));
}

TEST_CASE("Interior light: WMO fogs") {
    pack::ModelFog fog{};
    fog.innerRadius = 10.0f;
    fog.outerRadius = 20.0f;
    CHECK(fogWeight(fog, glm::vec3(5.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
    CHECK(fogWeight(fog, glm::vec3(15.0f, 0.0f, 0.0f)) == doctest::Approx(0.5f));
    CHECK(fogWeight(fog, glm::vec3(0.0f, 25.0f, 0.0f)) == doctest::Approx(0.0f));
}

TEST_CASE("Interior light: the camera's fog in a building, the 1.12 way") {
    // Lion's Pride Inn (GoldshireInn.wmo): fog 0 the building's, fog 1 flagged infinite with a short end.
    std::array<pack::ModelFog, 3> fogs{};
    fogs[0].end = 194.4f;
    fogs[0].startScale = 0.25f;
    fogs[0].color = 0xFF90D8FAu; // RGBA bytes fa d8 90
    fogs[0].underwaterEnd = 222.2f;
    fogs[0].underwaterStartScale = -1.0f;
    fogs[1].end = 83.3f;
    fogs[1].startScale = 0.25f;
    fogs[1].flags = pack::kModelFogInfinite;
    fogs[1].outerRadius = 3.4f;
    // A local fog: full inside 10 yards of the origin, none past 20.
    fogs[2].innerRadius = 10.0f;
    fogs[2].outerRadius = 20.0f;
    fogs[2].end = 50.0f;
    fogs[2].startScale = 0.5f;
    const std::array<u8, 4> listsInfinite{1, 0, 0, 0};
    InteriorFog fog = interiorFog(fogs, listsInfinite, glm::vec3(0.0f), false);
    CHECK(fog.end == doctest::Approx(194.4f)); // the infinite one is left out: the building's first fog
    CHECK(fog.startScale == doctest::Approx(0.25f));
    CHECK(fog.color.x == doctest::Approx(0xFA / 255.0f));
    fog = interiorFog(fogs, listsInfinite, glm::vec3(0.0f), true);
    CHECK(fog.end == doctest::Approx(222.2f));
    CHECK(fog.startScale == doctest::Approx(-1.0f));
    const std::array<u8, 4> listsLocal{2, 0, 0, 0};
    CHECK(interiorFog(fogs, listsLocal, glm::vec3(5.0f, 0.0f, 0.0f), false).end == doctest::Approx(50.0f));
    CHECK(interiorFog(fogs, listsLocal, glm::vec3(15.0f, 0.0f, 0.0f), false).end == doctest::Approx(122.2f));
    CHECK(interiorFog(fogs, listsLocal, glm::vec3(25.0f, 0.0f, 0.0f), false).end == doctest::Approx(194.4f));
}

TEST_CASE("Night glow follows the night") {
    const auto at = [](f32 hour) { return nightGlowFactor(hour * 120.0f); };
    CHECK(at(12.0f) == doctest::Approx(0.0f));
    CHECK(at(7.0f) == doctest::Approx(0.0f));
    CHECK(at(17.9f) == doctest::Approx(0.0f));
    CHECK(at(19.0f) == doctest::Approx(0.5f));
    CHECK(at(20.0f) == doctest::Approx(1.0f));
    CHECK(at(0.0f) == doctest::Approx(1.0f));
    CHECK(at(4.0f) == doctest::Approx(1.0f));
    CHECK(at(6.0f) == doctest::Approx(0.5f));
    CHECK(nightGlowFactor(2880.0f) == doctest::Approx(1.0f)); // midnight again
}

TEST_CASE("M2 light intensity loops over its keys") {
    const std::vector<pack::ModelLightKey> keys = {{0, 1.0f}, {100, 2.0f}, {200, 1.0f}};
    pack::ModelLight light{};
    light.intensity = 0.7f;
    CHECK(lightIntensity(light, keys, 50) == doctest::Approx(0.7f)); // no keys: constant
    light.firstKey = 0;
    light.keyCount = 3;
    light.loopMs = 200;
    CHECK(lightIntensity(light, keys, 50) == doctest::Approx(1.5f));
    CHECK(lightIntensity(light, keys, 100) == doctest::Approx(2.0f));
    CHECK(lightIntensity(light, keys, 250) == doctest::Approx(1.5f)); // looped
}

TEST_CASE("MCLY animation scrolls the layer along its direction") {
    CHECK(terrainLayerOffset(0x07 | 0x38, 3.0) == glm::vec2(0.0f)); // not animated
    // Direction 0, speed 7: a texture repeat every 1.6 s along +v.
    const u8 fast = 0x40 | (7 << 3);
    const glm::vec2 half = terrainLayerOffset(fast, 0.8);
    CHECK(half.x == doctest::Approx(0.0f));
    CHECK(half.y == doctest::Approx(0.5f));
    CHECK(terrainLayerOffset(fast, 1.6 + 0.4).y == doctest::Approx(0.25f).epsilon(0.001)); // wraps
    // Direction 2 (90 degrees), speed 1: a repeat every 11.2 s along -u.
    const glm::vec2 slow = terrainLayerOffset(0x40 | (1 << 3) | 2, 2.8);
    CHECK(slow.x == doctest::Approx(-0.25f));
    CHECK(slow.y == doctest::Approx(0.0f));
}
