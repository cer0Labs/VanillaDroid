// Phase 18 (motion): how the units the client does not move move, as vmangos-core 4641790 tells it and
// WoW.exe 1.12.1 shows it: the SMSG_SPLINE_MOVE_* states and speeds in the entity store, the movement
// animation and the swim rule (game/UnitMotion), units walking on a transport (SMSG_MONSTER_MOVE_TRANSPORT,
// create blocks on board, other players on a deck), the change mask, and SMSG_CLEAR_FAR_SIGHT_IMMEDIATE.
#include "anim/Animator.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Transports.h"
#include "game/UnitMotion.h"
#include "net/ByteBuffer.h"
#include "net/SplineMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
namespace animation = anim::animation;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kWolf = 0xF130000001000001ull;
constexpr u64 kRogue = 0x0000000000000042ull;
constexpr u64 kShip = 0x1FC0000000000014ull;
constexpr f32 kPi = 3.14159265f;

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

// A living object's create block (UPDATEFLAG_LIVING | UPDATEFLAG_ALL): its movement, the six speeds, its
// fields.
Bytes created(u64 guid, game::ObjectType type, const net::MovementInfo& info, const Fields& fields,
              f32 walk = 2.5f, f32 run = 7.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x30);
    net::writeMovementInfo(w, info);
    for (const f32 speed : {walk, run, 4.5f, 4.722222f, 2.5f, 3.141594f}) {
        w.f32v(speed);
    }
    w.u32v(1);
    writeValues(w, fields);
    return out;
}

Bytes valuesOf(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

net::MovementInfo standing(f32 x, f32 y, f32 z = 0.0f, f32 o = 0.0f, u32 flags = 0) {
    net::MovementInfo info;
    info.flags = flags;
    info.position = {x, y, z, o};
    return info;
}

Bytes splineMove(u64 guid) {
    Bytes out;
    net::writeSplineMove(out, guid);
    return out;
}

Bytes monsterMove(u64 guid, glm::vec3 from, glm::vec3 to, u32 flags, u32 durationMs, u64 transport = 0) {
    net::MonsterMove move;
    move.guid = guid;
    move.transport = transport;
    move.start = from;
    move.splineId = 1;
    move.flags = flags;
    move.duration = durationMs;
    move.points = {to};
    Bytes out;
    net::writeMonsterMove(out, move);
    return out;
}

// A wolf standing at the origin, its store frame begun.
struct Wolf {
    game::EntityStore store;
    const game::Entity* e = nullptr;
    Wolf() {
        store.nextFrame();
        REQUIRE(
            store.applyUpdateObject(created(kWolf, game::ObjectType::Unit, standing(0.0f, 0.0f),
                                            {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_DISPLAYID, 165}}),
                                    1000));
        e = store.find(kWolf);
        REQUIRE(e);
    }
};

// Where the test's transport is: at (100, 200, 10), turned a quarter (its +X along the world's +Y), 0.002
// rad/ms.
bool testResolver(const void*, u64 transport, u32 nowMs, game::TransportPlacement& out) {
    if (transport != kShip) {
        return false;
    }
    out.position = {100.0f, 200.0f + static_cast<f32>(nowMs) * 0.001f, 10.0f};
    out.orientation = kPi * 0.5f;
    out.moving = true;
    return true;
}

struct Resolver {
    Resolver() { game::setTransportResolver(&testResolver, nullptr); }
    ~Resolver() { game::setTransportResolver(nullptr, nullptr); }
};

} // namespace

