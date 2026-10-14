#include "net/SplineMessages.h"

#include "net/ByteBuffer.h"

#include <cmath>

namespace vd::net {
namespace {

struct KindOpcode {
    SplineMoveKind kind;
    Opcode opcode;
};
constexpr KindOpcode kKinds[] = {
    {SplineMoveKind::Root, Opcode::SMSG_SPLINE_MOVE_ROOT},
    {SplineMoveKind::Unroot, Opcode::SMSG_SPLINE_MOVE_UNROOT},
    {SplineMoveKind::WaterWalk, Opcode::SMSG_SPLINE_MOVE_WATER_WALK},
    {SplineMoveKind::LandWalk, Opcode::SMSG_SPLINE_MOVE_LAND_WALK},
    {SplineMoveKind::FeatherFall, Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL},
    {SplineMoveKind::NormalFall, Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL},
    {SplineMoveKind::SetHover, Opcode::SMSG_SPLINE_MOVE_SET_HOVER},
    {SplineMoveKind::UnsetHover, Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER},
    {SplineMoveKind::StartSwim, Opcode::SMSG_SPLINE_MOVE_START_SWIM},
    {SplineMoveKind::StopSwim, Opcode::SMSG_SPLINE_MOVE_STOP_SWIM},
    {SplineMoveKind::RunMode, Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE},
    {SplineMoveKind::WalkMode, Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE},
};

void writeVec3(ByteWriter& w, const glm::vec3& v) {
    w.f32v(v.x).f32v(v.y).f32v(v.z);
}

// ByteBuffer::appendPackXYZ: quarter yards in 11, 11 and 10 bits.
u32 packXyz(glm::vec3 offset) {
    // WriteLinearPath: "The client freezes when it gets a zero offset".
    if (std::fabs(offset.x) < 0.25f && std::fabs(offset.y) < 0.25f && std::fabs(offset.z) < 0.25f) {
        offset.z += offset.z < 0.0f ? 0.51f : 0.26f;
    }
    u32 packed = static_cast<u32>(std::lround(offset.x * 4.0f)) & 0x7FFu;
    packed |= (static_cast<u32>(std::lround(offset.y * 4.0f)) & 0x7FFu) << 11;
    packed |= (static_cast<u32>(std::lround(offset.z * 4.0f)) & 0x3FFu) << 22;
    return packed;
}

constexpr u32 kSplineFlying = 0x00000200;  // MoveSplineFlag::Flying (Mask_CatmullRom)
constexpr u32 kSplineCyclic = 0x00100000;  // MoveSplineFlag::Cyclic
constexpr u32 kSplineFacings = 0x00070000; // Final_Point | Final_Target | Final_Angle (Mask_Final_Facing)
constexpr u32 kSplineDone = 0x00000001;    // MoveSplineFlag::Done (Mask_No_Monster_Move with the facings)

} // namespace

bool splineMoveKind(Opcode opcode, SplineMoveKind& out) {
    for (const KindOpcode& k : kKinds) {
        if (k.opcode == opcode) {
            out = k.kind;
            return true;
        }
    }
    return false;
}

Opcode splineMoveOpcode(SplineMoveKind kind) {
    for (const KindOpcode& k : kKinds) {
        if (k.kind == kind) {
            return k.opcode;
        }
    }
    return Opcode::SMSG_SPLINE_MOVE_ROOT;
}

bool parseSplineMove(std::span<const u8> payload, u64& guid) {
    ByteReader r(payload);
    guid = r.packedGuid();
    return r.ok();
}

void writeSplineMove(std::vector<u8>& out, u64 guid) {
    ByteWriter(out).packedGuid(guid);
}

bool parseSplineSpeed(std::span<const u8> payload, SplineSpeed& out) {
    ByteReader r(payload);
    out.guid = r.packedGuid();
    out.speed = r.f32v();
    return r.ok() && std::isfinite(out.speed);
}

void writeSplineSpeed(std::vector<u8>& out, u64 guid, f32 speed) {
    ByteWriter(out).packedGuid(guid).f32v(speed);
}

bool parseMonsterMoveTransportHeader(std::span<const u8> payload, MonsterMoveTransportHeader& out) {
    ByteReader r(payload);
    out.unit = r.packedGuid();
    out.transport = r.packedGuid();
    out.body = r.rest();
    // The body has at least its start position, spline id and move type.
    return r.ok() && out.body.size() >= 17;
}

Opcode monsterMoveOpcode(const MonsterMove& move) {
    return move.transport != 0 ? Opcode::SMSG_MONSTER_MOVE_TRANSPORT : Opcode::SMSG_MONSTER_MOVE;
}

void writeMonsterMove(std::vector<u8>& out, const MonsterMove& move) {
    ByteWriter w(out);
    w.packedGuid(move.guid);
    if (move.transport != 0) {
        w.packedGuid(move.transport);
    }
    writeVec3(w, move.start);
    w.u32v(move.splineId);
    w.u8v(static_cast<u8>(move.facing));
    switch (move.facing) {
    case MonsterMove::Facing::Stop: return; // MoveSplineInit::Launch's stop packet ends here
    case MonsterMove::Facing::Spot: writeVec3(w, move.facingSpot); break;
    case MonsterMove::Facing::Target: w.u64v(move.facingTarget); break;
    case MonsterMove::Facing::Angle: w.f32v(move.facingAngle); break;
    default: break;
    }
    // WriteCommonMonsterMovePart: the flags without the facings and Done (Mask_No_Monster_Move).
    w.u32v(move.flags & ~(kSplineFacings | kSplineDone));
    w.u32v(move.duration);
    if (move.flags & kSplineFlying) {
        // WriteCatmullRomCyclicPath: a fake first point, erased after a cycle.
        if (move.flags & kSplineCyclic) {
            w.u32v(static_cast<u32>(move.points.size() + 1));
            writeVec3(w, move.start);
        } else { // WriteCatmullRomPath
            w.u32v(static_cast<u32>(move.points.size()));
        }
        for (const glm::vec3& p : move.points) {
            writeVec3(w, p);
        }
        return;
    }
    // WriteLinearPath: the point count with the start, the destination, then (with a point between them)
    // every point before it, the start first, as an offset back from it.
    if (move.points.empty()) {
        w.u32v(0);
        return;
    }
    const glm::vec3& destination = move.points.back();
    w.u32v(static_cast<u32>(move.points.size() + 1));
    writeVec3(w, destination);
    if (move.points.size() < 2) {
        return;
    }
    w.u32v(packXyz(destination - move.start));
    for (usize i = 0; i + 1 < move.points.size(); ++i) {
        w.u32v(packXyz(destination - move.points[i]));
    }
}

} // namespace vd::net
