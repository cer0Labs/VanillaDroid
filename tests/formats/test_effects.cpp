#include "anim/Skeleton.h"
#include "bake/ActorBaker.h"
#include "bake/EffectBaker.h"
#include "bake/ModelBaker.h"
#include "core/Hash.h"
#include "formats/M2.h"
#include "fx/EffectSet.h"
#include "jobs/JobSystem.h"
#include "pack/ActorFormat.h"
#include "pack/ModelFormat.h"
#include "pack/ParticleFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using vd::f32;
using vd::u32;
using vd::u8;

vd::Result<vd::formats::M2Model> readM2(const vd::formats::MpqArchiveSet& mpq, const char* path) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    return vd::formats::parseM2(bytes.value());
}

// A baked blob in 16-byte aligned memory, as the runtime gets it from the pack reader.
struct AlignedBlob {
    std::unique_ptr<u8[]> storage;
    u8* data = nullptr;
    vd::usize size = 0;
    explicit AlignedBlob(const std::vector<u8>& bytes)
        : storage(new u8[bytes.size() + 16]), size(bytes.size()) {
        data = storage.get() + (16 - reinterpret_cast<uintptr_t>(storage.get()) % 16) % 16;
        std::memcpy(data, bytes.data(), bytes.size());
    }
};

constexpr const char* kTorch = "World\\Generic\\PassiveDoodads\\Lights\\GeneralTorch01.m2";

} // namespace

TEST_CASE("M2 particle emitters (v256, 504 bytes): the street torch") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, kTorch);
    REQUIRE(model.ok());
    const vd::formats::M2Model& m = model.value();
    CHECK(m.effectsError.empty());
    REQUIRE(m.particles.size() == 2);
    // The flame: 4x4 cells of FLAMELICKSMALL, additive, 20 per second for a second, growing then shrinking.
    const vd::formats::M2ParticleEmitter& flame = m.particles[0];
    CHECK(flame.id == -1);
    CHECK(flame.bone == 1);
    CHECK(flame.blend == 4); // add
    CHECK(flame.emitterType == vd::formats::kM2EmitterPlane);
    CHECK(flame.rows == 4);
    CHECK(flame.columns == 4);
    CHECK(vd::normalizeAssetPath(m.textures[flame.texture].path) ==
          "item/objectcomponents/weapon/flamelicksmall.blp");
    REQUIRE(flame.emissionRate.values.size() >= 1);
    CHECK(flame.emissionRate.values[0] == doctest::Approx(20.0f));
    CHECK(flame.lifespan.values[0] == doctest::Approx(1.0f));
    CHECK(flame.midPoint == doctest::Approx(0.5f));
    CHECK(flame.scales[0] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(flame.scales[1] == doctest::Approx(0.28f).epsilon(0.02));
    CHECK(flame.colors[0] == 0x96FF4800u); // BGRA: orange, alpha 150
    CHECK(flame.headLifeCells[0] == 0);
    CHECK(flame.headLifeCells[1] == 7);
    CHECK(flame.headDecayCells[0] == 8);
    CHECK(flame.headDecayCells[1] == 16);
    // The glow around it.
    const vd::formats::M2ParticleEmitter& glow = m.particles[1];
    CHECK(glow.bone == 3);
    CHECK(glow.rows == 1);
    CHECK(vd::normalizeAssetPath(m.textures[glow.texture].path) ==
          "world/generic/nightelf/passive doodads/magicalimplements/glow.blp");
}