TEST_CASE("Walk and run mode, root, swimming and the other states of a unit the server moves") {
    Wolf w;
    // SMSG_SPLINE_MOVE_SET_WALK_MODE / _RUN_MODE (Unit::SetWalk): MOVEFLAG_WALK_MODE.
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::WalkMode, 1000));
    CHECK((w.e->moveFlags & 0x100) != 0);
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::RunMode, 1000));
    CHECK((w.e->moveFlags & 0x100) == 0);
    // Water walking, feather fall, hover: their flags on and off.
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::WaterWalk, 1000));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::FeatherFall, 1000));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::SetHover, 1000));
    CHECK((w.e->moveFlags & 0x70000000u) == 0x70000000u);
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::LandWalk, 1000));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::NormalFall, 1000));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::UnsetHover, 1000));
    CHECK((w.e->moveFlags & 0x70000000u) == 0);
    // Swimming; stopping takes the pitch with it (CMovement 0x7C6E80).
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::StartSwim, 1000));
    CHECK((w.e->moveFlags & 0x200000) != 0);
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::StopSwim, 1000));
    CHECK((w.e->moveFlags & 0x200000) == 0);
    CHECK(w.e->pitch == 0.0f);

    // Rooted half way along a 10 yd run of 1 s: it stays where it was (the spline ends there), facing its
    // way.
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {0, 10, 0}, 0x100, 1000), 2000));
    CHECK(w.e->onSpline(2500));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::Root, 2500));
    CHECK_FALSE(w.e->moving);
    CHECK((w.e->moveFlags & 0x1000) != 0);
    CHECK(w.e->positionAt(2500).y == doctest::Approx(5.0f));
    CHECK(w.e->positionAt(9000).y == doctest::Approx(5.0f));
    CHECK(w.e->facingAt(9000) == doctest::Approx(kPi * 0.5f));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::Unroot, 3000));
    CHECK((w.e->moveFlags & 0x1000) == 0);

    // Unknown units are skipped; a cut packet is refused.
    CHECK(w.store.applySplineMove(splineMove(0x99), net::SplineMoveKind::Root, 3000));
    CHECK_FALSE(w.store.applySplineMove({}, net::SplineMoveKind::Root, 3000));
}

TEST_CASE("A unit first seen half way along its path stops walking at its end (WoW.exe 0x7C6C00)") {
    // A Northshire peasant on its way to its tree: MOVEFLAG_FORWARD | WALK_MODE | SPLINE_ENABLED in the create block
    // (vMaNGOS BuildMovementUpdate), 1 s of a 2 s walk done, its work state already set (EMOTE_STATE_WORK_CHOPWOOD).
    const auto create = [](u32 elapsed) {
        Bytes out;
        net::ByteWriter c(out);
        c.u32v(1).u8v(0).u8v(2).packedGuid(kWolf).u8v(3).u8v(0x30);
        net::writeMovementInfo(c, standing(0.0f, 0.0f, 0.0f, 0.0f, 0x00400000 | 0x100 | 0x1));
        for (const f32 speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 3.14f}) {
            c.f32v(speed);
        }
        c.u32v(0).u32v(elapsed).u32v(2000).u32v(7).u32v(2); // no spline flags, elapsed, duration, id, 2 nodes
        c.f32v(-2.5f).f32v(0).f32v(0).f32v(2.5f).f32v(0).f32v(0);
        c.f32v(2.5f).f32v(0).f32v(0); // the destination
        c.u32v(1);
        writeValues(c, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_EMOTESTATE, 234}});
        return out;
    };
    game::EntityStore store;
    store.nextFrame();
    REQUIRE(store.applyUpdateObject(create(1000), 1000));
    const game::Entity* e = store.find(kWolf);
    REQUIRE(e);
    CHECK(game::movementAnimation(*e, 1500, false).moving);
    // At its tree: standing (its work state plays), and it stays there.
    CHECK_FALSE(game::movementAnimation(*e, 2500, false).moving);
    CHECK(game::movementAnimation(*e, 2500, false).animation == animation::kStand);
    CHECK(e->positionAt(9000).x == doctest::Approx(2.5f));

    // Created when its spline had already run out: where it ended, not sliding on.
    game::EntityStore late;
    late.nextFrame();
    REQUIRE(late.applyUpdateObject(create(2000), 5000));
    const game::Entity* l = late.find(kWolf);
    REQUIRE(l);
    CHECK_FALSE(game::movementAnimation(*l, 5000, false).moving);
    CHECK(l->positionAt(9000).x == doctest::Approx(l->positionAt(5000).x));
}

