#include "anim/Animator.h"
#include "anim/Skeleton.h"
#include "bake/ActorBaker.h"
#include "formats/M2.h"
#include "pack/ActorFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

namespace {

using vd::f32;
using vd::u16;
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
    explicit AlignedBlob(const std::vector<u8>& bytes) : storage(new u8[bytes.size() + 16]), size(bytes.size()) {
        data = storage.get() + (16 - reinterpret_cast<uintptr_t>(storage.get()) % 16) % 16;
        std::memcpy(data, bytes.data(), bytes.size());
    }
};

} // namespace

TEST_CASE("M2 skeleton: HumanMale bones, sequences and attachments") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, "Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(model.ok());
    const vd::formats::M2Model& m = model.value();
    CHECK(m.bones.size() == 119);
    CHECK(m.sequences.size() == 143);
    CHECK(m.globalSequences.size() == 2);
    CHECK(m.sequences[0].animationId == 0); // Stand
    CHECK(m.sequences[1].animationId == 4); // Walk
    CHECK(m.sequences[2].animationId == 5); // Run
    CHECK(m.sequences[2].moveSpeed == doctest::Approx(6.94f).epsilon(0.01));
    // Right hand (1) on the model's right (-Y), helm (11) on top of the head.
    const auto attachment = [&](u32 id) {
        for (const vd::formats::M2Attachment& a : m.attachments) {
            if (a.id == id) {
                return a;
            }
        }
        FAIL("missing attachment " << id);
        return vd::formats::M2Attachment{};
    };
    CHECK(attachment(1).position[1] < -0.3f);
    CHECK(attachment(2).position[1] > 0.3f);
    CHECK(attachment(11).position[2] > 1.9f);
    // Replaceable textures: body skin, hair, cape.
    REQUIRE(m.textures.size() == 3);
    CHECK(m.textures[0].type == vd::formats::kM2TextureBody);
    CHECK(m.textures[1].type == vd::formats::kM2TextureHair);
    CHECK(m.textures[2].type == vd::formats::kM2TextureObjectSkin);
}

TEST_CASE("Baked actor: sampling matches the original tracks, bones animate") {
    VD_REQUIRE_MPQ(mpq);
    for (const char* path : {"Character\\Human\\Male\\HumanMale.m2", "Creature\\Wolf\\Wolf.m2"}) {
        CAPTURE(path);
        vd::Result<vd::formats::M2Model> model = readM2(*mpq, path);
        REQUIRE(model.ok());
        const vd::formats::M2Model& m = model.value();
        vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(m);
        REQUIRE(baked.ok());
        AlignedBlob blob(baked.value());
        vd::anim::Skeleton skeleton;
        REQUIRE(skeleton.bind(blob.data, blob.size).ok());
        REQUIRE(skeleton.boneCount() == m.bones.size());
        REQUIRE(skeleton.sequenceCount() == m.sequences.size());

        // Translation keys: the baked sampler agrees with the original track at many times of every sequence.
        u32 compared = 0;
        for (u32 b = 0; b < m.bones.size(); ++b) {
            const vd::formats::M2Track<vd::formats::Vec3>& track = m.bones[b].translation;
            const u32 bakedTrack = skeleton.bone(b).tracks[0];
            if (!track.animated() || bakedTrack == vd::pack::kActorNoTrack || track.globalSequence >= 0) {
                continue;
            }
            for (u32 s = 0; s < m.sequences.size(); s += 7) {
                const u32 duration = m.sequences[s].end - m.sequences[s].start;
                for (u32 step = 0; step <= 4; ++step) {
                    const u32 local = duration * step / 4;
                    const vd::formats::Vec3 expected =
                        vd::formats::sampleM2Track(track, s, m.sequences[s].start + local, {0.0f, 0.0f, 0.0f});
                    const glm::vec3 got = skeleton.sampleVec3(bakedTrack, s, local, 0, glm::vec3(0.0f));
                    CHECK(got.x == doctest::Approx(expected[0]).epsilon(1e-4));
                    CHECK(got.y == doctest::Approx(expected[1]).epsilon(1e-4));
                    CHECK(got.z == doctest::Approx(expected[2]).epsilon(1e-4));
                    ++compared;
                }
            }
        }
        CHECK(compared > 0);

        // Bind pose: identity matrices. Walking: the feet move between two frames of the cycle.
        std::vector<vd::anim::BoneMatrix> bones(skeleton.boneCount());
        vd::anim::AnimPose bind;
        vd::anim::computeBones(skeleton, bind, 0, glm::vec3(0.0f), bones.data());
        for (const vd::anim::BoneMatrix& bone : bones) {
            CHECK(bone.rows[0] == doctest::Approx(1.0f));
            CHECK(bone.rows[3] == doctest::Approx(0.0f));
        }
        const vd::i32 walk = skeleton.findSequence(vd::anim::animation::kWalk);
        REQUIRE(walk >= 0);
        vd::anim::AnimPose pose;
        pose.current = {walk, 0};
        vd::anim::computeBones(skeleton, pose, 0, glm::vec3(0.0f), bones.data());
        std::vector<vd::anim::BoneMatrix> later(skeleton.boneCount());
        pose.current.time = skeleton.duration(static_cast<u32>(walk)) / 2;
        vd::anim::computeBones(skeleton, pose, 0, glm::vec3(0.0f), later.data());
        f32 moved = 0.0f;
        for (u32 b = 0; b < skeleton.boneCount(); ++b) {
            const glm::vec3 pivot(skeleton.bone(b).pivot[0], skeleton.bone(b).pivot[1], skeleton.bone(b).pivot[2]);
            moved = std::max(moved, glm::length(vd::anim::transformPoint(bones[b], pivot) -
                                                vd::anim::transformPoint(later[b], pivot)));
        }
        CHECK(moved > 0.1f); // yards: a leg swings forward and back
    }
}