TEST_CASE("Baked doodad effects: emitters, values and bone pose survive the round trip") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, kTorch);
    REQUIRE(model.ok());
    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(model.value());
    REQUIRE(blob.ok());
    AlignedBlob aligned(blob.value());
    vd::fx::EffectSet set;
    REQUIRE(set.bindModel(aligned.data, aligned.size).ok());
    REQUIRE(set.emitters().size() == 2);
    CHECK(set.hasParticles());
    CHECK_FALSE(set.actor());
    const vd::pack::ParticleEmitter& flame = set.emitters()[0];
    CHECK(flame.texture == vd::assetPathHash("item/objectcomponents/weapon/flamelicksmall.blp"));
    CHECK(flame.blend == static_cast<u8>(vd::pack::ModelBlend::Add));
    CHECK(flame.rows == 4);
    CHECK(flame.columns == 4);
    CHECK(flame.values[vd::pack::kEmitterRate] == doctest::Approx(20.0f));
    CHECK(flame.values[vd::pack::kEmitterLifespan] == doctest::Approx(1.0f));
    CHECK(flame.values[vd::pack::kEmitterEnabled] == doctest::Approx(1.0f));
    CHECK(flame.colors[0] == 0x960048FFu); // RGBA: red ff, green 48, blue 00, alpha 96
    CHECK(flame.reach > 0.5f);
    CHECK(flame.reach < 20.0f);
    CHECK(set.capacity(0) >= 20);
    // Doodads keep the bone's pose at the start of their loop in the emitter transform: compare with the M2
    // bone.
    const std::array<f32, 12> pose =
        vd::bake::m2BonePose(model.value(), 1, 0, model.value().sequences[0].start);
    const vd::formats::Vec3& p = model.value().particles[0].position;
    for (u32 row = 0; row < 3; ++row) {
        const f32 expected =
            pose[row * 4] * p[0] + pose[row * 4 + 1] * p[1] + pose[row * 4 + 2] * p[2] + pose[row * 4 + 3];
        CHECK(flame.transform[row * 4 + 3] == doctest::Approx(expected).epsilon(1e-4));
    }
    // The flame sits on top of the torch.
    CHECK(flame.transform[11] > 0.5f);
    // Old packs are refused.
    std::vector<u8> old = blob.value();
    vd::pack::ModelHeader header;
    std::memcpy(&header, old.data(), sizeof(header));
    header.version = 4;
    std::memcpy(old.data(), &header, sizeof(header));
    AlignedBlob oldAligned(old);
    vd::fx::EffectSet refused;
    CHECK_FALSE(refused.bindModel(oldAligned.data, oldAligned.size).ok());
    // A truncated blob is refused, not read out of bounds.
    vd::fx::EffectSet truncated;
    CHECK_FALSE(truncated.bindModel(aligned.data, aligned.size / 2).ok());
}