TEST_CASE("A speed or a mode changes what another player's movement carries forward from where it is") {
    game::EntityStore store;
    store.nextFrame();
    // Another player running north (+X) at 7 yd/s from the origin at 1000 ms.
    REQUIRE(store.applyUpdateObject(created(kRogue, game::ObjectType::Player,
                                            standing(0.0f, 0.0f, 0.0f, 0.0f, 0x1),
                                            {{uf::UNIT_FIELD_HEALTH, 100}}),
                                    1000));
    const game::Entity* e = store.find(kRogue);
    CHECK(e->positionAt(1500).x == doctest::Approx(3.5f));
    // Slowed to 3.5 yd/s at 1500: the half second already run stays run (CMovement 0x7C5CD0).
    Bytes slow;
    net::writeSplineSpeed(slow, kRogue, 3.5f);
    REQUIRE(store.applySplineSpeed(slow, game::SpeedType::Run, 1500));
    CHECK(e->speed(game::SpeedType::Run) == 3.5f);
    CHECK(e->positionAt(1500).x == doctest::Approx(3.5f));
    CHECK(e->positionAt(2000).x == doctest::Approx(5.25f));
    // Walk mode at 2000: on at 2.5 yd/s from 5.25.
    REQUIRE(store.applySplineMove(splineMove(kRogue), net::SplineMoveKind::WalkMode, 2000));
    CHECK(e->positionAt(2400).x == doctest::Approx(5.25f + 1.0f));
}

