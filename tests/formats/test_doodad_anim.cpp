// Phase 14 (14.1, 14.10a): animated doodads, billboards and the distance fade.
#include "bake/DoodadMotion.h"
#include "bake/ModelBaker.h"
#include "core/Hash.h"
#include "formats/M2.h"
#include "pack/ModelFormat.h"
#include "world/DoodadAnimation.h"
#include "world/DoodadFade.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using vd::f32;
using vd::u32;
using vd::u8;

constexpr const char* kWindmill = "World\\Azeroth\\Westfall\\Buildings\\Windmill\\WestfallWindmill.m2";
constexpr const char* kBanner = "World\\Generic\\Human\\Passive Doodads\\Banners\\DwarvenBanner01.m2";
constexpr const char* kLamp = "World\\Azeroth\\Elwynn\\PassiveDoodads\\Lamppost\\Lamppost.m2";
constexpr const char* kTorch = "World\\Generic\\PassiveDoodads\\Lights\\Torch.m2";
constexpr const char* kBarrel = "World\\Generic\\PassiveDoodads\\Furniture\\Containers\\Barrel02.m2";

vd::Result<vd::formats::M2Model> readM2(const vd::formats::MpqArchiveSet& mpq, const char* path) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    return vd::formats::parseM2(bytes.value());
}

// A baked blob in 16-byte aligned memory, as the runtime gets it.
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

} // namespace

TEST_CASE("Doodad motion census: windmill, banner, lamp, torch and a barrel") {
    VD_REQUIRE_MPQ(mpq);
    // Westfall's windmill (Elwynn's mills too): its sails turn over the 10 s Stand loop (not a global
    // sequence: instances out of step); a second Stand variation (3 % of the loops) shakes the vane.
    vd::Result<vd::formats::M2Model> windmill = readM2(*mpq, kWindmill);
    REQUIRE(windmill.ok());
    const vd::bake::DoodadMotion mill = vd::bake::analyzeDoodadMotion(windmill.value());
    CHECK(mill.loopMs == 10000);
    CHECK(mill.variations == 2);
    CHECK(vd::bake::doodadLoopVariations(windmill.value()) == std::vector<u32>{0, 1});
    CHECK(mill.animated());
    CHECK(mill.phased);
    CHECK(mill.animatedBones == 2);
    CHECK(mill.billboardVertices == 0);
    CHECK(mill.movingVertices == 74);
    CHECK(mill.maxMotion > 9.0f); // the sails' tips sweep a circle 10 yards across
    // The bounds hold the sails in every pose: wider than the bind pose's.
    CHECK(mill.boundsMax[2] - mill.boundsMin[2] >=
          windmill.value().boundsMax[2] - windmill.value().boundsMin[2] - 0.01f);

    // A banner waving on three bones.
    vd::Result<vd::formats::M2Model> banner = readM2(*mpq, kBanner);
    REQUIRE(banner.ok());
    const vd::bake::DoodadMotion flag = vd::bake::analyzeDoodadMotion(banner.value());
    CHECK(flag.animated());
    CHECK(flag.animatedBones == 3);
    CHECK(flag.movingVertices == 24);
    CHECK(flag.maxMotion == doctest::Approx(0.08f).epsilon(0.1));

    // Goldshire's lamp posts: a swinging lantern and two billboard halos (M2 bone flag 0x8).
    vd::Result<vd::formats::M2Model> lamp = readM2(*mpq, kLamp);
    REQUIRE(lamp.ok());
    const vd::bake::DoodadMotion post = vd::bake::analyzeDoodadMotion(lamp.value());
    CHECK(post.skinned());
    CHECK(post.billboardBones == 2);
    CHECK(post.billboardVertices > 0);
    CHECK(post.animatedBones == 3);
    CHECK(post.globalSequenceBones == 1);
    const std::vector<vd::i32> billboard = vd::bake::doodadBillboardBones(lamp.value());
    u32 flagged = 0;
    for (vd::usize b = 0; b < billboard.size(); ++b) {
        flagged += billboard[b] == static_cast<vd::i32>(b) ? 1 : 0;
    }
    CHECK(flagged == 2);

    // The generic torch: one billboard halo whose flicker (a global sequence) scales it.
    vd::Result<vd::formats::M2Model> torch = readM2(*mpq, kTorch);
    REQUIRE(torch.ok());
    const vd::bake::DoodadMotion fire = vd::bake::analyzeDoodadMotion(torch.value());
    CHECK(fire.billboardBones == 1);
    CHECK(fire.globalSequenceBones == 1);
    CHECK(fire.skinned());

    // A barrel never moves: it stays on the static path.
    vd::Result<vd::formats::M2Model> barrel = readM2(*mpq, kBarrel);
    REQUIRE(barrel.ok());
    const vd::bake::DoodadMotion still = vd::bake::analyzeDoodadMotion(barrel.value());
    CHECK_FALSE(still.skinned());
    CHECK(still.movingVertices == 0);
}