TEST_CASE("Baked actor effects: ribbons and animated emitters sample like the original tracks") {
    VD_REQUIRE_MPQ(mpq);
    // A spell missile: animated emitters and two ribbons on its bones.
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, "Spells\\GooBolt_Missile_Low.m2");
    REQUIRE(model.ok());
    const vd::formats::M2Model& m = model.value();
    REQUIRE(m.ribbons.size() == 2);
    vd::Result<std::vector<u8>> blob = vd::bake::bakeActor(m);
    REQUIRE(blob.ok());
    AlignedBlob aligned(blob.value());
    vd::anim::Skeleton skeleton;
    REQUIRE(skeleton.bind(aligned.data, aligned.size).ok());
    vd::fx::EffectSet set;
    REQUIRE(set.bindActor(aligned.data, aligned.size, skeleton.tracks(), skeleton.boneCount()).ok());
    CHECK(set.actor());
    REQUIRE(set.ribbons().size() == 2);
    const vd::pack::RibbonEmitter& ribbon = set.ribbons()[0];
    CHECK(ribbon.bone == m.ribbons[0].bone);
    CHECK(ribbon.edgesPerSecond == doctest::Approx(30.0f));
    CHECK(ribbon.texture ==
          vd::assetPathHash(vd::normalizeAssetPath(m.textures[m.ribbons[0].textures[0]].path)));
    CHECK(ribbon.position[0] == doctest::Approx(m.ribbons[0].position[0]));
    // Every emitter of an animated one (the Great Anvil's sparks fly when the hammer hits): its baked rate
    // (in place or sampled from its track) equals the M2's at each of its keys.
    vd::Result<vd::formats::M2Model> anvilModel =
        readM2(*mpq, "World\\KHAZMODAN\\IRONFORGE\\PASSIVEDOODADS\\GreatForge\\TheGreatAnvil.m2");
    REQUIRE(anvilModel.ok());
    const vd::formats::M2Model& anvil = anvilModel.value();
    vd::Result<std::vector<u8>> anvilBlob = vd::bake::bakeActor(anvil);
    REQUIRE(anvilBlob.ok());
    AlignedBlob anvilAligned(anvilBlob.value());
    vd::anim::Skeleton anvilSkeleton;
    REQUIRE(anvilSkeleton.bind(anvilAligned.data, anvilAligned.size).ok());
    vd::fx::EffectSet anvilSet;
    REQUIRE(anvilSet
                .bindActor(anvilAligned.data, anvilAligned.size, anvilSkeleton.tracks(),
                           anvilSkeleton.boneCount())
                .ok());
    REQUIRE(anvilSet.emitters().size() == anvil.particles.size());
    u32 compared = 0;
    u32 animated = 0;
    for (vd::usize e = 0; e < anvilSet.emitters().size(); ++e) {
        const vd::pack::ParticleEmitter& baked = anvilSet.emitters()[e];
        const vd::formats::M2Track<f32>& rate = anvil.particles[e].emissionRate;
        animated += baked.tracks[vd::pack::kEmitterRate] != vd::pack::kEffectNoTrack ? 1 : 0;
        for (u32 s = 0; s < anvil.sequences.size() && s < rate.ranges.size() && rate.globalSequence < 0;
             ++s) {
            const u32 first = std::min(rate.ranges[s][0], static_cast<u32>(rate.times.size() - 1));
            const u32 last = std::min(rate.ranges[s][1], static_cast<u32>(rate.times.size() - 1));
            for (u32 k = first; k <= last; ++k) {
                if (rate.times[k] < anvil.sequences[s].start || rate.times[k] > anvil.sequences[s].end) {
                    continue;
                }
                const u32 local = rate.times[k] - anvil.sequences[s].start;
                const f32 value = baked.tracks[vd::pack::kEmitterRate] == vd::pack::kEffectNoTrack
                                      ? baked.values[vd::pack::kEmitterRate]
                                      : anvilSet.tracks().sampleScalar(baked.tracks[vd::pack::kEmitterRate],
                                                                       s, local, 0, 0.0f);
                CHECK(value == doctest::Approx(rate.values[k]));
                ++compared;
            }
        }
    }
    CHECK(animated > 5);
    CHECK(compared > 0);
}

TEST_CASE("Texture animations: the Orgrimmar waterfall scrolls") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model =
        readM2(*mpq, "World\\Kalimdor\\ORGRIMMAR\\PASSIVEDOODADS\\Waterfalls\\Orgrimmar_WaterfallA.m2");
    REQUIRE(model.ok());
    const vd::formats::M2Model& m = model.value();
    REQUIRE(m.textureTransforms.size() == 1);
    bool animatedBatch = false;
    for (const vd::formats::M2Batch& b : m.batches) {
        animatedBatch |= b.textureTransform == 0;
    }
    CHECK(animatedBatch);
    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(m);
    REQUIRE(blob.ok());
    AlignedBlob aligned(blob.value());
    vd::pack::ModelHeader header;
    std::memcpy(&header, aligned.data, sizeof(header));
    const auto* materials =
        reinterpret_cast<const vd::pack::ModelMaterial*>(aligned.data + header.materialsOffset);
    u32 animatedMaterials = 0;
    for (u32 i = 0; i < header.materialCount; ++i) {
        animatedMaterials += materials[i].uvAnimation == 1 ? 1 : 0;
    }
    CHECK(animatedMaterials >= 1);
    vd::fx::EffectSet set;
    REQUIRE(set.bindModel(aligned.data, aligned.size).ok());
    REQUIRE(set.uvAnimations().size() == 1);
    // Its translation over time, against the original keys (the loop: a global sequence or the first
    // sequence).
    const vd::formats::M2Track<vd::formats::Vec3>& translation = m.textureTransforms[0].translation;
    REQUIRE(translation.times.size() == 2);
    const u32 period =
        translation.globalSequence >= 0 ? m.globalSequences[translation.globalSequence] : set.loopMs();
    REQUIRE(period > 0);
    const u32 half = (translation.times[0] + translation.times[1]) / 2;
    vd::UvTransform uv;
    set.computeUvTransforms(0, half % std::max(1u, set.loopMs()), half, &uv);
    const vd::formats::Vec3 expected =
        vd::formats::sampleM2Track(translation, 0, half, vd::formats::Vec3{0, 0, 0});
    CHECK(uv.rows[2] == doctest::Approx(expected[0]).epsilon(1e-3));
    CHECK(uv.rows[6] == doctest::Approx(expected[1]).epsilon(1e-3));
    CHECK(uv.rows[0] == doctest::Approx(1.0f));
    CHECK(uv.rows[5] == doctest::Approx(1.0f));
}