TEST_CASE(
    "WoW.exe's movement animation: walk or run by the speed, sprint, stealth, backwards, fly, swimming") {
    Wolf w;
    auto animationAt = [&](u32 nowMs, bool swimming = false) {
        return game::movementAnimation(*w.e, nowMs, swimming).animation;
    };
    // Standing: Stand, SwimIdle in the water.
    CHECK(animationAt(1000) == animation::kStand);
    CHECK(animationAt(1000, true) == animation::kSwimIdle);
    CHECK_FALSE(game::movementAnimation(*w.e, 1000, false).moving);
    // A walk spline (2.5 yd/s, no Runmode): Walk; a run one at 7: Run (more than twice the walk speed).
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {25, 0, 0}, 0, 10000), 2000));
    CHECK(animationAt(3000) == animation::kWalk);
    CHECK(game::currentSpeed(*w.e, 3000, false) == doctest::Approx(2.5f));
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {70, 0, 0}, 0x100, 10000), 2000));
    CHECK(animationAt(3000) == animation::kRun);
    CHECK(game::movementAnimation(*w.e, 3000, false).speed == doctest::Approx(7.0f));
    // Running slowed to 3.5 yd/s (Hamstring): it walks, whatever its spline's Runmode says.
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {35, 0, 0}, 0x100, 10000), 2000));
    CHECK(animationAt(3000) == animation::kWalk);
    // A walk speed raised to 4: 7 yd/s is no more than twice it... 8: Walk; 9 yd/s runs.
    Bytes walkSpeed;
    net::writeSplineSpeed(walkSpeed, kWolf, 4.0f);
    REQUIRE(w.store.applySplineSpeed(walkSpeed, game::SpeedType::Walk, 2000));
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {70, 0, 0}, 0x100, 10000), 2000));
    CHECK(animationAt(3000) == animation::kWalk);
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {90, 0, 0}, 0x100, 10000), 2000));
    CHECK(animationAt(3000) == animation::kRun);
    // From 11 yd/s: Sprint (a pet catching up with its master: FollowMovementGenerator, up to 2.1 times).
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {140, 0, 0}, 0x100, 10000), 2000));
    CHECK(animationAt(3000) == animation::kSprint);
    // A flying spline (MoveSplineFlag Flying: taxis): Fly.
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {70, 0, 5}, 0x300, 10000), 2000));
    CHECK(animationAt(3000) == animation::kFly);
    // Swimming along a spline: Swim.
    CHECK(animationAt(3000, true) == animation::kSwim);
    // Stealthed (UNIT_VIS_FLAGS_CREEP): StealthWalk, even running.
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {0, 0, 0}, {70, 0, 0}, 0x100, 10000), 2000));
    REQUIRE(w.store.applyUpdateObject(valuesOf(kWolf, {{uf::UNIT_FIELD_BYTES_1, 0x02000000}}), 2000));
    CHECK(animationAt(3000) == animation::kStealthWalk);
    // The spline over: standing again.
    CHECK(animationAt(20000) == animation::kStand);

    // Another player's own movement flags: backwards, strafing and turning in the water, walk mode.
    game::EntityStore store;
    store.nextFrame();
    REQUIRE(store.applyUpdateObject(created(kRogue, game::ObjectType::Player, standing(0, 0, 0, 0, 0x2), {}),
                                    1000));
    const game::Entity* p = store.find(kRogue);
    CHECK(game::movementAnimation(*p, 1100, false).animation == animation::kWalkBackwards);
    CHECK(game::currentSpeed(*p, 1100, false) == doctest::Approx(4.5f)); // the slower of run back and run
    CHECK(game::movementAnimation(*p, 1100, true).animation == animation::kSwimBackwards);
    REQUIRE(store.applyUpdateObject(
        created(kRogue, game::ObjectType::Player, standing(0, 0, 0, 0, 0x1 | 0x4), {}), 1000));
    p = store.find(kRogue);
    CHECK(game::movementAnimation(*p, 1100, true).animation == animation::kSwimLeft);
    REQUIRE(store.applyUpdateObject(
        created(kRogue, game::ObjectType::Player, standing(0, 0, 0, 0, 0x1 | 0x20), {}), 1000));
    p = store.find(kRogue);
    CHECK(game::movementAnimation(*p, 1100, true).animation == animation::kSwimIdle);
    REQUIRE(store.applyUpdateObject(
        created(kRogue, game::ObjectType::Player, standing(0, 0, 0, 0, 0x1 | 0x100), {}), 1000));
    p = store.find(kRogue);
    CHECK(game::currentSpeed(*p, 1100, false) == doctest::Approx(2.5f));
    CHECK(game::movementAnimation(*p, 1100, false).animation == animation::kWalk);
    // Rooted: no speed, standing.
    REQUIRE(store.applyUpdateObject(
        created(kRogue, game::ObjectType::Player, standing(0, 0, 0, 0, 0x1 | 0x1000), {}), 1000));
    CHECK(game::movementAnimation(*store.find(kRogue), 1100, false).animation == animation::kStand);
}

TEST_CASE("The swim rule of the client for the server's creatures") {
    Wolf w;
    // A 2.083 yd tall wolf swims past 0.75 x 2.083 - 1/36 = 1.535 yd of water, if it may swim.
    CHECK(game::swimDepth(2.083f) == doctest::Approx(1.5345f).epsilon(1e-3));
    CHECK_FALSE(game::unitSwims(*w.e, false, 1.6f, 2.083f)); // no UNIT_FLAG_USE_SWIM_ANIMATION: on the bottom
    REQUIRE(w.store.applyUpdateObject(valuesOf(kWolf, {{uf::UNIT_FIELD_FLAGS, 0x8000}}), 1000));
    CHECK_FALSE(game::unitSwims(*w.e, false, 1.5f, 2.083f));
    CHECK(game::unitSwims(*w.e, false, 1.6f, 2.083f));
    CHECK(game::unitSwims(*w.e, true, 1.6f, 2.083f));
    CHECK_FALSE(game::unitSwims(*w.e, true, 1.4f, 2.083f)); // out below the depth
    // A pet (UNIT_FLAG_PLAYER_CONTROLLED) swims too; walking on water, not.
    REQUIRE(w.store.applyUpdateObject(valuesOf(kWolf, {{uf::UNIT_FIELD_FLAGS, 0x8}}), 1000));
    CHECK(game::unitSwims(*w.e, false, 3.0f, 2.083f));
    REQUIRE(w.store.applySplineMove(splineMove(kWolf), net::SplineMoveKind::WaterWalk, 1000));
    CHECK_FALSE(game::unitSwims(*w.e, false, 3.0f, 2.083f));
}