TEST_CASE("Baked doodad skeleton: the runtime's pose matches the M2's") {
    VD_REQUIRE_MPQ(mpq);
    for (const char* path : {kWindmill, kBanner, kLamp}) {
        CAPTURE(path);
        vd::Result<vd::formats::M2Model> model = readM2(*mpq, path);
        REQUIRE(model.ok());
        const vd::formats::M2Model& m = model.value();
        vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(m);
        REQUIRE(blob.ok());
        AlignedBlob aligned(blob.value());
        vd::pack::ModelHeader header;
        std::memcpy(&header, aligned.data, sizeof(header));
        CHECK(header.version == vd::pack::kModelFormatVersion);
        REQUIRE(header.skeletonOffset != 0);
        CHECK(header.fileRadius == doctest::Approx(m.boundsRadius));
        vd::pack::ModelSkeletonHeader skeletonHeader;
        std::memcpy(&skeletonHeader, aligned.data + header.skeletonOffset, sizeof(skeletonHeader));
        CHECK(skeletonHeader.boneCount == m.bones.size());
        REQUIRE(skeletonHeader.skinOffset != 0);

        vd::DoodadSkeleton skeleton;
        REQUIRE(skeleton.bind(aligned.data, aligned.size));
        CHECK(skeleton.moves());
        const vd::i32 loop = vd::bake::doodadLoopSequence(m);
        const u32 loopMs =
            m.sequences[static_cast<vd::usize>(loop)].end - m.sequences[static_cast<vd::usize>(loop)].start;
        CHECK(skeleton.loopMs() == loopMs);
        std::vector<vd::anim::BoneMatrix> runtime(skeleton.boneCount());
        std::vector<std::array<f32, 12>> reference;
        for (const u32 t : {0u, loopMs / 7, loopMs / 3, loopMs / 2, loopMs - 1}) {
            CAPTURE(t);
            const u32 globalMs = 123456 + t;
            skeleton.pose(0, t, globalMs, runtime.data());
            vd::bake::doodadPose(m, loop, t, globalMs, reference);
            for (u32 b = 0; b < skeleton.boneCount(); ++b) {
                for (u32 k = 0; k < 12; ++k) {
                    // Quaternions are snorm16 in the pack: a few 1e-4 of rotation, times the bone's reach.
                    CHECK(runtime[b].rows[k] == doctest::Approx(reference[b][k]).epsilon(2e-3).scale(10.0));
                }
            }
        }
        // Skin stream: weights sum 255, or all zero on a billboard bone.
        const auto* skin =
            reinterpret_cast<const vd::pack::ModelSkinVertex*>(aligned.data + skeletonHeader.skinOffset);
        const std::vector<vd::i32> billboard = vd::bake::doodadBillboardBones(m);
        u32 billboards = 0;
        for (u32 v = 0; v < header.vertexCount; ++v) {
            const u32 sum =
                u32{skin[v].weights[0]} + skin[v].weights[1] + skin[v].weights[2] + skin[v].weights[3];
            if (sum == 0) {
                ++billboards;
                CHECK((skeleton.bone(skin[v].bones[0]).flags &
                       (vd::pack::kModelBoneBillboard | vd::pack::kModelBoneBillboardZ)) != 0);
            } else {
                CHECK(sum >= 254);
                CHECK(sum <= 256);
            }
        }
        CHECK(billboards == vd::bake::analyzeDoodadMotion(m).billboardVertices);
    }
    // A static doodad has no skeleton.
    vd::Result<vd::formats::M2Model> barrel = readM2(*mpq, kBarrel);
    REQUIRE(barrel.ok());
    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(barrel.value());
    REQUIRE(blob.ok());
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.skeletonOffset == 0);
}

