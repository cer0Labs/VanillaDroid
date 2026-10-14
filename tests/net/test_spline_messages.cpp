// Phase 18 (motion): the packets of the units the server moves, byte for byte as vmangos-core 4641790 writes
// them (MovementPacketSender.cpp, MoveSplineInit.cpp Launch, packet_builder.cpp WriteMonsterMove /
// WriteLinearPath).
#include "net/ByteBuffer.h"
#include "net/SplineMessages.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
using Bytes = std::vector<u8>;

Bytes bytesOf(f32 v) {
    Bytes out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
}

void append(Bytes& out, const Bytes& more) {
    out.insert(out.end(), more.begin(), more.end());
}

void appendU32(Bytes& out, u32 v) {
    for (u32 i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(v >> (i * 8)));
    }
}

void appendVec(Bytes& out, f32 x, f32 y, f32 z) {
    append(out, bytesOf(x));
    append(out, bytesOf(y));
    append(out, bytesOf(z));
}

// ByteBuffer::appendPackXYZ.
u32 pack(f32 x, f32 y, f32 z) {
    return (static_cast<u32>(std::lround(x * 4.0f)) & 0x7FF) |
           ((static_cast<u32>(std::lround(y * 4.0f)) & 0x7FF) << 11) |
           ((static_cast<u32>(std::lround(z * 4.0f)) & 0x3FF) << 22);
}

constexpr u64 kCreature = 0xF130000123000456ull;
// The packed GUID of kCreature: the mask of its non-zero bytes, then them.
const Bytes kPackedCreature = {0xDB, 0x56, 0x04, 0x23, 0x01, 0x30, 0xF1};

} // namespace

TEST_CASE("SMSG_SPLINE_MOVE_*: every opcode and its kind, the packed GUID alone") {
    const Opcode opcodes[] = {Opcode::SMSG_SPLINE_MOVE_ROOT,         Opcode::SMSG_SPLINE_MOVE_UNROOT,
                              Opcode::SMSG_SPLINE_MOVE_WATER_WALK,   Opcode::SMSG_SPLINE_MOVE_LAND_WALK,
                              Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL, Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL,
                              Opcode::SMSG_SPLINE_MOVE_SET_HOVER,    Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER,
                              Opcode::SMSG_SPLINE_MOVE_START_SWIM,   Opcode::SMSG_SPLINE_MOVE_STOP_SWIM,
                              Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE, Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE};
    for (u32 i = 0; i < std::size(opcodes); ++i) {
        net::SplineMoveKind kind{};
        REQUIRE(net::splineMoveKind(opcodes[i], kind));
        CHECK(static_cast<u32>(kind) == i);
        CHECK(net::splineMoveOpcode(kind) == opcodes[i]);
    }
    net::SplineMoveKind kind{};
    CHECK_FALSE(net::splineMoveKind(Opcode::SMSG_MONSTER_MOVE, kind));
    CHECK_FALSE(net::splineMoveKind(Opcode::SMSG_SPLINE_SET_RUN_SPEED, kind));
    // The 1.12.1 numbers (Opcodes_1_12_1.h): what WoW.exe registers 0x603C80 for.
    CHECK(static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_UNROOT) == 0x304);
    CHECK(static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE) == 0x30E);
    CHECK(static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_ROOT) == 0x31A);

    // SendToggleRunWalkToAll / SendMovementFlagChangeToAll: data << unit->GetPackGUID() and nothing more.
    Bytes packet;
    net::writeSplineMove(packet, kCreature);
    CHECK(packet == kPackedCreature);
    u64 guid = 0;
    REQUIRE(net::parseSplineMove(packet, guid));
    CHECK(guid == kCreature);
    CHECK_FALSE(net::parseSplineMove(std::span<const u8>(packet.data(), packet.size() - 1), guid));
    CHECK_FALSE(net::parseSplineMove({}, guid));
}

TEST_CASE("SMSG_SPLINE_SET_*_SPEED: the packed GUID and the speed") {
    // SendSpeedChangeToAll: data << GetPackGUID() << float(newRate * baseMoveSpeed[mtype]).
    Bytes expected = kPackedCreature;
    append(expected, bytesOf(3.5f));
    Bytes packet;
    net::writeSplineSpeed(packet, kCreature, 3.5f);
    CHECK(packet == expected);
    net::SplineSpeed speed;
    REQUIRE(net::parseSplineSpeed(packet, speed));
    CHECK(speed.guid == kCreature);
    CHECK(speed.speed == 3.5f);
    CHECK_FALSE(net::parseSplineSpeed(std::span<const u8>(packet.data(), packet.size() - 1), speed));
    Bytes nan = kPackedCreature;
    append(nan, bytesOf(std::nanf("")));
    CHECK_FALSE(net::parseSplineSpeed(nan, speed));
}

