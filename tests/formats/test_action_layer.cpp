// Phase 9: actions over the base animation (swings, wounds, casts) on the whole body or the upper body only,
// with the priorities and body flags of AnimationData.dbc.
#include "anim/Animator.h"
#include "anim/Skeleton.h"
#include "bake/ActorBaker.h"
#include "db/DbcView.h"
#include "formats/M2.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cstring>
#include <memory>
#include <vector>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u8;
namespace animation = vd::anim::animation;

struct Baked {
    std::vector<u8> bytes;
    std::unique_ptr<u8[]> storage;
    vd::anim::Skeleton skeleton;
};

// A baked actor in 16-byte aligned memory, as the runtime gets it from the pack reader.
bool bake(const vd::formats::MpqArchiveSet& mpq, const char* path, Baked& out) {
    vd::Result<std::vector<u8>> file = mpq.read(path);
    if (!file) {
        return false;
    }
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(file.value());
    if (!model) {
        return false;
    }
    vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(model.value());
    if (!baked) {
        return false;
    }
    out.bytes = std::move(baked.value());
    out.storage.reset(new u8[out.bytes.size() + 16]);
    u8* data = out.storage.get() + (16 - reinterpret_cast<uintptr_t>(out.storage.get()) % 16) % 16;
    std::memcpy(data, out.bytes.data(), out.bytes.size());
    return out.skeleton.bind(data, out.bytes.size()).ok();
}

// The key bone a bone belongs to (itself or its closest ancestor with one), -1 if none.
vd::i32 keyBoneOf(const vd::anim::Skeleton& skeleton, u32 bone) {
    for (vd::i32 b = static_cast<vd::i32>(bone); b >= 0; b = skeleton.bone(static_cast<u32>(b)).parent) {
        if (skeleton.bone(static_cast<u32>(b)).keyBone >= 0) {
            return skeleton.bone(static_cast<u32>(b)).keyBone;
        }
    }
    return -1;
}

f32 difference(const vd::anim::BoneMatrix& a, const vd::anim::BoneMatrix& b) {
    f32 d = 0.0f;
    for (u32 i = 0; i < 12; ++i) {
        d = std::max(d, std::abs(a.rows[i] - b.rows[i]));
    }
    return d;
}

bool loadAnimationData(const vd::formats::MpqArchiveSet& mpq, vd::anim::AnimFallbacks& table) {
    vd::Result<std::vector<u8>> file = mpq.read("DBFilesClient\\AnimationData.dbc");
    if (!file) {
        return false;
    }
    vd::Result<vd::db::DbcView> view = vd::db::DbcView::parse(file.value().data(), file.value().size());
    if (!view) {
        return false;
    }
    table.load(view.value());
    return true;
}

} // namespace

