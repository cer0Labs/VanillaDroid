// Phase 14 (spellfx): emitters that emit models (M2 geometry_model) bake each model as a particle mesh: Cone
// of Cold's eight shard emitters, read from the original client (VD_WOW_DIR).
#include "anim/Skeleton.h"
#include "bake/ActorBaker.h"
#include "bake/EffectBaker.h"
#include "core/Hash.h"
#include "formats/M2.h"
#include "fx/EffectSet.h"
#include "pack/ParticleFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

using vd::u32;
using vd::u8;

// The client's models by path, parsed once (what the baker's ActorCommands does).
class MpqModels final : public vd::bake::ParticleModelSource {
public:
    explicit MpqModels(const vd::formats::MpqArchiveSet& mpq) : m_mpq(mpq) {}
    const vd::formats::M2Model* particleModel(const std::string& path) const override {
        std::string m2 = path.substr(0, path.find_last_of('.')) + ".m2";
        std::lock_guard<std::mutex> lock(m_mutex);
        auto [it, inserted] = m_models.try_emplace(m2);
        if (inserted) {
            vd::Result<std::vector<u8>> bytes = m_mpq.read(m2);
            vd::Result<vd::formats::M2Model> model =
                bytes ? vd::formats::parseM2(bytes.value()) : vd::Result<vd::formats::M2Model>(bytes.error());
            if (model) {
                it->second = std::make_unique<vd::formats::M2Model>(std::move(model.value()));
            }
        }
        return it->second.get();
    }

private:
    const vd::formats::MpqArchiveSet& m_mpq;
    mutable std::mutex m_mutex;
    mutable std::map<std::string, std::unique_ptr<vd::formats::M2Model>> m_models;
};

} // namespace

TEST_CASE("Particle meshes: Cone of Cold's shards bake as models and bind (VD_WOW_DIR)") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("SPELLS\\ConeofCold_Hand.m2");
    REQUIRE(bytes);
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model);
    u32 geometryEmitters = 0;
    for (const vd::formats::M2ParticleEmitter& e : model.value().particles) {
        geometryEmitters += e.geometryModel.empty() ? 0 : 1;
    }
    CHECK(geometryEmitters == 8);

    // Without the models (the old bake) those emitters are left out; with them they are kept, with a mesh
    // each.
    const MpqModels models(*mpq);
    vd::Result<std::vector<u8>> without = vd::bake::bakeActor(model.value());
    vd::Result<std::vector<u8>> with = vd::bake::bakeActor(model.value(), &models);
    REQUIRE(without);
    REQUIRE(with);
    // 16-byte aligned copies, as the pack reader gives them.
    std::vector<std::unique_ptr<u8[]>> keep;
    const auto bind = [&](const std::vector<u8>& blob, vd::anim::Skeleton& skeleton, vd::fx::EffectSet& set) {
        keep.emplace_back(new u8[blob.size() + 16]);
        u8* data = keep.back().get() + (16 - reinterpret_cast<uintptr_t>(keep.back().get()) % 16) % 16;
        std::memcpy(data, blob.data(), blob.size());
        REQUIRE(skeleton.bind(data, blob.size()).ok());
        REQUIRE(set.bindActor(data, blob.size(), skeleton.tracks(), skeleton.boneCount()).ok());
    };
    vd::anim::Skeleton skeletonA, skeletonB;
    vd::fx::EffectSet a, b;
    bind(without.value(), skeletonA, a);
    bind(with.value(), skeletonB, b);
    CHECK(b.emitters().size() == a.emitters().size() + 8);
    u32 meshes = 0;
    for (u32 i = 0; i < b.emitters().size(); ++i) {
        const vd::pack::ParticleMesh* mesh = b.mesh(i);
        if (!mesh) {
            continue;
        }
        ++meshes;
        CHECK(mesh->triangleCount > 0);
        CHECK(b.meshVertices(*mesh).size() == mesh->triangleCount * 3);
        CHECK(mesh->texture != 0);
        CHECK(b.emitters()[i].texture == mesh->texture); // the particle draws the model's texture
    }
    CHECK(meshes == 8);
}
