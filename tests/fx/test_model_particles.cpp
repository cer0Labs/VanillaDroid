// Phase 14 (spellfx): particles that are models (M2 geometry_model: Cone of Cold's shards, Cyclone's rocks):
// the baked mesh section of an effect set and the triangles the particle system writes for each particle.
#include "fx/EffectSet.h"
#include "fx/ParticleSystem.h"
#include "pack/ModelFormat.h"
#include "pack/ParticleFormat.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cstring>
#include <vector>

namespace {

using vd::f32;
using vd::u32;
using vd::u8;

// A doodad blob (pack::ModelHeader) with one emitter of a one-triangle model.
struct MeshBlob {
    std::vector<u8> bytes;
    vd::fx::EffectSet set;

    MeshBlob(const vd::pack::ParticleEmitter& emitter, const vd::pack::ParticleMesh& mesh,
             const std::vector<vd::pack::ParticleMeshVertex>& corners) {
        vd::pack::ModelHeader header{};
        header.version = vd::pack::kModelFormatVersion;
        header.kind = static_cast<u32>(vd::pack::ModelKind::M2);
        const u32 emittersOffset = 256;
        const u32 meshesOffset = emittersOffset + 304; // 296 rounded to 16
        const u32 cornersOffset = meshesOffset + 48;
        const u32 effectsOffset = cornersOffset + static_cast<u32>((corners.size() * 20 + 15) / 16 * 16);
        header.effectsOffset = effectsOffset;
        vd::pack::EffectsHeader effects{};
        effects.emitterCount = 1;
        effects.emittersOffset = emittersOffset;
        effects.reach = 10.0f;
        effects.meshCount = 1;
        effects.meshesOffset = meshesOffset;
        effects.meshVerticesOffset = cornersOffset;
        bytes.assign(effectsOffset + sizeof(effects), 0);
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + emittersOffset, &emitter, sizeof(emitter));
        std::memcpy(bytes.data() + meshesOffset, &mesh, sizeof(mesh));
        std::memcpy(bytes.data() + cornersOffset, corners.data(), corners.size() * sizeof(corners[0]));
        std::memcpy(bytes.data() + effectsOffset, &effects, sizeof(effects));
    }
};

vd::pack::ParticleEmitter shards() {
    vd::pack::ParticleEmitter e{};
    e.blend = static_cast<u8>(vd::pack::ModelBlend::Alpha);
    e.shape = static_cast<u8>(vd::pack::EmitterShape::Plane);
    e.rows = 1;
    e.columns = 1;
    const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(e.transform, identity, sizeof(identity));
    for (u32& t : e.tracks) {
        t = vd::pack::kEffectNoTrack;
    }
    e.values[vd::pack::kEmitterLifespan] = 2.0f;
    e.values[vd::pack::kEmitterRate] = 4.0f;
    e.values[vd::pack::kEmitterEnabled] = 1.0f;
    e.midPoint = 0.5f;
    e.colors[0] = e.colors[1] = e.colors[2] = 0xFF8080FFu;
    e.scales[0] = e.scales[1] = e.scales[2] = 2.0f; // the model twice its size
    e.twinklePercent = 1.0f;
    e.twinkleMin = e.twinkleMax = 1.0f;
    e.reach = 5.0f;
    e.mesh = 1;
    return e;
}

vd::fx::ParticleView viewOf() {
    vd::fx::ParticleView view;
    view.camera = glm::dvec3(-10.0, 0.0, 0.0);
    view.forward = glm::vec3(1.0f, 0.0f, 0.0f);
    view.right = glm::vec3(0.0f, -1.0f, 0.0f);
    view.up = glm::vec3(0.0f, 0.0f, 1.0f);
    for (glm::vec4& plane : view.frustum) {
        plane = glm::vec4(0.0f, 0.0f, 0.0f, 1e6f);
    }
    return view;
}

} // namespace

