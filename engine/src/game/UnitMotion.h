#pragma once

#include "core/Types.h"

#include <unordered_map>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

struct Entity;

// Phase 18 (motion): how WoW.exe 1.12.1 shows the movement of a unit it does not move itself (read-only
// disassembly). Pure functions of an entity: app/Units animates the server's units with them.
//
// CMovement::GetCurrentSpeed (0x7C4C90): 0 without a direction flag; along a server spline, the spline's
// length over its duration; swimming, the swim speed (the slower of swim and swim back going backwards); in
// walk mode the slower of walk and run; else run (the slower of run and run back going backwards).
//
// The movement animation (CGUnit_C 0x5FD100), for a unit that moves:
// - swimming: Swim forward (42), SwimBackwards (45), SwimLeft (43) / SwimRight (44) strafing, SwimIdle
//   (41) while turning;
// - a flying spline (MoveSplineFlag Flying: taxis, flying creatures): Fly (135);
// - backwards: WalkBackwards (13);
// - stealthed (UNIT_FIELD_BYTES_1 byte 3, UNIT_VIS_FLAGS_CREEP): StealthWalk (119);
// - else by the speed it measures: Sprint (143) from 11 yards per second, Run (5) above twice its walk
//   speed, Walk (4) at or below it. Walk or run is the speed's, not the walk mode's: a slowed runner walks, a
//   pet catching up (FollowMovementGenerator: up to 2.1 times its master's speed) sprints.
//   SMSG_SPLINE_MOVE_SET_WALK_MODE / _RUN_MODE (vMaNGOS sends them before each spline that changes the mode)
//   only change what a unit moving by its own flags measures.
// The models fall back through AnimationData (Sprint -> Run, StealthWalk -> Walk, SwimBackwards -> Swim).
f32 currentSpeed(const Entity& e, u32 nowMs, bool swimming);

struct MotionAnimation {
    u16 animation = 0; // AnimationData id (anim::animation); Stand / SwimIdle when not moving
    f32 speed = 0.0f;  // what the legs match, yards per second (0: not moving)
    bool moving = false;
};
// `swimming`: its swim flag or the client's own decision (unitSwims).
MotionAnimation movementAnimation(const Entity& e, u32 nowMs, bool swimming);

// CGUnit_C 0x6030C0, the depth of liquid at a unit (from its feet up to the surface) as the client checks it
// every update: one not swimming starts when the depth passes 0.75 of its collision height - 1/36 yard and it
// may swim (UNIT_FLAG_USE_SWIM_ANIMATION, UNIT_FLAG_PLAYER_CONTROLLED or UNIT_FLAG_PET_IN_COMBAT; without
// them a creature walks on the bottom) unless it walks on water; one swimming stops below that depth. Never
// while levitating. The players' own clients tell theirs (MSG_MOVE_START_SWIM): this is for the server's
// creatures.
f32 swimDepth(f32 collisionHeight);
bool unitSwims(const Entity& e, bool swimmingNow, f32 depth, f32 collisionHeight);

// CreatureModelData's collision height of a display's model (CreatureDisplayInfo -> CreatureModelData, yards
// before the unit's scale): the height its swimming depth is measured against.
class UnitShapes {
public:
    void init(const pack::PackReader& db);
    // 0 when unknown.
    f32 modelHeight(u32 displayId) const;
    usize size() const { return m_heights.size(); }

private:
    std::unordered_map<u32, f32> m_heights; // display id -> collision height
};

} // namespace vd::game
