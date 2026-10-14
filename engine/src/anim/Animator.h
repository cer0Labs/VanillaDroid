#pragma once

#include "anim/Skeleton.h"
#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <span>

namespace vd::anim {

// Animation ids (AnimationData.dbc, 5875) the game picks directly.
namespace animation {
constexpr u16 kStand = 0;
constexpr u16 kDeath = 1;
constexpr u16 kWalk = 4;
constexpr u16 kRun = 5;
constexpr u16 kDead = 6;
constexpr u16 kWalkBackwards = 13;
constexpr u16 kReadyUnarmed = 25;
constexpr u16 kReady1H = 26;
constexpr u16 kReady2H = 27;
constexpr u16 kJumpStart = 37;
constexpr u16 kJump = 38;
constexpr u16 kJumpEnd = 39;
constexpr u16 kFall = 40;
constexpr u16 kSwimIdle = 41;
constexpr u16 kSwim = 42;
constexpr u16 kSwimBackwards = 45;
constexpr u16 kMount = 91;
constexpr u16 kSitGround = 97;
constexpr u16 kSleep = 100;
constexpr u16 kSitChairLow = 102;
constexpr u16 kSitChairMedium = 103;
constexpr u16 kSitChairHigh = 104;
constexpr u16 kKneel = 115;
// Phase 9: combat, spells and emotes.
constexpr u16 kCombatWound = 9;
constexpr u16 kCombatCritical = 10;
constexpr u16 kAttackUnarmed = 16;
constexpr u16 kAttack1H = 17;
constexpr u16 kAttack2H = 18;
constexpr u16 kAttack2HLoose = 19;
constexpr u16 kParryUnarmed = 20;
constexpr u16 kParry1H = 21;
constexpr u16 kParry2H = 22;
constexpr u16 kParry2HLoose = 23;
constexpr u16 kShieldBlock = 24;
constexpr u16 kReady2HLoose = 28;
constexpr u16 kReadyBow = 29;
constexpr u16 kDodge = 30;
constexpr u16 kSpellPrecast = 31;
constexpr u16 kSpellCast = 32;
constexpr u16 kAttackBow = 46;
constexpr u16 kReadyRifle = 48;
constexpr u16 kAttackRifle = 49;
constexpr u16 kReadySpellDirected = 51;
constexpr u16 kReadySpellOmni = 52;
constexpr u16 kSpellCastDirected = 53;
constexpr u16 kSpellCastOmni = 54;
constexpr u16 kEmoteEat = 61; // food, drink, potions: always on the upper body (WoW.exe 0x5FE834)
constexpr u16 kAttack1HPierce = 85;
constexpr u16 kAttackOff = 87;
constexpr u16 kAttackOffPierce = 88;
constexpr u16 kAttackThrown = 107;
constexpr u16 kReadyThrown = 108;
constexpr u16 kAttackUnarmedOff = 117;
constexpr u16 kChannelCastDirected = 124;
constexpr u16 kChannelCastOmni = 125;
// Phase 18 (motion): the rest of the movement animations of WoW.exe's choice (game/UnitMotion).
constexpr u16 kSwimLeft = 43;
constexpr u16 kSwimRight = 44;
constexpr u16 kStealthWalk = 119;
constexpr u16 kFly = 135;
constexpr u16 kSprint = 143;
constexpr u16 kCount = 256; // ids stay below this in 1.12
} // namespace animation

// AnimationData.dbc body flags (field 3) the animation code reads. Inferred from the 1.12 table: kLegs is on the
// animations that move the legs (Stand, Walk, Run, Swim, Jump, strafing), kUpperBody on those that may play on the
// upper body only while the legs keep moving (swings, wounds, parries, directed casts, talking and pointing; not
// the full-body abilities, kicks or bows).
namespace bodyflag {
constexpr u16 kLegs = 0x1;
constexpr u16 kUpperBody = 0x8;
} // namespace bodyflag

// AnimationData.dbc weapon flags (field 2), as WoW.exe 0x5FDF80 reads them each time a unit's animation is chosen:
// kSheathe puts the weapons away whatever else holds (casting, swimming, looting, mounting, a chair, sleep),
// kUnarmed puts them away too (unarmed swings and stance, emotes, sitting), kArmed draws them (swings, the armed
// stances). game::sheathAfterAnimation has the whole rule.
namespace weaponflag {
constexpr u16 kSheathe = 0x4;
constexpr u16 kUnarmed = 0x10;
constexpr u16 kArmed = 0x20;
} // namespace weaponflag

// Ours (seated animations): WoW.exe 0x5FE2F0 plays these on the upper body only while the legs are busy (moving, or a
// stand state: sitting, on a chair, kneeling, asleep): its table at 0x5FED90 without the ones that always take the
// whole body (JumpEnd, Special1H / 2H, SpecialUnarmed: 0x5FEC40, 0x5FEC60). EmoteEat goes there even standing still.
bool upperBodyAnimation(u16 id);

// What to play when a model lacks an animation (AnimationData.dbc "fallback" column), ending at Stand. Phase 9:
// also each animation's priority and body flags, which decide how an action (a swing, a wound) plays over what
// the actor is doing (Actors::playAction).
struct AnimFallbacks {
    std::array<u16, animation::kCount> next{}; // next[id] = fallback id (kStand when unknown)
    // Field 4: which animation wins (Stand 0, casting 1, movement 10, combat 20, mounted 200, death 500).
    std::array<u16, animation::kCount> priority{};
    std::array<u16, animation::kCount> bodyFlags{}; // field 3: bodyflag::*
    std::array<u16, animation::kCount> weaponFlags{}; // field 2: weaponflag::* (what it does with the weapons)
    AnimFallbacks();
    // AnimationData.dbc (5875): field 0 id, 2 weapon flags, 3 body flags, 4 priority, 6 fallback. Keeps the
    // built-in movement fallbacks (Run -> Walk) where the table only says Stand.
    void load(const db::DbcView& animationData);
    u16 priorityOf(u16 id) const { return id < animation::kCount ? priority[id] : 0; }
    u16 bodyFlagsOf(u16 id) const { return id < animation::kCount ? bodyFlags[id] : 0; }
    u16 weaponFlagsOf(u16 id) const { return id < animation::kCount ? weaponFlags[id] : 0; }
};

// Chooses and advances the sequences of one skeleton: loops with random variations (weighted by the sequences'
// frequency), holds the last frame of one-shot animations, and cross-fades over the new sequence's blend time.
class Animator {
public:
    // Switches to `animationId` (or its fallback) unless it is already playing. `loop` = false holds the last
    // frame (death). Returns the animation actually playing. `restart`: from its start (and a new variation) even
    // when it is already playing (one swing after another), blending from where the last one was.
    u16 play(const Skeleton& skeleton, u16 animationId, bool loop, const AnimFallbacks& fallbacks, u32& random,
             bool restart = false);
    // Advances by dt seconds times `speed` (walk/run playback matches the movement speed).
    void update(const Skeleton& skeleton, f32 dt, f32 speed, u32& random);
    void reset();