TEST_CASE("Baked doodad skeleton: the loop's variations and their weights") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> model = readM2(*mpq, kWindmill);
    REQUIRE(model.ok());
    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(model.value());
    REQUIRE(blob.ok());
    AlignedBlob aligned(blob.value());
    vd::DoodadSkeleton skeleton;
    REQUIRE(skeleton.bind(aligned.data, aligned.size));
    REQUIRE(skeleton.variationCount() == 2);
    CHECK(skeleton.variationMs(0) == 10000);
    CHECK(skeleton.variationMs(1) == 10000);
    // Weights 31783 and 983: the second plays about 3 % of the loops.
    u32 second = 0;
    for (u32 i = 0; i < 10000; ++i) {
        second += skeleton.nextVariation(i * 2654435761u) == 1 ? 1 : 0;
    }
    CHECK(second > 200);
    CHECK(second < 400);
    // The second variation's pose is the M2's second sequence.
    std::vector<vd::anim::BoneMatrix> runtime(skeleton.boneCount());
    std::vector<std::array<f32, 12>> reference;
    skeleton.pose(1, 2500, 0, runtime.data());
    vd::bake::doodadPose(model.value(), 1, 2500, 0, reference);
    for (u32 b = 0; b < skeleton.boneCount(); ++b) {
        for (u32 k = 0; k < 12; ++k) {
            CHECK(runtime[b].rows[k] == doctest::Approx(reference[b][k]).epsilon(2e-3).scale(10.0));
        }
    }
}

TEST_CASE("Billboard palette entry: posed pivot, scale, bind pivot and kind") {
    vd::pack::ActorBone bone{};
    bone.pivot[0] = 1.0f;
    bone.pivot[1] = 2.0f;
    bone.pivot[2] = 3.0f;
    bone.flags = vd::pack::kModelBoneBillboard;
    // Scale 2 and a translation of (10, 0, 0).
    const vd::anim::BoneMatrix posed{
        {2.0f, 0.0f, 0.0f, 10.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f}};
    const vd::anim::BoneMatrix entry = vd::billboardEntry(bone, posed);
    CHECK(entry.rows[0] == doctest::Approx(12.0f));
    CHECK(entry.rows[1] == doctest::Approx(4.0f));
    CHECK(entry.rows[2] == doctest::Approx(6.0f));
    CHECK(entry.rows[3] == doctest::Approx(2.0f));
    CHECK(entry.rows[4] == doctest::Approx(1.0f));
    CHECK(entry.rows[7] == doctest::Approx(1.0f)); // spherical
    bone.flags = vd::pack::kModelBoneBillboardZ;
    CHECK(vd::billboardEntry(bone, posed).rows[7] == doctest::Approx(2.0f)); // about Z
}