TEST_CASE("Every M2's effects parse, bake and bind (scan-particles)") {
    VD_REQUIRE_MPQ(mpq);
    const std::vector<std::string> paths = mpq->list("*.m2");
    REQUIRE(paths.size() > 9000);
    std::mutex mutex;
    std::vector<std::string> failures;
    std::atomic<u32> emitters{0}, ribbons{0}, uvAnimations{0}, withEffects{0};
    vd::JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 16, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            vd::Result<std::vector<u8>> bytes = mpq->read(paths[i]);
            vd::Result<vd::formats::M2Model> model =
                bytes ? vd::formats::parseM2(bytes.value()) : vd::Result<vd::formats::M2Model>(bytes.error());
            std::string error;
            if (!model) {
                error = model.error().message;
            } else if (!model.value().effectsError.empty()) {
                error = model.value().effectsError;
            } else if (!model.value().particles.empty() || !model.value().ribbons.empty() ||
                       !model.value().textureTransforms.empty()) {
                ++withEffects;
                // As a doodad and as an actor: both bind, with every emitter kept that is not a model
                // emitter.
                vd::Result<std::vector<u8>> doodad = vd::bake::bakeM2(model.value());
                vd::Result<std::vector<u8>> actor = vd::bake::bakeActor(model.value());
                if (!doodad || !actor) {
                    error = "bake failed";
                } else {
                    AlignedBlob a(doodad.value());
                    AlignedBlob b(actor.value());
                    vd::fx::EffectSet doodadSet, actorSet;
                    vd::anim::Skeleton skeleton;
                    vd::Result<void> d = doodadSet.bindModel(a.data, a.size);
                    vd::Result<void> s = skeleton.bind(b.data, b.size);
                    vd::Result<void> c =
                        s ? actorSet.bindActor(b.data, b.size, skeleton.tracks(), skeleton.boneCount()) : s;
                    if (!d || !c) {
                        error = !d ? d.error().message : c.error().message;
                    } else if (doodadSet.emitters().size() != actorSet.emitters().size()) {
                        error = "emitter count differs between the formats";
                    } else if (actorSet.emitters().size() !=
                               static_cast<vd::usize>(std::count_if(
                                   model.value().particles.begin(), model.value().particles.end(),
                                   [](const vd::formats::M2ParticleEmitter& e) {
                                       return e.geometryModel.empty() && e.recursionModel.empty();
                                   }))) {
                        error = "an emitter was lost";
                    } else {
                        emitters += static_cast<u32>(actorSet.emitters().size());
                        ribbons += static_cast<u32>(actorSet.ribbons().size());
                        uvAnimations += static_cast<u32>(actorSet.uvAnimations().size());
                    }
                }
            }
            if (!error.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(paths[i] + ": " + error);
            }
        }
    });
    jobs.shutdown();
    for (const std::string& failure : failures) {
        MESSAGE(failure);
    }
    CHECK(failures.empty());
    MESSAGE(withEffects.load() << " models with effects: " << emitters.load() << " emitters, "
                               << ribbons.load() << " ribbons, " << uvAnimations.load()
                               << " texture animations");
    // scan-particles: 7,863 emitters (those that emit models or alias another model's are not baked), 590
    // ribbons, 315 texture transforms.
    CHECK(emitters.load() > 7700);
    CHECK(ribbons.load() == 590);
    CHECK(uvAnimations.load() == 315);
}
