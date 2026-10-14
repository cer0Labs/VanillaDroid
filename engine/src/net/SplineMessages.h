#pragma once

#include "core/Types.h"
#include "net/Opcodes.gen.h"

#include <glm/vec3.hpp>

#include <span>
#include <vector>

// Phase 18 (motion): how the server moves the units it controls, as vmangos-core 4641790 writes it for build
// 5875 (Movement/MovementPacketSender.cpp: SendMovementFlagChangeToAll, SendToggleRunWalkToAll,
// SendSpeedChangeToAll; Movement/spline/MoveSplineInit.cpp Launch and packet_builder.cpp WriteMonsterMove).
// Same rules as net/GameMessages: parsers return false on a malformed packet and never read past its end. The
// builders write what the server writes: the tests and the offline demonstration (app/MotionDemo) feed them
// to the entity store.
namespace vd::net {

// SMSG_SPLINE_MOVE_*: a movement state of a unit the server moves (a creature, a feared or charmed player),
// sent to everyone around (WoW.exe 1.12.1 handles them all in 0x603C80 -> CGUnit_C 0x601420, on the unit's
// CMovement).
enum class SplineMoveKind : u8 {
    Root,        // SMSG_SPLINE_MOVE_ROOT: MOVEFLAG_ROOT; its spline ends (Unit::SetRooted sends a stop first)
    Unroot,      // SMSG_SPLINE_MOVE_UNROOT
    WaterWalk,   // SMSG_SPLINE_MOVE_WATER_WALK: MOVEFLAG_WATERWALKING
    LandWalk,    // SMSG_SPLINE_MOVE_LAND_WALK
    FeatherFall, // SMSG_SPLINE_MOVE_FEATHER_FALL: MOVEFLAG_SAFE_FALL
    NormalFall,  // SMSG_SPLINE_MOVE_NORMAL_FALL
    SetHover,    // SMSG_SPLINE_MOVE_SET_HOVER: MOVEFLAG_HOVER
    UnsetHover,  // SMSG_SPLINE_MOVE_UNSET_HOVER
    StartSwim, // SMSG_SPLINE_MOVE_START_SWIM: MOVEFLAG_SWIMMING (WoW.exe handles it; vMaNGOS never sends it)
    StopSwim,  // SMSG_SPLINE_MOVE_STOP_SWIM
    RunMode,  // SMSG_SPLINE_MOVE_SET_RUN_MODE: MOVEFLAG_WALK_MODE off (Unit::SetWalk, MoveSplineInit::Launch)
    WalkMode, // SMSG_SPLINE_MOVE_SET_WALK_MODE: MOVEFLAG_WALK_MODE on
};
// The kind of an SMSG_SPLINE_MOVE_* opcode (false: not one).
bool splineMoveKind(Opcode opcode, SplineMoveKind& out);
Opcode splineMoveOpcode(SplineMoveKind kind);
// The unit's packed GUID, nothing else.
bool parseSplineMove(std::span<const u8> payload, u64& guid);
void writeSplineMove(std::vector<u8>& out, u64 guid);

// SMSG_SPLINE_SET_*_SPEED (SendSpeedChangeToAll, and SendSpeedChangeToObservers while a player-moved unit
// follows a spline): packed GUID + the new speed (yards or radians per second).
struct SplineSpeed {
    u64 guid = 0;
    f32 speed = 0.0f;
};
bool parseSplineSpeed(std::span<const u8> payload, SplineSpeed& out);
void writeSplineSpeed(std::vector<u8>& out, u64 guid, f32 speed);

// SMSG_MONSTER_MOVE_TRANSPORT (MoveSplineInit::Launch with a transport): the unit's packed GUID, the
// transport's packed GUID, then SMSG_MONSTER_MOVE's body after its GUID, every position in the transport's
// frame (the server's CalculatePassengerOffset; the facing angle too). WoW.exe 0x603F00 parses both opcodes
// with one function and puts the unit on that transport first (CMovement 0x617100; SMSG_MONSTER_MOVE takes it
// off any).
struct MonsterMoveTransportHeader {
    u64 unit = 0;
    u64 transport = 0;
    std::span<const u8> body; // from the start position on, as SMSG_MONSTER_MOVE's after the GUID
};
bool parseMonsterMoveTransportHeader(std::span<const u8> payload, MonsterMoveTransportHeader& out);

// A server spline as PacketBuilder::WriteMonsterMove writes it (the builder of the tests and the
// demonstration).
struct MonsterMove {
    // MonsterMoveType.
    enum class Facing : u8 { None = 0, Stop = 1, Spot = 2, Target = 3, Angle = 4 };
    u64 guid = 0;
    u64 transport = 0; // non-zero: SMSG_MONSTER_MOVE_TRANSPORT, the points in its frame
    glm::vec3 start{0.0f};
    u32 splineId = 0;
    Facing facing = Facing::None;
    glm::vec3 facingSpot{0.0f};
    u64 facingTarget = 0;
    f32 facingAngle = 0.0f;
    u32 flags = 0;    // MoveSplineFlag (0x100 Runmode, 0x200 Flying, 0x100000 Cyclic, 0x200000 Enter_Cycle)
    u32 duration = 0; // ms
    // The points after the start, the destination last. Linear splines write the destination and, when there
    // are points between, the start and those as packed offsets from it (WriteLinearPath: quarter yards,
    // 11/11/10 bits); flying ones every point in full.
    std::vector<glm::vec3> points;
};
void writeMonsterMove(std::vector<u8>& out, const MonsterMove& move);
Opcode monsterMoveOpcode(const MonsterMove& move);

} // namespace vd::net