TEST_CASE("Units walking on a transport: SMSG_MONSTER_MOVE_TRANSPORT, on board in the create block, a deck "
          "player") {
    Resolver resolver;
    Wolf w;
    // Its spline on the ship, from (2, 0, 6) to (2, 10, 6) of its frame in 1 s, facing the bow at the end.
    net::MonsterMove move;
    move.guid = kWolf;
    move.transport = kShip;
    move.start = {2.0f, 0.0f, 6.0f};
    move.splineId = 4;
    move.facing = net::MonsterMove::Facing::Angle;
    move.facingAngle = 0.0f;
    move.duration = 1000;
    move.points = {{2.0f, 10.0f, 6.0f}};
    Bytes packet;
    net::writeMonsterMove(packet, move);
    REQUIRE(w.store.applyMonsterMoveTransport(packet, 1000));
    CHECK(w.e->transportGuid == kShip);
    CHECK((w.e->moveFlags & 0x02000000) != 0); // MOVEFLAG_ONTRANSPORT
    // Half way at 1500: (2, 5, 6) on board; the ship turned a quarter and at (100, 201.5, 10).
    const glm::vec3 local = w.e->localAt(1500);
    CHECK(local.y == doctest::Approx(5.0f));
    const glm::vec3 world = w.e->positionAt(1500);
    CHECK(world.x == doctest::Approx(100.0f - 5.0f));
    CHECK(world.y == doctest::Approx(201.5f + 2.0f));
    CHECK(world.z == doctest::Approx(16.0f));
    f32 heading = 0.0f;
    REQUIRE(w.e->headingAt(1500, heading));
    CHECK(heading == doctest::Approx(kPi)); // its +Y along the deck is the world's -X
    // Arrived: it stays on its spot of the deck while the ship sails on, facing the bow (the ship's facing).
    CHECK(w.e->positionAt(5000).y == doctest::Approx(205.0f + 2.0f));
    CHECK(w.e->worldAngle(w.e->finalAngle, 5000) == doctest::Approx(kPi * 0.5f));
    // A plain SMSG_MONSTER_MOVE takes it off (CMovement 0x617100 with no transport).
    REQUIRE(w.store.applyMonsterMove(monsterMove(kWolf, {90, 200, 0}, {80, 200, 0}, 0x100, 1000), 6000));
    CHECK(w.e->transportGuid == 0);
    CHECK((w.e->moveFlags & 0x02000000) == 0);
    CHECK(w.e->positionAt(6500).x == doctest::Approx(85.0f));
    // A cut packet is refused.
    CHECK_FALSE(w.store.applyMonsterMoveTransport(std::span<const u8>(packet.data(), 12), 7000));

    // On board in its create block, walking a spline of the ship's frame (vMaNGOS WriteCreate: MoveSpline's
    // path).
    game::EntityStore store;
    store.nextFrame();
    Bytes create;
    net::ByteWriter c(create);
    c.u32v(1).u8v(0).u8v(2).packedGuid(kWolf).u8v(3).u8v(0x30);
    net::MovementInfo info = standing(110.0f, 300.0f, 16.0f, 0.0f, 0x02000000 | 0x00400000 | 0x1);
    info.transportGuid = kShip;
    info.transportPosition = {0.0f, 0.0f, 6.0f, 0.0f};
    net::writeMovementInfo(c, info);
    for (const f32 speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 3.14f}) {
        c.f32v(speed);
    }
    c.u32v(0x100).u32v(0).u32v(2000).u32v(5).u32v(2); // runmode, elapsed, duration, id, 2 nodes
    c.f32v(0).f32v(0).f32v(6).f32v(0).f32v(14).f32v(6);
    c.f32v(0).f32v(14).f32v(6); // the destination
    c.u32v(1);
    writeValues(c, {{uf::UNIT_FIELD_HEALTH, 10}});
    REQUIRE(store.applyUpdateObject(create, 2000));
    const game::Entity* e = store.find(kWolf);
    REQUIRE(e);
    CHECK(e->localAt(3000).y == doctest::Approx(7.0f));
    CHECK(e->positionAt(3000).x == doctest::Approx(100.0f - 7.0f));
    CHECK(e->positionAt(3000).y == doctest::Approx(203.0f));

    // Another player standing on the deck (MSG_MOVE_* with MOVEFLAG_ONTRANSPORT): carried and turned with the
    // ship.
    Bytes heartbeat;
    net::ByteWriter h(heartbeat);
    h.packedGuid(kRogue);
    net::MovementInfo deck = standing(97.0f, 202.0f, 16.0f, kPi * 0.5f + 1.0f, 0x02000000);
    deck.transportGuid = kShip;
    deck.transportPosition = {2.0f, 3.0f, 6.0f, 1.0f};
    net::writeMovementInfo(h, deck);
    REQUIRE(store.applyUpdateObject(created(kRogue, game::ObjectType::Player, standing(0, 0), {}), 2000));
    REQUIRE(store.applyMove(heartbeat, 2000));
    const game::Entity* rogue = store.find(kRogue);
    CHECK(rogue->positionAt(4000).x == doctest::Approx(100.0f - 3.0f));
    CHECK(rogue->positionAt(4000).y == doctest::Approx(204.0f + 2.0f));
    CHECK(rogue->facingAt(4000) == doctest::Approx(kPi * 0.5f + 1.0f));

    // Without a resolver (the transport unknown): where its last packet put it in the world.
    game::setTransportResolver(nullptr, nullptr);
    CHECK(rogue->positionAt(4000).x == doctest::Approx(97.0f));
}