TEST_CASE("SMSG_MONSTER_MOVE as WriteMonsterMove writes it: linear, flying, stops, facings") {
    // A linear path with a point between: the count with the start, the destination, then the start and the
    // middle point as packed offsets back from the destination.
    net::MonsterMove move;
    move.guid = kCreature;
    move.start = {0.0f, 0.0f, 0.0f};
    move.splineId = 9;
    move.facing = net::MonsterMove::Facing::Angle;
    move.facingAngle = 1.5f;
    move.flags = 0x100 | 0x40000; // Runmode; the Final_Angle bit is not written (Mask_No_Monster_Move)
    move.duration = 2000;
    move.points = {{10.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 0.0f}};
    Bytes expected = kPackedCreature;
    appendVec(expected, 0.0f, 0.0f, 0.0f);
    appendU32(expected, 9);
    expected.push_back(4); // MonsterMoveFacingAngle
    append(expected, bytesOf(1.5f));
    appendU32(expected, 0x100);
    appendU32(expected, 2000);
    appendU32(expected, 3);
    appendVec(expected, 10.0f, 10.0f, 0.0f);
    appendU32(expected, pack(10.0f, 10.0f, 0.0f));
    appendU32(expected, pack(0.0f, 10.0f, 0.0f));
    Bytes packet;
    net::writeMonsterMove(packet, move);
    CHECK(packet == expected);
    CHECK(net::monsterMoveOpcode(move) == Opcode::SMSG_MONSTER_MOVE);

    // Start and destination only: "if (last_idx > 1)": no offsets at all.
    move.points = {{10.0f, 0.0f, 0.0f}};
    move.facing = net::MonsterMove::Facing::None;
    packet.clear();
    net::writeMonsterMove(packet, move);
    CHECK(packet.size() == kPackedCreature.size() + 12 + 4 + 1 + 4 + 4 + 4 + 12);

    // A zero offset is nudged up ("The client freezes when it gets a zero offset"): 0.26 yd, one quarter.
    move.points = {{0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}};
    move.start = {5.0f, 0.0f, 0.0f};
    packet.clear();
    net::writeMonsterMove(packet, move);
    u32 last = 0;
    std::memcpy(&last, packet.data() + packet.size() - 4, 4);
    CHECK(last == pack(5.0f, 0.0f, 0.0f));
    u32 first = 0;
    std::memcpy(&first, packet.data() + packet.size() - 8, 4);
    CHECK(first == pack(0.0f, 0.0f, 0.26f));

    // Flying (Mask_CatmullRom): every point after the start in full.
    net::MonsterMove fly;
    fly.guid = kCreature;
    fly.flags = 0x200 | 0x100;
    fly.duration = 1000;
    fly.facing = net::MonsterMove::Facing::Target;
    fly.facingTarget = 0x42;
    fly.points = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    expected = kPackedCreature;
    appendVec(expected, 0.0f, 0.0f, 0.0f);
    appendU32(expected, 0);
    expected.push_back(3); // MonsterMoveFacingTarget
    appendU32(expected, 0x42);
    appendU32(expected, 0);
    appendU32(expected, 0x300);
    appendU32(expected, 1000);
    appendU32(expected, 2);
    appendVec(expected, 1.0f, 2.0f, 3.0f);
    appendVec(expected, 4.0f, 5.0f, 6.0f);
    packet.clear();
    net::writeMonsterMove(packet, fly);
    CHECK(packet == expected);

    // The stop of MoveSplineInit::Launch: the position, the id, MonsterMoveStop and nothing more.
    net::MonsterMove stop;
    stop.guid = kCreature;
    stop.start = {7.0f, 8.0f, 9.0f};
    stop.splineId = 12;
    stop.facing = net::MonsterMove::Facing::Stop;
    expected = kPackedCreature;
    appendVec(expected, 7.0f, 8.0f, 9.0f);
    appendU32(expected, 12);
    expected.push_back(1);
    packet.clear();
    net::writeMonsterMove(packet, stop);
    CHECK(packet == expected);
}

TEST_CASE("SMSG_MONSTER_MOVE_TRANSPORT: the unit's GUID, the transport's, then SMSG_MONSTER_MOVE's body") {
    constexpr u64 kShip = 0x1FC0000000000014ull; // HIGHGUID_MO_TRANSPORT, entry 20
    net::MonsterMove move;
    move.guid = kCreature;
    move.transport = kShip;
    move.start = {1.0f, 2.0f, 6.0f};
    move.splineId = 3;
    move.duration = 800;
    move.points = {{3.0f, 2.0f, 6.0f}};
    Bytes packet;
    net::writeMonsterMove(packet, move);
    CHECK(net::monsterMoveOpcode(move) == Opcode::SMSG_MONSTER_MOVE_TRANSPORT);
    Bytes expected = kPackedCreature;
    append(expected, {0xC1, 0x14, 0xC0, 0x1F}); // the transport's packed GUID: its bytes 0, 6 and 7
    net::MonsterMove plain = move;
    plain.transport = 0;
    Bytes body;
    net::writeMonsterMove(body, plain);
    append(expected, Bytes(body.begin() + static_cast<std::ptrdiff_t>(kPackedCreature.size()), body.end()));
    CHECK(packet == expected);

    net::MonsterMoveTransportHeader header;
    REQUIRE(net::parseMonsterMoveTransportHeader(packet, header));
    CHECK(header.unit == kCreature);
    CHECK(header.transport == kShip);
    CHECK(header.body.size() == body.size() - kPackedCreature.size());
    CHECK(std::equal(header.body.begin(), header.body.end(),
                     body.begin() + static_cast<std::ptrdiff_t>(kPackedCreature.size())));
    // Cut in the GUIDs or before the move type: refused.
    CHECK_FALSE(net::parseMonsterMoveTransportHeader(std::span<const u8>(packet.data(), 9), header));
    CHECK_FALSE(net::parseMonsterMoveTransportHeader(
        std::span<const u8>(packet.data(), kPackedCreature.size() + 4 + 16), header));
}