TEST_CASE("Doodad animator: one pose per model and phase") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> windmill = readM2(*mpq, kWindmill);
    vd::Result<vd::formats::M2Model> torch = readM2(*mpq, kTorch);
    REQUIRE(windmill.ok());
    REQUIRE(torch.ok());
    vd::Result<std::vector<u8>> millBlob = vd::bake::bakeM2(windmill.value());
    vd::Result<std::vector<u8>> torchBlob = vd::bake::bakeM2(torch.value());
    REQUIRE(millBlob.ok());
    REQUIRE(torchBlob.ok());
    AlignedBlob mill(millBlob.value());
    AlignedBlob fire(torchBlob.value());
    vd::DoodadAnimator animator;
    const u32 millSlot = animator.add(mill.data, mill.size);
    const u32 torchSlot = animator.add(fire.data, fire.size);
    REQUIRE(millSlot != vd::DoodadAnimator::kNone);
    REQUIRE(torchSlot != vd::DoodadAnimator::kNone);
    std::vector<vd::anim::BoneMatrix> palette(256);
    animator.beginFrame(50000, palette);
    // Windmills that appeared together share a pose; one that appeared 5 s later (half its loop) has another.
    const u32 a = animator.touch(millSlot, 1000, 20.0f);
    const u32 b = animator.touch(millSlot, 1000, 40.0f);
    const u32 c = animator.touch(millSlot, 6000, 20.0f);
    CHECK(a == b);
    CHECK(a != c);
    // The torch flickers on a global sequence: every instance in step whenever it appeared.
    CHECK(animator.touch(torchSlot, 1000, 5.0f) == animator.touch(torchSlot, 7777, 5.0f));
    // Each pose takes its bones once in the palette.
    const u32 baseA = animator.paletteBase(a);
    CHECK(animator.paletteBase(b) == baseA);
    const u32 baseC = animator.paletteBase(c);
    CHECK(baseC != baseA);
    CHECK(animator.paletteUsed() == 2 * animator.boneCount(millSlot));
    // The two phases are half a loop apart: their sails are turned differently.
    const vd::anim::BoneMatrix* poseA = animator.pose(a);
    const vd::anim::BoneMatrix* poseC = animator.pose(c);
    f32 difference = 0.0f;
    for (u32 bone = 0; bone < animator.boneCount(millSlot); ++bone) {
        for (u32 k = 0; k < 12; ++k) {
            difference += std::fabs(poseA[bone].rows[k] - poseC[bone].rows[k]);
        }
    }
    CHECK(difference > 0.1f);
    animator.remove(millSlot);
    animator.remove(torchSlot);
}

TEST_CASE("1.12 doodad distances and fade (WoW.exe 0x683F80)") {
    // Small doodads (radius <= 0.5): drawn to 50 yd, fading over the last 10.
    const vd::DoodadRange small = vd::doodadRange(0.4f, 1.0f, 700.0f);
    CHECK(small.end == doctest::Approx(50.0f));
    CHECK(small.band == doctest::Approx(10.0f));
    CHECK(vd::doodadFade(small, 30.0f) == doctest::Approx(1.0f));
    CHECK(vd::doodadFade(small, 40.0f) == doctest::Approx(1.0f));
    CHECK(vd::doodadFade(small, 45.0f) == doctest::Approx(0.5f));
    CHECK(vd::doodadFade(small, 50.0f) == doctest::Approx(0.0f));
    CHECK(vd::doodadFade(small, 60.0f) == doctest::Approx(0.0f));
    // Medium (<= 2.5): 125 yd over 25; large (<= 7): 200 over 50, never past the far clip.
    const vd::DoodadRange medium = vd::doodadRange(1.0f, 1.0f, 700.0f);
    CHECK(medium.end == doctest::Approx(125.0f));
    CHECK(vd::doodadFade(medium, 112.5f) == doctest::Approx(0.5f));
    const vd::DoodadRange large = vd::doodadRange(5.0f, 1.0f, 700.0f);
    CHECK(large.end == doctest::Approx(200.0f));
    CHECK(vd::doodadFade(large, 175.0f) == doctest::Approx(0.5f));
    CHECK(vd::doodadRange(5.0f, 1.0f, 180.0f).end == doctest::Approx(180.0f));
    // Bigger ones have no range of their own: always opaque.
    const vd::DoodadRange huge = vd::doodadRange(12.0f, 1.0f, 700.0f);
    CHECK(huge.end == doctest::Approx(0.0f));
    CHECK(vd::doodadFade(huge, 600.0f) == doctest::Approx(1.0f));
    // The quality's environment detail stretches the ranges and their bands.
    const vd::DoodadRange far = vd::doodadRange(0.4f, 1.3f, 700.0f);
    CHECK(far.end == doctest::Approx(65.0f));
    CHECK(far.band == doctest::Approx(13.0f));
}