TEST_CASE("Animator: fallbacks, one-shot hold and variations") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, "Creature\\Wolf\\Wolf.m2");
    REQUIRE(model.ok());
    vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(model.value());
    REQUIRE(baked.ok());
    AlignedBlob blob(baked.value());
    vd::anim::Skeleton skeleton;
    REQUIRE(skeleton.bind(blob.data, blob.size).ok());

    vd::anim::AnimFallbacks fallbacks;
    vd::anim::Animator animator;
    u32 random = 12345;
    CHECK(animator.play(skeleton, vd::anim::animation::kRun, true, fallbacks, random) == vd::anim::animation::kRun);
    // Something wolves lack (Mount) falls back to Stand.
    CHECK(animator.play(skeleton, vd::anim::animation::kMount, true, fallbacks, random) == vd::anim::animation::kStand);
    CHECK(animator.pose().blend < 1.0f); // cross-fading from Run
    for (u32 i = 0; i < 100; ++i) {
        animator.update(skeleton, 1.0f / 60.0f, 1.0f, random);
    }
    CHECK(animator.pose().blend == doctest::Approx(1.0f));
    // Death holds its last frame.
    animator.play(skeleton, vd::anim::animation::kDeath, false, fallbacks, random);
    for (u32 i = 0; i < 600; ++i) {
        animator.update(skeleton, 1.0f / 60.0f, 1.0f, random);
    }
    CHECK(animator.finished());
    const u32 duration = skeleton.duration(static_cast<u32>(animator.pose().current.sequence));
    CHECK(animator.pose().current.time == duration - 1);
}

TEST_CASE("Skeleton sampling cost (report only)") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, "Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(model.ok());
    vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(model.value());
    REQUIRE(baked.ok());
    AlignedBlob blob(baked.value());
    vd::anim::Skeleton skeleton;
    REQUIRE(skeleton.bind(blob.data, blob.size).ok());
    std::vector<vd::anim::BoneMatrix> bones(skeleton.boneCount());
    vd::anim::AnimPose pose;
    pose.current = {skeleton.findSequence(vd::anim::animation::kWalk), 0};
    const u32 duration = skeleton.duration(static_cast<u32>(pose.current.sequence));
    constexpr u32 kRuns = 20000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRuns; ++i) {
        pose.current.time = (i * 7) % duration;
        vd::anim::computeBones(skeleton, pose, i, glm::vec3(0.0f), bones.data());
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("HumanMale walk: " << ns / kRuns / 1000.0 << " us per skeleton, " << ns / kRuns / skeleton.boneCount()
                               << " ns per bone (" << skeleton.boneCount() << " bones)");
    CHECK(bones[0].rows[0] == bones[0].rows[0]);
}
