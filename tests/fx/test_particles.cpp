#include "core/Hash.h"
#include "fx/EffectSet.h"
#include "fx/ParticleMath.h"
#include "fx/ParticleSystem.h"
#include "pack/ActorFormat.h"
#include "pack/ModelFormat.h"
#include "pack/ParticleFormat.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u8;

// A baked doodad blob (pack::ModelHeader) with only effects: the emitters given, no tracks.
struct EffectBlob {
    std::vector<u8> bytes;
    vd::fx::EffectSet set;

    explicit EffectBlob(const std::vector<vd::pack::ParticleEmitter>& emitters, f32 reach = 10.0f) {
        vd::pack::ModelHeader header{};
        header.version = vd::pack::kModelFormatVersion;
        header.kind = static_cast<u32>(vd::pack::ModelKind::M2);
        const u32 emittersOffset = 256;
        const u32 effectsOffset =
            emittersOffset +
            static_cast<u32>((emitters.size() * sizeof(vd::pack::ParticleEmitter) + 15) / 16 * 16);
        header.effectsOffset = effectsOffset;
        vd::pack::EffectsHeader effects{};
        effects.emitterCount = static_cast<u32>(emitters.size());
        effects.emittersOffset = emittersOffset;
        effects.reach = reach;
        bytes.assign(effectsOffset + sizeof(effects), 0);
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (!emitters.empty()) {
            std::memcpy(bytes.data() + emittersOffset, emitters.data(),
                        emitters.size() * sizeof(emitters[0]));
        }
        std::memcpy(bytes.data() + effectsOffset, &effects, sizeof(effects));
        REQUIRE(set.bindModel(bytes.data(), bytes.size()).ok());
    }
};

// A torch-like emitter: a plane spitting particles straight up for one second.
vd::pack::ParticleEmitter torch() {
    vd::pack::ParticleEmitter e{};
    e.blend = static_cast<u8>(vd::pack::ModelBlend::Add);
    e.shape = static_cast<u8>(vd::pack::EmitterShape::Plane);
    e.rows = 1;
    e.columns = 1;
    const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(e.transform, identity, sizeof(identity));
    for (u32& t : e.tracks) {
        t = vd::pack::kEffectNoTrack;
    }
    e.values[vd::pack::kEmitterSpeed] = 2.0f;
    e.values[vd::pack::kEmitterSpeedVariation] = 0.2f;
    e.values[vd::pack::kEmitterVerticalRange] = 0.3f;
    e.values[vd::pack::kEmitterHorizontalRange] = 6.28f;
    e.values[vd::pack::kEmitterLifespan] = 1.0f;
    e.values[vd::pack::kEmitterRate] = 20.0f;
    e.values[vd::pack::kEmitterAreaLength] = 0.5f;
    e.values[vd::pack::kEmitterAreaWidth] = 0.5f;
    e.values[vd::pack::kEmitterEnabled] = 1.0f;
    e.midPoint = 0.5f;
    e.colors[0] = 0x80FF8000u; // RGBA bytes: 00 80 FF 80
    e.colors[1] = 0xFFFFFFFFu;
    e.colors[2] = 0x00000000u;
    e.scales[0] = 0.2f;
    e.scales[1] = 0.4f;
    e.scales[2] = 0.1f;
    e.headCells[2] = 1;
    e.headCells[5] = 1;
    e.twinklePercent = 1.0f;
    e.twinkleMin = 1.0f;
    e.twinkleMax = 1.0f;
    e.reach = 5.0f;
    return e;
}

// Looking north from 10 yards south of the origin, everything in view.
vd::fx::ParticleView viewOf(const glm::dvec3& camera = glm::dvec3(-10.0, 0.0, 0.0)) {
    vd::fx::ParticleView view;
    view.camera = camera;
    view.forward = glm::vec3(1.0f, 0.0f, 0.0f);
    view.right = glm::vec3(0.0f, -1.0f, 0.0f);
    view.up = glm::vec3(0.0f, 0.0f, 1.0f);
    for (glm::vec4& plane : view.frustum) {
        plane = glm::vec4(0.0f, 0.0f, 0.0f, 1e6f); // accepts everything
    }
    view.pixelsPerYard = 1000.0f;
    return view;
}