TEST_CASE("AnimationData: priorities and body flags of the actions") {
    VD_REQUIRE_MPQ(mpq);
    vd::anim::AnimFallbacks table;
    REQUIRE(loadAnimationData(*mpq, table));
    // Priorities: Stand < casting < movement < combat < mounted < death.
    CHECK(table.priorityOf(animation::kStand) == 0);
    CHECK(table.priorityOf(animation::kSpellCast) == 1);
    CHECK(table.priorityOf(animation::kRun) == 10);
    CHECK(table.priorityOf(animation::kAttack1H) == 20);
    CHECK(table.priorityOf(animation::kCombatWound) == 20);
    CHECK(table.priorityOf(animation::kMount) == 200);
    CHECK(table.priorityOf(animation::kDeath) == 500);
    // The legs: standing, walking, running, swimming; not the ready stances or casting.
    for (u16 id :
         {animation::kStand, animation::kWalk, animation::kRun, animation::kSwim, animation::kJump}) {
        CAPTURE(id);
        CHECK((table.bodyFlagsOf(id) & vd::anim::bodyflag::kLegs) != 0);
    }
    CHECK((table.bodyFlagsOf(animation::kReady1H) & vd::anim::bodyflag::kLegs) == 0);
    // Upper body while running: swings, wounds, defenses, directed casts; not abilities or full-body emotes.
    for (u16 id :
         {animation::kAttack1H, animation::kAttack2H, animation::kAttackOff, animation::kCombatWound,
          animation::kCombatCritical, animation::kParry1H, animation::kDodge, animation::kShieldBlock,
          animation::kSpellCastDirected, animation::kSpellCastOmni, animation::kAttackBow}) {
        CAPTURE(id);
        CHECK((table.bodyFlagsOf(id) & vd::anim::bodyflag::kUpperBody) != 0);
    }
    for (u16 id :
         {u16{57} /* Special1H */, u16{95} /* Kick */, animation::kSpellCast, u16{69} /* EmoteDance */}) {
        CAPTURE(id);
        CHECK((table.bodyFlagsOf(id) & vd::anim::bodyflag::kUpperBody) == 0);
    }
    // Weapon flags (WoW.exe 0x5FDF80): casting, swimming and looting put the weapons away, the unarmed stance and
    // swing too, the armed swings and stances draw them; standing and running leave them where they are.
    for (u16 id : {animation::kSpellCast, animation::kReadySpellDirected, animation::kSpellCastDirected,
                   animation::kSpellCastOmni, animation::kChannelCastDirected, animation::kSwim, u16{50} /* Loot */}) {
        CAPTURE(id);
        CHECK(table.weaponFlagsOf(id) == vd::anim::weaponflag::kSheathe);
    }
    CHECK(table.weaponFlagsOf(animation::kReadyUnarmed) == vd::anim::weaponflag::kUnarmed);
    CHECK(table.weaponFlagsOf(animation::kAttackUnarmed) == vd::anim::weaponflag::kUnarmed);
    CHECK(table.weaponFlagsOf(animation::kAttack2HLoose) == vd::anim::weaponflag::kArmed);
    CHECK(table.weaponFlagsOf(animation::kReady1H) == vd::anim::weaponflag::kArmed);
    CHECK(table.weaponFlagsOf(animation::kStand) == 0);
    CHECK(table.weaponFlagsOf(animation::kRun) == 0);
    // Fallbacks the swings rely on: a dagger's thrust falls back to the 1H swing, that one to the fists.
    CHECK(table.next[animation::kAttack1HPierce] == animation::kAttack1H);
    CHECK(table.next[animation::kAttack1H] == animation::kAttackUnarmed);
    CHECK(table.next[animation::kAttackOff] == animation::kAttackOffPierce);
    CHECK(table.next[animation::kCombatCritical] == animation::kCombatWound);
}

TEST_CASE("WoW.exe's upper-body animations (0x5FED90): over a moving or seated unit's legs") {
    // Eating and drinking (EmoteEat), casts, talking, the work and fishing loops: on the upper body.
    for (u16 id : {animation::kEmoteEat, animation::kSpellCast, animation::kSpellCastDirected, animation::kAttack1H,
                   animation::kCombatWound, u16{60} /* EmoteTalk */, u16{69} /* EmoteDance */,
                   u16{123} /* UseStandingLoop */, u16{134} /* FishingLoop */}) {
        CAPTURE(id);
        CHECK(vd::anim::upperBodyAnimation(id));
    }
    // The whole body always: standing, death, JumpEnd, the specials, a kick, sitting down.
    for (u16 id : {animation::kStand, animation::kDeath, animation::kJumpEnd, u16{57} /* Special1H */,
                   u16{58} /* Special2H */, u16{118} /* SpecialUnarmed */, u16{95} /* Kick */,
                   animation::kSitGround, u16{300}}) {
        CAPTURE(id);
        CHECK_FALSE(vd::anim::upperBodyAnimation(id));
    }
}

TEST_CASE("Skeleton: upper body of the races, none for most creatures") {
    VD_REQUIRE_MPQ(mpq);
    for (const char* path :
         {"Character\\Human\\Male\\HumanMale.m2", "Character\\Tauren\\Female\\TaurenFemale.m2",
          "Character\\Gnome\\Male\\GnomeMale.m2"}) {
        CAPTURE(path);
        Baked model;
        REQUIRE(bake(*mpq, path, model));
        const vd::anim::Skeleton& s = model.skeleton;
        REQUIRE(s.hasUpperBody());
        u32 upper = 0;
        for (u32 b = 0; b < s.boneCount(); ++b) {
            const vd::i32 key = keyBoneOf(s, b);
            // Key bones: 0/1 arms, 2/3 shoulders, 6 head (upper); 5 waist (the legs), 26 root.
            if (key == 0 || key == 1 || key == 2 || key == 3 || key == 6) {
                CHECK(s.upperBody(b));
            }
            if (key == 5 || key == 26) {
                CHECK_FALSE(s.upperBody(b));
            }
            upper += s.upperBody(b) ? 1 : 0;
        }
        CHECK(upper > s.boneCount() / 3);
        CHECK(upper < s.boneCount());
    }
    Baked wolf;
    REQUIRE(bake(*mpq, "Creature\\Wolf\\Wolf.m2", wolf));
    CHECK_FALSE(wolf.skeleton.hasUpperBody());
}