TEST_CASE("Particles of models: each particle is the model's triangles, sized, colored and tumbling") {
    vd::pack::ParticleMesh mesh{};
    mesh.texture = 77;
    mesh.firstVertex = 0;
    mesh.triangleCount = 1;
    mesh.tumbleMin[2] = mesh.tumbleMax[2] = 3.0f; // turns about z at 3 rad/s
    mesh.blend = static_cast<u8>(vd::pack::ModelBlend::Add);
    const std::vector<vd::pack::ParticleMeshVertex> corners = {{{0.5f, 0.0f, 0.0f}, {0.0f, 0.0f}},
                                                               {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f}},
                                                               {{0.0f, 0.0f, 0.5f}, {0.0f, 1.0f}}};
    MeshBlob blob(shards(), mesh, corners);
    REQUIRE(blob.set.bindModel(blob.bytes.data(), blob.bytes.size()).ok());
    REQUIRE(blob.set.mesh(0));
    CHECK(blob.set.mesh(0)->triangleCount == 1);
    CHECK(blob.set.meshVertices(*blob.set.mesh(0)).size() == 3);
    CHECK(blob.set.capacity(0) <= 64); // models are capped harder than quads

    vd::fx::ParticleSystem::Settings settings;
    settings.maxInstances = 8;
    settings.maxEmitters = 8;
    settings.maxRibbons = 2;
    settings.workers = false;
    settings.budgetMs = 1000.0f;
    vd::fx::ParticleSystem system;
    system.init(settings);
    const vd::TextureHandle textures[1] = {};
    const u32 instance = system.create(blob.set, textures, 11, false);
    std::vector<vd::ParticleVertex> vertices(4 * 256);
    std::vector<vd::ParticleDraw> draws;
    u32 quads = 0;
    for (u32 frame = 0; frame < 60; ++frame) {
        system.place(instance, vd::fx::Placement{});
        quads = system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws);
    }
    REQUIRE(quads > 0);
    REQUIRE_FALSE(draws.empty());
    // Every written quad is a triangle (its last corner repeated) around the emitter, 10 yards ahead, its
    // corners 2 * 0.5 = 1 yard from the particle's center at most, in the particle's color.
    u32 triangles = 0;
    for (u32 q = 0; q < quads; ++q) {
        const vd::ParticleVertex* v = vertices.data() + q * 4;
        if (v[0].color == 0) {
            continue; // an unused slot of the emitter's range
        }
        ++triangles;
        CHECK(std::memcmp(&v[2], &v[3], sizeof(v[2])) == 0);
        const glm::vec3 a(v[0].position[0], v[0].position[1], v[0].position[2]);
        const glm::vec3 b(v[1].position[0], v[1].position[1], v[1].position[2]);
        CHECK(glm::length(a - b) ==
              doctest::Approx(std::sqrt(0.5f) * 2.0f).epsilon(0.01)); // edge scaled by 2
        CHECK(a.x == doctest::Approx(10.0f).epsilon(0.15));
        CHECK(v[0].color == 0xFF8080FFu);
    }
    CHECK(triangles >= 3); // ~4 a second for a second
    // VD_MODEL_PARTICLES=0: the emitter is left out.
    system.settings().meshes = false;
    system.place(instance, vd::fx::Placement{});
    CHECK(system.update(1.0f / 60.0f, viewOf(), nullptr, vertices, draws) == 0);
}

TEST_CASE("Particles of models: a mesh index past the meshes is a corrupt blob") {
    vd::pack::ParticleMesh mesh{};
    mesh.triangleCount = 1;
    const std::vector<vd::pack::ParticleMeshVertex> corners(3);
    vd::pack::ParticleEmitter e = shards();
    e.mesh = 2;
    MeshBlob blob(e, mesh, corners);
    CHECK_FALSE(blob.set.bindModel(blob.bytes.data(), blob.bytes.size()).ok());
}