vd::fx::ParticleSystem::Settings smallSettings() {
    vd::fx::ParticleSystem::Settings settings;
    settings.maxInstances = 64;
    settings.maxEmitters = 256;
    settings.maxRibbons = 8;
    settings.workers = false;
    settings.budgetMs = 1000.0f; // no adaptive scaling in tests
    return settings;
}

} // namespace

TEST_CASE("Particle look over its life: three points around the middle") {
    CHECK(vd::fx::lifeRamp(0.0f, 0.5f, 1.0f, 3.0f, 5.0f) == doctest::Approx(1.0f));
    CHECK(vd::fx::lifeRamp(0.25f, 0.5f, 1.0f, 3.0f, 5.0f) == doctest::Approx(2.0f));
    CHECK(vd::fx::lifeRamp(0.5f, 0.5f, 1.0f, 3.0f, 5.0f) == doctest::Approx(3.0f));
    CHECK(vd::fx::lifeRamp(0.75f, 0.5f, 1.0f, 3.0f, 5.0f) == doctest::Approx(4.0f));
    CHECK(vd::fx::lifeRamp(1.0f, 0.5f, 1.0f, 3.0f, 5.0f) == doctest::Approx(5.0f));
    // An early middle point: most of the life goes to the second ramp.
    CHECK(vd::fx::lifeRamp(0.1f, 0.2f, 0.0f, 1.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(vd::fx::lifeRamp(0.6f, 0.2f, 0.0f, 1.0f, 0.0f) == doctest::Approx(0.5f));
    // Degenerate middles do not divide by zero.
    CHECK(vd::fx::lifeRamp(0.0f, 0.0f, 1.0f, 2.0f, 3.0f) == doctest::Approx(2.0f));
    CHECK(vd::fx::lifeRamp(1.0f, 1.0f, 1.0f, 2.0f, 3.0f) == doctest::Approx(2.0f));
}

TEST_CASE("Particle texture cells: the 4x4 flame of the torches") {
    // GeneralTorch01: cells 0-7 over the first half, 8-16 over the second (4 x 4 cells).
    vd::pack::ParticleEmitter e = torch();
    e.rows = 4;
    e.columns = 4;
    const u16 cells[6] = {0, 7, 1, 8, 16, 1};
    std::memcpy(e.headCells, cells, sizeof(cells));
    CHECK(vd::fx::headCell(e, 0.0f, 16) == 0);
    CHECK(vd::fx::headCell(e, 0.25f, 16) == 3);
    CHECK(vd::fx::headCell(e, 0.49f, 16) == 6);
    CHECK(vd::fx::headCell(e, 0.5f, 16) == 8);
    CHECK(vd::fx::headCell(e, 0.75f, 16) == 12);
    CHECK(vd::fx::headCell(e, 0.999f, 16) == 15);
    CHECK(vd::fx::headCell(e, 1.0f, 16) == 8); // the cycle wraps exactly at death (never drawn: age < life)
    // Repeats: twice through 0..3 over the first half.
    const u16 repeat[6] = {0, 4, 2, 0, 0, 1};
    std::memcpy(e.headCells, repeat, sizeof(repeat));
    CHECK(vd::fx::headCell(e, 0.125f, 16) == 2);
    CHECK(vd::fx::headCell(e, 0.25f, 16) == 0);
    CHECK(vd::fx::headCell(e, 0.375f, 16) == 2);
    // Out of range cells are clamped.
    const u16 wild[6] = {40, 60, 1, 40, 60, 1};
    std::memcpy(e.headCells, wild, sizeof(wild));
    CHECK(vd::fx::headCell(e, 0.3f, 16) == 15);
}

TEST_CASE("Texture coordinate animation matrices") {
    auto apply = [](const vd::UvTransform& m, f32 u, f32 v) {
        return glm::vec2(m.rows[0] * u + m.rows[1] * v + m.rows[2],
                         m.rows[4] * u + m.rows[5] * v + m.rows[6]);
    };
    const vd::UvTransform identity =
        vd::fx::uvTransform(glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
    CHECK(apply(identity, 0.3f, 0.7f).x == doctest::Approx(0.3f));
    CHECK(apply(identity, 0.3f, 0.7f).y == doctest::Approx(0.7f));
    // Scrolling water: a translation.
    const vd::UvTransform scroll =
        vd::fx::uvTransform(glm::vec3(0.25f, -0.5f, 0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
    CHECK(apply(scroll, 0.0f, 0.0f).x == doctest::Approx(0.25f));
    CHECK(apply(scroll, 0.0f, 0.0f).y == doctest::Approx(-0.5f));
    // A swirl: a quarter turn about the texture's center keeps the center, turns the corners.
    const glm::quat quarter = glm::angleAxis(1.5707963f, glm::vec3(0.0f, 0.0f, 1.0f));
    const vd::UvTransform swirl = vd::fx::uvTransform(glm::vec3(0.0f), quarter, glm::vec3(1.0f));
    CHECK(apply(swirl, 0.5f, 0.5f).x == doctest::Approx(0.5f));
    CHECK(apply(swirl, 0.5f, 0.5f).y == doctest::Approx(0.5f));
    CHECK(apply(swirl, 1.0f, 0.5f).x == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(apply(swirl, 1.0f, 0.5f).y == doctest::Approx(1.0f).epsilon(1e-4));
    // Scaled about the center.
    const vd::UvTransform zoom = vd::fx::uvTransform(glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(2.0f));
    CHECK(apply(zoom, 1.0f, 1.0f).x == doctest::Approx(1.5f));
    CHECK(apply(zoom, 0.5f, 0.5f).x == doctest::Approx(0.5f));
}

TEST_CASE("Particle emitters: spawning is deterministic per seed") {
    EffectBlob blob({torch()});
    vd::fx::ParticleSystem a, b, c;
    a.init(smallSettings());
    b.init(smallSettings());
    c.init(smallSettings());
    const vd::TextureHandle textures[1] = {};
    const u32 ia = a.create(blob.set, textures, 1234, false);
    const u32 ib = b.create(blob.set, textures, 1234, false);
    const u32 ic = c.create(blob.set, textures, 999, false);
    REQUIRE(ia != vd::fx::ParticleSystem::kNone);
    std::vector<vd::ParticleVertex> vertices(4 * 1024);
    std::vector<vd::ParticleDraw> draws;
    const vd::fx::ParticleView view = viewOf();
    vd::fx::Placement placement;
    for (u32 frame = 0; frame < 90; ++frame) {
        placement.globalMs = frame * 16;
        for (auto [system, instance] : {std::pair{&a, ia}, std::pair{&b, ib}, std::pair{&c, ic}}) {
            system->place(instance, placement);
            system->update(1.0f / 60.0f, view, nullptr, vertices, draws);
        }
    }
    std::vector<vd::fx::ParticleSystem::ParticleState> pa(64), pb(64), pc(64);
    const u32 na = a.debugParticles(ia, 0, pa);
    const u32 nb = b.debugParticles(ib, 0, pb);
    const u32 nc = c.debugParticles(ic, 0, pc);
    // 20 per second for one second each: about 20 alive.
    CHECK(na >= 17);
    CHECK(na <= 23);
    REQUIRE(na == nb);
    for (u32 i = 0; i < na; ++i) {
        CHECK(pa[i].position == pb[i].position);
        CHECK(pa[i].velocity == pb[i].velocity);
        CHECK(pa[i].age == pb[i].age);
    }
    bool differs = na != nc;
    for (u32 i = 0; i < std::min(na, nc) && !differs; ++i) {
        differs = pa[i].position != pc[i].position;
    }
    CHECK(differs);
    // Particles start in the 0.5 yd square and fly up and out at about 2 yd/s.
    for (u32 i = 0; i < na; ++i) {
        const f32 speed = glm::length(pa[i].velocity);
        CHECK(speed >= 1.5f);
        CHECK(speed <= 2.5f);
        CHECK(pa[i].velocity.z > 0.0f);
        CHECK(pa[i].age < pa[i].life);
        CHECK(std::fabs(pa[i].position.x) <= 0.25f + 2.5f * pa[i].age + 0.01f);
    }
    // Every alive particle is drawn as one quad, additive, in one draw.
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].blend == static_cast<u8>(vd::pack::ModelBlend::Add));
    CHECK(a.stats().particles == na);
}

TEST_CASE("Particle emitters: gravity, drag and the camera-relative quads") {
    vd::pack::ParticleEmitter e = torch();
    e.values[vd::pack::kEmitterSpeed] = 0.0f;
    e.values[vd::pack::kEmitterSpeedVariation] = 0.0f;
    e.values[vd::pack::kEmitterAreaLength] = 0.0f;
    e.values[vd::pack::kEmitterAreaWidth] = 0.0f;
    e.values[vd::pack::kEmitterGravity] = 4.0f;
    e.values[vd::pack::kEmitterLifespan] = 2.0f;
    e.values[vd::pack::kEmitterRate] = 1.0f;
    e.scales[0] = e.scales[1] = e.scales[2] = 0.5f;
    EffectBlob blob({e});
    vd::fx::ParticleSystem system;
    system.init(smallSettings());
    const vd::TextureHandle textures[1] = {};
    const u32 instance = system.create(blob.set, textures, 7, false);
    std::vector<vd::ParticleVertex> vertices(4 * 64);
    std::vector<vd::ParticleDraw> draws;
    vd::fx::Placement placement;
    placement.origin = glm::dvec3(1000.0, 2000.0, 50.0); // the world position of the doodad
    const vd::fx::ParticleView view = viewOf(glm::dvec3(990.0, 2000.0, 50.0));
    for (u32 frame = 0; frame < 75; ++frame) { // 1.25 s
        system.place(instance, placement);
        system.update(1.0f / 60.0f, view, nullptr, vertices, draws);
    }
    std::vector<vd::fx::ParticleSystem::ParticleState> particles(8);
    REQUIRE(system.debugParticles(instance, 0, particles) >= 1);
    // Fell from rest: z = -g t^2 / 2, v = -g t (anchor space, the anchor is the first placement).
    const f32 t = particles[0].age;
    CHECK(particles[0].position.z == doctest::Approx(-0.5f * 4.0f * t * t).epsilon(0.05));
    CHECK(particles[0].velocity.z == doctest::Approx(-4.0f * t).epsilon(0.05));
    // Its quad is drawn camera-relative: 10 yards ahead, facing the camera (spanning Y and Z, half size 0.5).
    REQUIRE(draws.size() == 1);
    const vd::ParticleVertex* q = vertices.data() + draws[0].firstQuad * 4;
    for (u32 v = 0; v < 4; ++v) {
        CHECK(q[v].position[0] == doctest::Approx(10.0f));
        CHECK(std::fabs(q[v].position[1]) == doctest::Approx(0.5f));
    }
    CHECK(q[0].position[2] - q[1].position[2] == doctest::Approx(1.0f));

    // Drag slows a thrown particle down.
    vd::pack::ParticleEmitter thrown = torch();
    thrown.values[vd::pack::kEmitterVerticalRange] = 0.0f;
    thrown.values[vd::pack::kEmitterSpeedVariation] = 0.0f;
    thrown.values[vd::pack::kEmitterRate] = 2.0f;
    thrown.drag = 1.0f;
    EffectBlob dragBlob({thrown});
    const u32 dragged = system.create(dragBlob.set, textures, 7, false);
    for (u32 frame = 0; frame < 40; ++frame) {
        system.place(dragged, vd::fx::Placement{});
        system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    REQUIRE(system.debugParticles(dragged, 0, particles) >= 1);
    CHECK(particles[0].velocity.z < 2.0f * std::exp(-0.5f * particles[0].age));
    CHECK(particles[0].velocity.z > 0.5f);
}

TEST_CASE("Particle budgets: per-emitter capacity, frame quads, distance and view") {
    vd::pack::ParticleEmitter flood = torch();
    flood.values[vd::pack::kEmitterRate] = 5000.0f; // asks for thousands alive
    flood.values[vd::pack::kEmitterLifespan] = 2.0f;
    EffectBlob blob({flood, torch()});
    CHECK(blob.set.capacity(0) == 512);
    CHECK(blob.set.capacity(1) >= 20);
    CHECK(blob.set.capacity(1) <= 32);

    vd::fx::ParticleSystem system;
    system.init(smallSettings());
    const vd::TextureHandle textures[2] = {};
    const u32 instance = system.create(blob.set, textures, 1, false);
    std::vector<vd::ParticleVertex> vertices(4 * 300); // room for 300 quads only
    std::vector<vd::ParticleDraw> draws;
    for (u32 frame = 0; frame < 60; ++frame) {
        system.place(instance, vd::fx::Placement{});
        const u32 quads = system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
        CHECK(quads <= 300);
    }
    std::vector<vd::fx::ParticleSystem::ParticleState> particles(1024);
    CHECK(system.debugParticles(instance, 0, particles) <= 512);
    CHECK(system.stats().clipped >= 1); // the flood does not fit: not drawn, still simulated
    for (const vd::ParticleDraw& draw : draws) {
        CHECK(draw.firstQuad + draw.quadCount <= 300);
    }

    // Beyond the draw distance, or behind the camera: frozen, nothing drawn.
    vd::fx::ParticleSystem far;
    far.init(smallSettings());
    EffectBlob single({torch()});
    const u32 a = far.create(single.set, textures, 1, false);
    far.place(a, vd::fx::Placement{});
    far.update(0.5f, viewOf(glm::dvec3(-500.0, 0.0, 0.0)), nullptr, vertices, draws);
    CHECK(far.particleCount(a) == 0);
    CHECK(far.stats().culled == 1);
    CHECK(draws.empty());
    vd::fx::ParticleView behind = viewOf();
    behind.frustum[0] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f); // only what is behind the camera (x < camera.x)
    far.place(a, vd::fx::Placement{});
    far.update(0.5f, behind, nullptr, vertices, draws);
    CHECK(far.particleCount(a) == 0);
    // Further away, fewer particles: the rate drops to minRateScale at maxDistance.
    vd::fx::ParticleSystem near, distant;
    near.init(smallSettings());
    distant.init(smallSettings());
    const u32 n = near.create(single.set, textures, 1, false);
    const u32 d = distant.create(single.set, textures, 1, false);
    for (u32 frame = 0; frame < 60; ++frame) {
        near.place(n, vd::fx::Placement{});
        near.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
        distant.place(d, vd::fx::Placement{});
        distant.update(1.0f / 60.0f, viewOf(glm::dvec3(-95.0, 0.0, 0.0)), nullptr, vertices, draws);
    }
    CHECK(distant.particleCount(d) < near.particleCount(n) / 2);
    CHECK(distant.particleCount(d) > 0);
}

TEST_CASE("Particle pools: emitters that find no room do not emit, instances stop and go away") {
    vd::pack::ParticleEmitter big = torch();
    big.values[vd::pack::kEmitterRate] =
        1000.0f; // wants a 512-particle slab: there are 16, then smaller ones
    EffectBlob blob({big});
    vd::fx::ParticleSystem system;
    vd::fx::ParticleSystem::Settings settings = smallSettings();
    constexpr u32 kCount = 4000; // more than the pools' 3648 slabs
    settings.maxInstances = kCount;
    settings.maxEmitters = kCount;
    system.init(settings);
    const vd::TextureHandle textures[1] = {};
    std::vector<u32> instances;
    for (u32 i = 0; i < kCount; ++i) {
        instances.push_back(system.create(blob.set, textures, i + 1, false));
        REQUIRE(instances.back() != vd::fx::ParticleSystem::kNone);
    }
    CHECK(system.create(blob.set, textures, 1, false) == vd::fx::ParticleSystem::kNone); // table full
    std::vector<vd::ParticleVertex> vertices(4 * 16384);
    std::vector<vd::ParticleDraw> draws;
    for (u32 frame = 0; frame < 10; ++frame) {
        for (u32 instance : instances) {
            system.place(instance, vd::fx::Placement{});
        }
        system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    // Every slab in use (big and small ones, rather than none); the rest starve, nothing breaks.
    CHECK(system.stats().starved > 0);
    CHECK(system.stats().emitters + system.stats().starved == kCount);

    // stop(): no new particles, released once the last one died.
    const u32 first = instances[0];
    REQUIRE(system.particleCount(first) > 0);
    system.stop(first);
    for (u32 frame = 0; frame < 90 && system.alive(first); ++frame) {
        system.place(first, vd::fx::Placement{});
        system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    CHECK_FALSE(system.alive(first));
    for (u32 i = 1; i < instances.size(); ++i) {
        system.destroy(instances[i]);
    }
    CHECK(system.stats().instances == 0);
    // Everything is back in the pools: a new instance emits at once.
    const u32 again = system.create(blob.set, textures, 5, false);
    system.place(again, vd::fx::Placement{});
    system.update(0.1f, viewOf(), nullptr, vertices, draws);
    CHECK(system.particleCount(again) > 0);
    CHECK(system.stats().starved == 0);
}

TEST_CASE("Particle emitters: a warm start looks like it has been running") {
    EffectBlob blob({torch()});
    vd::fx::ParticleSystem system;
    system.init(smallSettings());
    const vd::TextureHandle textures[1] = {};
    const u32 warm = system.create(blob.set, textures, 3, true);
    const u32 cold = system.create(blob.set, textures, 3, false);
    std::vector<vd::ParticleVertex> vertices(4 * 1024);
    std::vector<vd::ParticleDraw> draws;
    system.place(warm, vd::fx::Placement{});
    system.place(cold, vd::fx::Placement{});
    system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    CHECK(system.particleCount(warm) >= 15);
    CHECK(system.particleCount(cold) <= 1);
}

TEST_CASE("Particle update cost (report only)") {
    // A street of 100 torches (two emitters each, ~20 particles alive per emitter), as in a city at night.
    vd::pack::ParticleEmitter glow = torch();
    glow.scales[0] = glow.scales[1] = glow.scales[2] = 0.6f;
    EffectBlob blob({torch(), glow});
    vd::fx::ParticleSystem system;
    vd::fx::ParticleSystem::Settings settings = smallSettings();
    settings.maxInstances = 256;
    settings.maxEmitters = 512;
    system.init(settings);
    const vd::TextureHandle textures[2] = {};
    std::vector<u32> instances;
    for (u32 i = 0; i < 100; ++i) {
        instances.push_back(system.create(blob.set, textures, i + 1, true));
    }
    std::vector<vd::ParticleVertex> vertices(4 * 16384);
    std::vector<vd::ParticleDraw> draws;
    const vd::fx::ParticleView view = viewOf();
    auto frame = [&](u32 n) {
        for (u32 f = 0; f < n; ++f) {
            for (u32 i = 0; i < instances.size(); ++i) {
                vd::fx::Placement placement;
                placement.origin =
                    glm::dvec3(static_cast<double>(i % 10) * 3.0, static_cast<double>(i / 10) * 3.0, 0.0);
                system.place(instances[i], placement);
            }
            system.update(1.0f / 60.0f, view, nullptr, vertices, draws);
        }
    };
    frame(60);
    const auto start = std::chrono::steady_clock::now();
    frame(300);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 300.0;
    MESSAGE("100 torches: " << ms << " ms per frame, " << system.stats().particles << " particles, "
                            << system.stats().quads << " quads in " << system.stats().draws << " draws");
    CHECK(system.stats().particles > 2000);
}

TEST_CASE("Ribbons trail their bone; spell model paths become actors.vdpk hashes") {
    // An actor blob (pack::ActorModelHeader) with one ribbon on bone 0: 0.5 yd above and below, 20 edges per
    // second living half a second.
    vd::pack::RibbonEmitter ribbon{};
    ribbon.blend = static_cast<u8>(vd::pack::ModelBlend::Add);
    ribbon.color = 0xFF00FF00u;
    for (u32& t : ribbon.tracks) {
        t = vd::pack::kEffectNoTrack;
    }
    ribbon.values[vd::pack::kRibbonAbove] = 0.5f;
    ribbon.values[vd::pack::kRibbonBelow] = 0.5f;
    ribbon.values[vd::pack::kRibbonAlpha] = 1.0f;
    ribbon.values[vd::pack::kRibbonVisibility] = 1.0f;
    ribbon.edgesPerSecond = 20.0f;
    ribbon.edgeLifetime = 0.5f;
    vd::pack::ActorModelHeader header{};
    header.version = vd::pack::kActorFormatVersion;
    header.effectsOffset = 256;
    vd::pack::EffectsHeader effects{};
    effects.ribbonCount = 1;
    effects.ribbonsOffset = 320;
    effects.reach = 5.0f;
    std::vector<u8> bytes(320 + sizeof(ribbon), 0);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + 256, &effects, sizeof(effects));
    std::memcpy(bytes.data() + 320, &ribbon, sizeof(ribbon));
    vd::fx::EffectSet set;
    REQUIRE(set.bindActor(bytes.data(), bytes.size(), vd::anim::TrackTable{}, 1).ok());
    REQUIRE(set.ribbons().size() == 1);
    // A ribbon on a bone outside the skeleton is refused.
    vd::fx::EffectSet broken;
    vd::pack::RibbonEmitter far = ribbon;
    far.bone = 5;
    std::memcpy(bytes.data() + 320, &far, sizeof(far));
    CHECK_FALSE(broken.bindActor(bytes.data(), bytes.size(), vd::anim::TrackTable{}, 1).ok());
    std::memcpy(bytes.data() + 320, &ribbon, sizeof(ribbon));

    vd::fx::ParticleSystem system;
    system.init(smallSettings());
    const vd::TextureHandle textures[1] = {};
    const u32 instance = system.create(set, textures, 1, false);
    REQUIRE(instance != vd::fx::ParticleSystem::kNone);
    std::vector<vd::ParticleVertex> vertices(4 * 256);
    std::vector<vd::ParticleDraw> draws;
    const vd::anim::BoneMatrix bones[1] = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}};
    // The model flies along +Y at 6 yd/s for a second.
    u32 quads = 0;
    for (u32 frame = 0; frame < 60; ++frame) {
        vd::fx::Placement placement;
        placement.toWorld.rows[7] = static_cast<f32>(frame) * 0.1f;
        placement.bones = bones;
        placement.boneCount = 1;
        system.place(instance, placement);
        quads = system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    CHECK(system.stats().ribbons == 1);
    // Half a second of edges at 20 per second: about 10, the oldest dropped.
    CHECK(system.particleCount(instance) >= 9);
    CHECK(system.particleCount(instance) <= 13);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].blend == static_cast<u8>(vd::pack::ModelBlend::Add));
    // Segments run back along the path (camera 10 yd south of the origin): from y = 5.9 back about 3 yards, 1
    // yard tall.
    const vd::ParticleVertex* q = vertices.data() + draws[0].firstQuad * 4;
    f32 minY = 1e9f, maxY = -1e9f;
    for (u32 v = 0; v < draws[0].quadCount * 4; ++v) {
        if (q[v].color == 0) {
            continue; // padding
        }
        minY = std::min(minY, q[v].position[1]);
        maxY = std::max(maxY, q[v].position[1]);
        CHECK(std::fabs(q[v].position[2]) == doctest::Approx(0.5f));
        CHECK(q[v].position[0] == doctest::Approx(10.0f));
    }
    CHECK(maxY == doctest::Approx(5.9f).epsilon(0.01));
    CHECK(maxY - minY > 2.0f);
    CHECK(maxY - minY < 3.6f);
    CHECK(quads >= draws[0].quadCount);
    // Stopped: no new edges, gone once the last one is half a second old.
    system.stop(instance);
    for (u32 frame = 0; frame < 45 && system.alive(instance); ++frame) {
        vd::fx::Placement placement;
        placement.bones = bones;
        placement.boneCount = 1;
        system.place(instance, placement);
        system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    CHECK_FALSE(system.alive(instance));

    // SpellVisualEffectName.dbc spellings.
    CHECK(vd::fx::effectModelHash("Spells\\Frostbolt_Missile.mdx") ==
          vd::assetPathHash("spells/frostbolt_missile.m2"));
    CHECK(vd::fx::effectModelHash("\\spells\\HellFire_FirePuff_Caster_Base.mdx") ==
          vd::assetPathHash("spells/hellfire_firepuff_caster_base.m2"));
    CHECK(vd::fx::effectModelHash("Spells\\Holy_Missile_Med.mdl") ==
          vd::assetPathHash("spells/holy_missile_med.m2"));
    CHECK(vd::fx::effectModelHash("") == 0);
}