    const AnimPose& pose() const { return m_pose; }
    u16 requested() const { return m_requested; }
    bool finished() const { return m_finished; } // a one-shot animation reached its last frame

    // Phase 14: the stretch of animation the last update() played, for the events on its timeline: local ms
    // [from, to) of a sequence, in at most two pieces (the end of a loop and the start of its next cycle or
    // variation). Empty before the first update and while nothing advances.
    struct Played {
        i32 sequence = -1;
        u32 from = 0;
        u32 to = 0;
    };
    std::span<const Played> played() const { return {m_played.data(), m_playedCount}; }

private:
    i32 pickVariation(const Skeleton& skeleton, i32 first, u32& random) const;
    void start(const Skeleton& skeleton, i32 sequence, bool blendSame = false);

    AnimPose m_pose;
    u16 m_requested = 0xFFFF;
    i32 m_first = -1; // first variation of the playing animation
    bool m_loop = true;
    bool m_currentLoop = true;  // whether the sequence shown loops (m_loop when it started)
    bool m_previousLoop = true; // and the one blending out
    bool m_finished = false;
    f32 m_timeMs = 0.0f; // exact time inside the current sequence
    f32 m_previousMs = 0.0f;
    f32 m_blendRate = 0.0f; // blend weight per second
    std::array<Played, 2> m_played{}; // phase 14
    u32 m_playedCount = 0;
};

} // namespace vd::anim