TEST_CASE("The change mask: what changed in each frame, by update-mask word") {
    game::EntityStore store;
    store.nextFrame();
    const u32 first = store.frame();
    REQUIRE(store.applyUpdateObject(created(kWolf, game::ObjectType::Unit, standing(0, 0),
                                            {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_DISPLAYID, 165}}),
                                    1000));
    const game::Entity* e = store.find(kWolf);
    REQUIRE(store.changed().size() == 1);
    CHECK(store.changed()[0] == kWolf);
    CHECK(e->createdFrame == first);
    CHECK(e->changedIn(first, game::EntityStore::wordOf(uf::UNIT_FIELD_DISPLAYID))); // a creation is all new
    const u32 version = e->valuesVersion;

    // The next frame: its health (word 0), twice; the same display again changes nothing.
    store.nextFrame();
    CHECK(store.changed().empty());
    CHECK_FALSE(e->changedIn(store.frame(), ~u64{0}));
    REQUIRE(store.applyUpdateObject(
        valuesOf(kWolf, {{uf::UNIT_FIELD_HEALTH, 90}, {uf::UNIT_FIELD_DISPLAYID, 165}}), 1100));
    REQUIRE(store.applyUpdateObject(valuesOf(kWolf, {{uf::UNIT_FIELD_HEALTH, 80}}), 1100));
    REQUIRE(store.changed().size() == 1);
    CHECK(e->changedIn(store.frame(), game::EntityStore::wordOf(uf::UNIT_FIELD_HEALTH)));
    CHECK_FALSE(e->changedIn(store.frame(), game::EntityStore::wordOf(uf::UNIT_FIELD_DISPLAYID)));
    CHECK(e->valuesVersion > version);
    CHECK(game::EntityStore::wordOf(uf::UNIT_FIELD_HEALTH) == 1);
    CHECK(game::EntityStore::wordOf(uf::UNIT_FIELD_DISPLAYID) == (u64{1} << (uf::UNIT_FIELD_DISPLAYID / 32)));
    CHECK(game::EntityStore::wordOf(4000) == (u64{1} << 63)); // past 63 words: the last bit

    // A frame with only the same values: nothing changed. Then out of range: removed.
    store.nextFrame();
    const u32 unchanged = e->valuesVersion;
    REQUIRE(store.applyUpdateObject(valuesOf(kWolf, {{uf::UNIT_FIELD_HEALTH, 80}}), 1200));
    CHECK(store.changed().empty());
    CHECK(e->valuesVersion == unchanged);
    Bytes outOfRange;
    net::ByteWriter(outOfRange).u32v(1).u8v(0).u8v(4).u32v(1).packedGuid(kWolf);
    REQUIRE(store.applyUpdateObject(outOfRange, 1200));
    REQUIRE(store.removed().size() == 1);
    CHECK(store.removed()[0] == kWolf);
    store.nextFrame();
    CHECK(store.removed().empty());
}