TEST_CASE("Action layer: a swing on the upper body leaves the running legs alone") {
    VD_REQUIRE_MPQ(mpq);
    Baked model;
    REQUIRE(bake(*mpq, "Character\\Human\\Male\\HumanMale.m2", model));
    const vd::anim::Skeleton& s = model.skeleton;
    const vd::i32 run = s.findSequence(animation::kRun);
    const vd::i32 swing = s.findSequence(animation::kAttack1H);
    REQUIRE(run >= 0);
    REQUIRE(swing >= 0);
    vd::anim::AnimPose base;
    base.current = {run, s.duration(static_cast<u32>(run)) / 3};
    vd::anim::ActionPose action;
    action.pose.current = {swing, s.duration(static_cast<u32>(swing)) / 2};
    action.weight = 1.0f;
    action.upperBody = true;

    std::vector<vd::anim::BoneMatrix> running(s.boneCount()), split(s.boneCount()), full(s.boneCount());
    vd::anim::computeBones(s, base, 0, glm::vec3(0.0f), running.data());
    vd::anim::computeBones(s, base, 0, glm::vec3(0.0f), split.data(), &action);
    action.upperBody = false;
    vd::anim::computeBones(s, base, 0, glm::vec3(0.0f), full.data(), &action);
    f32 legs = 0.0f, arms = 0.0f, fullLegs = 0.0f;
    for (u32 b = 0; b < s.boneCount(); ++b) {
        const vd::i32 key = keyBoneOf(s, b);
        if (key == 5) { // the legs hang from the waist
            legs = std::max(legs, difference(running[b], split[b]));
            fullLegs = std::max(fullLegs, difference(running[b], full[b]));
        }
        if (key == 1) { // the right arm swings
            arms = std::max(arms, difference(running[b], split[b]));
        }
    }
    CHECK(legs < 1e-5f);     // split: the legs keep running
    CHECK(arms > 0.05f);     // and the arm swings
    CHECK(fullLegs > 0.01f); // the whole body: the legs take the swing's stance

    // Half weight lies between the two.
    action.upperBody = true;
    action.weight = 0.5f;
    std::vector<vd::anim::BoneMatrix> half(s.boneCount());
    vd::anim::computeBones(s, base, 0, glm::vec3(0.0f), half.data(), &action);
    for (u32 b = 0; b < s.boneCount(); ++b) {
        if (keyBoneOf(s, b) == 1) {
            CHECK(difference(running[b], half[b]) <= difference(running[b], split[b]) + 1e-4f);
        }
    }
}

TEST_CASE("Animator: restarting a one-shot (the next swing) plays it again from the start") {
    VD_REQUIRE_MPQ(mpq);
    Baked model;
    REQUIRE(bake(*mpq, "Character\\Orc\\Male\\OrcMale.m2", model));
    const vd::anim::Skeleton& s = model.skeleton;
    vd::anim::AnimFallbacks fallbacks;
    REQUIRE(loadAnimationData(*mpq, fallbacks));
    vd::anim::Animator animator;
    u32 random = 777;
    CHECK(animator.play(s, animation::kAttack1H, false, fallbacks, random, true) == animation::kAttack1H);
    for (u32 i = 0; i < 300 && !animator.finished(); ++i) {
        animator.update(s, 1.0f / 60.0f, 1.0f, random);
    }
    REQUIRE(animator.finished());
    // Asked again without restart: it holds; with restart: from the start, blending from the last frame.
    animator.play(s, animation::kAttack1H, false, fallbacks, random);
    CHECK(animator.finished());
    animator.play(s, animation::kAttack1H, false, fallbacks, random, true);
    CHECK_FALSE(animator.finished());
    CHECK(animator.pose().current.time == 0);
    CHECK(animator.pose().previous.sequence >= 0);
    // A creature without the animation falls back (a wolf has no 1H swing: its bite, AttackUnarmed).
    Baked wolf;
    REQUIRE(bake(*mpq, "Creature\\Wolf\\Wolf.m2", wolf));
    vd::anim::Animator bite;
    CHECK(bite.play(wolf.skeleton, animation::kAttack1H, false, fallbacks, random) ==
          animation::kAttackUnarmed);
}