TEST_CASE("SMSG_CLEAR_FAR_SIGHT_IMMEDIATE: the far sight field taken again at once") {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Opcode> sent;
    std::vector<Bytes> bodies;
    struct Context {
        std::vector<Opcode>* sent;
        std::vector<Bytes>* bodies;
    } context{&sent, &bodies};
    game::PacketSink sink;
    sink.context = &context;
    sink.send = [](void* c, Opcode opcode, std::span<const u8> body) {
        if (opcode == Opcode::CMSG_FAR_SIGHT) { // the login's queries aside
            static_cast<Context*>(c)->sent->push_back(opcode);
            static_cast<Context*>(c)->bodies->push_back(Bytes(body.begin(), body.end()));
        }
        return true;
    };
    play.setSink(sink);
    play.enterWorld(kRogue, 0, true, 1000);
    constexpr u64 kEye = 0xF100000000000777ull; // the Far Sight's dynamic object
    store.nextFrame();
    REQUIRE(store.applyUpdateObject(created(kRogue, game::ObjectType::Player, standing(0, 0), {}), 1000));
    Bytes eye;
    net::ByteWriter w(eye);
    w.u32v(1).u8v(0).u8v(2).packedGuid(kEye).u8v(6).u8v(0x40);
    w.f32v(500.0f).f32v(300.0f).f32v(20.0f).f32v(0.0f);
    writeValues(w, {{uf::DYNAMICOBJECT_SPELLID, 6196}});
    REQUIRE(store.applyUpdateObject(eye, 1000));
    REQUIRE(store.applyUpdateObject(
        valuesOf(kRogue, {{uf::PLAYER_FARSIGHT, static_cast<u32>(kEye)},
                          {static_cast<u16>(uf::PLAYER_FARSIGHT + 1), static_cast<u32>(kEye >> 32)}}),
        1000));
    play.update(1016);
    REQUIRE(sent == std::vector<Opcode>{Opcode::CMSG_FAR_SIGHT});
    CHECK(bodies[0] == Bytes{1});
    CHECK(play.control().viewTarget() == kEye);
    CHECK(play.control().viewingFarSight());
    // The field cleared, then SMSG_CLEAR_FAR_SIGHT_IMMEDIATE (no payload): back at once, CMSG_FAR_SIGHT 0.
    REQUIRE(store.applyUpdateObject(
        valuesOf(kRogue, {{uf::PLAYER_FARSIGHT, 0}, {static_cast<u16>(uf::PLAYER_FARSIGHT + 1), 0}}), 1020));
    REQUIRE(play.handle(Opcode::SMSG_CLEAR_FAR_SIGHT_IMMEDIATE, {}, 1020) == game::Gameplay::Handled::Yes);
    REQUIRE(sent.size() == 2);
    CHECK(sent[1] == Opcode::CMSG_FAR_SIGHT);
    CHECK(bodies[1] == Bytes{0});
    CHECK(play.control().viewTarget() == kRogue);
    CHECK_FALSE(play.control().viewingFarSight());
    // Not viewing: nothing.
    REQUIRE(play.handle(Opcode::SMSG_CLEAR_FAR_SIGHT_IMMEDIATE, {}, 1030) == game::Gameplay::Handled::Yes);
    CHECK(sent.size() == 2);
}
