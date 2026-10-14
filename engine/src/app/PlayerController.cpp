#include "app/PlayerController.h"

#include "anim/Animator.h"
#include "app/Crowd.h"
#include "app/GameSession.h"
#include "core/Log.h"
#include "game/MoverControl.h"
#include "game/Travel.h"
#include "net/UpdateFields.gen.h" // phase 18 (extras)
#include "core/Profile.h"
#include "physics/CollisionWorld.h"
#include "world/Actors.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using net::Opcode;
namespace mf = physics::moveflag;

constexpr f32 kPi = 3.14159265f;
constexpr f32 kMouseRadiansPerPixel = 0.005f;
constexpr f32 kStickDeadZone = 0.15f; // of the stick's travel
constexpr f32 kMaxPitch = 1.4f;
constexpr u32 kHeartbeatMs = 500;     // the client's heartbeat while moving (the server wants one per second at least)
constexpr u32 kFacingIntervalMs = 100; // turning alone: MSG_MOVE_SET_FACING at most this often
constexpr f32 kCameraRadius = 0.2f;
constexpr f32 kCameraMinDistance = 1.5f; // the closest third-person distance; nearer, the next step in is first person
// Phase 14: translucent while the camera is closer to the head than kCameraFadeDistance, down to WoW.exe's CVar
// PlayerFadeOutAlpha (128 of 255). Its PlayerFadeOut/InRate (4096) has no known unit: a quarter of a second each way.
constexpr f32 kPlayerFadeOutAlpha = 128.0f / 255.0f;
constexpr f32 kPlayerFadeRate = 2.0f; // opacity per second
constexpr f32 kCameraFadeDistance = 1.0f;
constexpr f32 kCameraEaseOut = 12.0f; // yards per second back out once a wall is gone
// Game controller: radial dead zones (XInput's recommended ones, as a fraction of the travel) and the camera's turn
// rates with the right stick fully pushed.
constexpr f32 kPadMoveDeadZone = 0.24f;
constexpr f32 kPadLookDeadZone = 0.27f;
constexpr f32 kPadYawRate = 3.0f;   // radians per second
constexpr f32 kPadPitchRate = 1.8f;

f32 wrapAngle(f32 a) {
    return std::remainder(a, 2.0f * kPi);
}

f32 positiveAngle(f32 a) {
    const f32 wrapped = std::fmod(a, 2.0f * kPi);
    return wrapped < 0.0f ? wrapped + 2.0f * kPi : wrapped;
}

// A stick without its dead zone, rescaled so the push still goes from 0 to 1 past it.
glm::vec2 stickPastDeadZone(f32 x, f32 y, f32 deadZone) {
    const glm::vec2 v(x, y);
    const f32 length = glm::length(v);
    if (length <= deadZone) {
        return glm::vec2(0.0f);
    }
    const f32 push = std::min(1.0f, (length - deadZone) / (1.0f - deadZone));
    return v * (push / length);
}

} // namespace

PlayerController::PlayerController() : m_cameraSet(4096) {}

void PlayerController::onKey(Key key, bool down) {
    if (key >= Key::Count) {
        return;
    }
    if (key == Key::Walk && down && !m_keys[static_cast<usize>(key)]) {
        m_walk = !m_walk;
    }
    m_keys[static_cast<usize>(key)] = down;
}

void PlayerController::onLook(f32 dxPixels, f32 dyPixels) {
    m_lookX += dxPixels;
    m_lookY += dyPixels;
}

void PlayerController::onWheel(f32 steps) {
    if (m_cameraDistance <= 0.0f) {
        if (steps < 0.0f) {
            m_cameraDistance = kCameraMinDistance; // out of first person: the closest third-person distance
        }
        return;
    }
    const f32 next = m_cameraDistance * std::pow(0.85f, steps);
    // Phase 14: zooming in past the closest distance enters first person (as the original's wheel does). Phase 15
    // (options): out to MAX_FOLLOW_DIST.
    m_cameraDistance =
        steps > 0.0f && next < kCameraMinDistance * 0.95f ? 0.0f : std::min(next, m_cameraOptions.maxDistance);
}

void PlayerController::releaseAll() {
    m_keys.fill(false);
    m_lookX = 0.0f;
    m_lookY = 0.0f;
    m_touchInput = {};
    m_gamepad = {};
}

void PlayerController::setShape(f32 radius, f32 height) {
    m_character.setShape(std::clamp(radius, 0.2f, 1.5f), std::clamp(height, 0.8f, 6.0f));
}

void PlayerController::spawnLocal(Actors& actors, const glm::dvec3& feet, f32 facing) {
    leave(&actors);
    m_active = true;
    m_online = false;
    m_waiting = true;
    m_character.place(feet, facing);
    m_cameraYaw = facing;
    if (actors.isOpen()) {
        m_actor = Crowd::randomLook(actors, m_lookSeed);
        m_actorYaw = facing;
    }
}

void PlayerController::enterWorld(const glm::dvec3& feet, f32 facing) {
    m_active = true;
    m_online = true;
    m_waiting = true;
    m_character.place(feet, facing);
    m_character.setRooted(false);
    m_character.speeds = physics::CharacterSpeeds{};
    m_cameraYaw = facing;
    m_sentFlags = 0;
    m_sentFacing = facing;
    m_onSpline = false;
    m_handledSpline = 0;
    m_transport = 0;
    m_boardPending = false;
    // Phase 13: the player moves itself in a new world (game::MoverControl::enterWorld).
    m_unit = 0;
    m_controlling = true;
    m_otherHandledSpline = 0;
    m_lastUpdateMs = 0;
    m_viewElsewhere = false;
}

void PlayerController::boardOnArrival(u64 transport, const glm::vec3& offset, f32 facing) {
    m_transport = transport;
    m_transportOffset = offset;
    m_transportOrientation = 0.0f;
    m_boardPending = true;
    m_waiting = true;
    m_character.place(m_character.position(), facing); // facing on the transport until it is known
    VD_LOG_INFO("Arriving on transport %llx at (%.2f, %.2f, %.2f) on it", static_cast<unsigned long long>(transport),
                offset.x, offset.y, offset.z);
}

void PlayerController::leave(Actors* actors) {
    if (m_actor != ~0u && actors) {
        actors->destroy(m_actor);
    }
    m_actor = ~0u;
    m_active = false;
    m_online = false;
    m_waiting = false;
}

physics::CharacterInput PlayerController::readInput(f32 /*contentScale*/) {
    physics::CharacterInput input;
    auto key = [&](Key k) { return m_keys[static_cast<usize>(k)]; };
    // Camera: mouse drag and the look side of the screen.
    f32 yawDelta = -m_lookX * kMouseRadiansPerPixel;
    f32 pitchDelta = -m_lookY * kMouseRadiansPerPixel;
    m_lookX = 0.0f;
    m_lookY = 0.0f;
    // Touch (the HUD's stick and camera area).
    yawDelta += m_touchInput.yawDelta;
    pitchDelta += m_touchInput.pitchDelta;
    if (m_touchInput.zoomSteps != 0.0f) {
        onWheel(m_touchInput.zoomSteps);
    }
    // Game controller: the right stick turns the camera at a rate (squared: fine aim near the center), the left one
    // adds to the touch stick.
    const glm::vec2 padLook = stickPastDeadZone(m_gamepad.rightX, m_gamepad.rightY, kPadLookDeadZone);
    const glm::vec2 padTurn = padLook * glm::length(padLook);
    yawDelta -= padTurn.x * kPadYawRate * m_dt;
    pitchDelta += padTurn.y * kPadPitchRate * m_dt;
    glm::vec2 stick = m_touchInput.stick + stickPastDeadZone(m_gamepad.leftX, m_gamepad.leftY, kPadMoveDeadZone);
    if (glm::length(stick) > 1.0f) {
        stick = glm::normalize(stick);
    }
    m_cameraYaw = wrapAngle(m_cameraYaw + yawDelta);
    m_cameraPitch = std::clamp(m_cameraPitch + pitchDelta, -kMaxPitch, kMaxPitch);

    input.facing = m_hasFacingGoal ? m_facingGoal : m_character.facing(); // movement input overrides it below
    m_hasFacingGoal = false;
    input.walk = m_walk;
    input.forward = key(Key::Forward);
    input.backward = key(Key::Back);
    input.strafeLeft = key(Key::Left);
    input.strafeRight = key(Key::Right);
    input.jump = key(Key::Jump) || m_touchInput.jump || (m_gamepad.buttons & gamepad::kA) != 0;
    if (input.forward || input.backward || input.strafeLeft || input.strafeRight) {
        input.facing = m_cameraYaw; // moving with the keys: the character turns with the camera
    }
    m_touchInput = {}; // one frame's worth
    const f32 push = glm::length(stick);
    if (push > kStickDeadZone) {
        // The stick points where to go, relative to the camera; the character turns there and runs (walks when
        // the stick is barely pushed). Up on the screen is the camera's forward.
        input.forward = true;
        input.backward = input.strafeLeft = input.strafeRight = false;
        input.facing = wrapAngle(m_cameraYaw + std::atan2(-stick.x, stick.y));
        input.walk = m_walk || push < 0.5f;
    }
    // Phase 15 (options): CAMERA_FOLLOWING_STYLE (setCameraOptions).
    if (yawDelta != 0.0f || pitchDelta != 0.0f) {
        m_stillCameraSeconds = 0.0f;
    } else {
        m_stillCameraSeconds += m_dt;
        const bool moving = input.forward || input.backward || input.strafeLeft || input.strafeRight;
        const f32 off = wrapAngle(input.facing - m_cameraYaw);
        constexpr f32 kFollowCone = 75.0f * kPi / 180.0f;
        const bool smart = m_cameraOptions.followStyle == 1 && moving && std::abs(off) <= kFollowCone;
        const bool always = m_cameraOptions.followStyle == 2 &&
                            ((moving && std::abs(off) <= kFollowCone) || (!moving && m_stillCameraSeconds > 1.0f));
        if (smart || always) {
            const f32 step = m_cameraOptions.followSpeed * (kPi / 180.0f) * m_dt;
            m_cameraYaw = wrapAngle(m_cameraYaw + std::clamp(off, -step, step));
        }
    }
    // Swimming heads where the camera looks, level around its resting pitch.
    const f32 pitch = m_cameraPitch + 0.3f;
    input.pitch = std::abs(pitch) < 0.15f ? 0.0f : pitch;
    // Phase 13 (social): auto-follow runs the character unless the player moves it, which ends the follow.
    if (m_followActive) {
        m_followActive = false;
        if (input.forward || input.backward || input.strafeLeft || input.strafeRight || input.jump) {
            m_followBroken = true;
        } else if (m_followMove) {
            input.forward = true;
            input.walk = false;
            input.facing = m_followFacing;
        }
    }
    return input;
}

bool PlayerController::moving() const {
    return m_active && (m_sentFlags & mf::kDirections) != 0;
}

net::MovementInfo PlayerController::movementInfo(u32 flags) const {
    net::MovementInfo info;
    info.flags = flags;
    info.time = m_clientTime;
    const glm::dvec3& p = m_character.position();
    info.position = {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z),
                     positiveAngle(m_character.facing())};
    info.pitch = m_character.pitch();
    if (m_transport != 0) { // phase 8: its place on the transport it rides
        info.flags |= mf::kOnTransport;
        info.transportGuid = m_transport;
        info.transportPosition = {m_transportOffset.x, m_transportOffset.y, m_transportOffset.z,
                                  positiveAngle(m_character.facing() - m_transportOrientation)};
    }
    if (flags & (mf::kJumping | mf::kFallingFar)) {
        info.fallTime = m_character.fallTimeMs();
        const physics::CharacterController::Jump& jump = m_character.jump();
        info.jumpVelocity = jump.zspeed;
        info.jumpCos = jump.cosAngle;
        info.jumpSin = jump.sinAngle;
        info.jumpXySpeed = jump.xySpeed;
    }
    return info;
}

void PlayerController::send(GameSession& session, Opcode opcode, u32 flags, u32 nowMs) {
    net::MovementInfo info = movementInfo(flags);
    // The packet that ends a flight carries the whole fall: FALL_LAND for the fall damage, and either of them for
    // MovementAnticheat::CheckNoFallTime (a fall-end packet with fallTime 0 after a second of flight is a cheat).
    if (opcode == Opcode::MSG_MOVE_FALL_LAND ||
        (opcode == Opcode::MSG_MOVE_START_SWIM && (m_sentFlags & (mf::kJumping | mf::kFallingFar)))) {
        info.fallTime = m_character.endedFallTimeMs();
    }
    session.sendMovement(opcode, info);
    if (traceMovement) {
        VD_LOG_DEBUG("Move %s flags 0x%x fall %u ms (%.2f, %.2f, %.2f) jump z %.2f xy %.2f", net::opcodeName(
                         static_cast<u32>(opcode)), flags, info.fallTime, info.position.x, info.position.y,
                     info.position.z, info.jumpVelocity, info.jumpXySpeed);
    }
    m_sentFlags = flags;
    m_sentFacing = m_character.facing();
    m_sentPitch = m_character.pitch();
    m_lastSendMs = nowMs;
    ++m_packets;
    m_heartbeats += opcode == Opcode::MSG_MOVE_HEARTBEAT ? 1 : 0;
}

// What changed since the last packet, in the order the server's checks accept (MovementAnticheat::CheckMoveStart:
// each direction flag appears with its own START opcode; a fall ends with FALL_LAND or START_SWIM).
void PlayerController::sendChanges(GameSession& session, u32 nowMs) {
    const u32 target = m_character.flags();
    const physics::CharacterController::Events& events = m_character.events();
    u32 sent = m_sentFlags;
    // Released directions.
    const u32 stopped = sent & (mf::kForward | mf::kBackward) & ~target;
    if (stopped) {
        sent &= ~stopped;
        send(session, Opcode::MSG_MOVE_STOP, sent, nowMs);
    }
    const u32 unstrafed = sent & (mf::kStrafeLeft | mf::kStrafeRight) & ~target;
    if (unstrafed) {
        sent &= ~unstrafed;
        send(session, Opcode::MSG_MOVE_STOP_STRAFE, sent, nowMs);
    }
    if ((sent ^ target) & mf::kWalkMode) {
        sent = (sent & ~mf::kWalkMode) | (target & mf::kWalkMode);
        send(session, (target & mf::kWalkMode) ? Opcode::MSG_MOVE_SET_WALK_MODE : Opcode::MSG_MOVE_SET_RUN_MODE, sent,
             nowMs);
    }
    // New directions, one per packet.
    static constexpr struct {
        u32 flag;
        Opcode opcode;
    } kStarts[] = {{mf::kForward, Opcode::MSG_MOVE_START_FORWARD},
                   {mf::kBackward, Opcode::MSG_MOVE_START_BACKWARD},
                   {mf::kStrafeLeft, Opcode::MSG_MOVE_START_STRAFE_LEFT},
                   {mf::kStrafeRight, Opcode::MSG_MOVE_START_STRAFE_RIGHT}};
    for (const auto& start : kStarts) {
        if ((target & start.flag) && !(sent & start.flag)) {
            sent |= start.flag;
            send(session, start.opcode, sent, nowMs);
        }
    }
    // Airborne and liquid transitions carry the whole state. A landing goes first: a new flight can start in the
    // same frame (a jump on touching the ground), and the server must see the old one end before the new one.
    if (events.landed) {
        const u32 airborne = mf::kJumping | mf::kFallingFar;
        send(session, Opcode::MSG_MOVE_FALL_LAND, (events.jumped || events.fell) ? target & ~airborne : target, nowMs);
        ++m_landings;
    }
    if (events.jumped) {
        send(session, Opcode::MSG_MOVE_JUMP, target, nowMs);
        ++m_jumps;
    } else if (events.fell) {
        send(session, Opcode::MSG_MOVE_HEARTBEAT, target, nowMs); // off a ledge: where the fall starts
    }
    if (events.startedSwim) {
        send(session, Opcode::MSG_MOVE_START_SWIM, target, nowMs);
    } else if (events.stoppedSwim) {
        send(session, Opcode::MSG_MOVE_STOP_SWIM, target, nowMs);
    }
    // Turning and pitching.
    const bool turned = std::abs(wrapAngle(m_character.facing() - m_sentFacing)) > 0.02f;
    const bool pitched = (target & mf::kSwimming) && std::abs(m_character.pitch() - m_sentPitch) > 0.05f;
    if ((turned || pitched) && nowMs - m_lastSendMs >= kFacingIntervalMs) {
        send(session, turned ? Opcode::MSG_MOVE_SET_FACING : Opcode::MSG_MOVE_SET_PITCH, target, nowMs);
    }
    // Heartbeat while moving (the server skips any gap over a second as a cheat).
    if ((target & mf::kMoving) && nowMs - m_lastSendMs >= kHeartbeatMs) {
        send(session, Opcode::MSG_MOVE_HEARTBEAT, target, nowMs);
    }
}

void PlayerController::applyCommand(GameSession& session, const MoveCommand& command, u32 nowMs) {
    const net::Position& to = command.info.position;
    if (command.guid != 0 && command.guid != m_unit) {
        // Phase 13: an order for the player while it moves another unit (or for the unit it just let go): answered
        // with that unit's place as the server knows it (the speed is kept on its entity by GameSession).
        if (const game::Entity* e = session.entities().find(command.guid); e && command.kind != MoveCommand::Kind::Correction) {
            net::MovementInfo info;
            info.flags = e->moveFlags & ~mf::kMoving;
            info.time = std::max(m_clientTime, nowMs);
            info.position = {e->position.x, e->position.y, e->position.z, positiveAngle(e->orientation)};
            session.acknowledge(command, info);
            ++m_acks;
        }
        return;
    }
    switch (command.kind) {
    case MoveCommand::Kind::Teleport:
        m_transport = 0;
        m_character.place(glm::dvec3(to.x, to.y, to.z), to.o);
        m_cameraYaw = to.o;
        m_waiting = true; // until the collision there is loaded
        m_teleportedMs = nowMs;
        m_sentFlags = m_character.flags();
        VD_LOG_INFO("Teleported to (%.2f, %.2f, %.2f)", to.x, to.y, to.z);
        break;
    case MoveCommand::Kind::Correction:
        m_character.place(glm::dvec3(to.x, to.y, to.z), to.o);
        m_sentFlags = command.info.flags; // what the server now holds: held keys start again from there
        ++m_corrections;
        return;
    case MoveCommand::Kind::Speed: {
        physics::CharacterSpeeds& s = m_character.speeds;
        switch (command.speed) {
        case game::SpeedType::Walk: s.walk = command.value; break;
        case game::SpeedType::Run: s.run = command.value; break;
        case game::SpeedType::RunBack: s.runBack = command.value; break;
        case game::SpeedType::Swim: s.swim = command.value; break;
        case game::SpeedType::SwimBack: s.swimBack = command.value; break;
        default: s.turnRate = command.value; break;
        }
        break;
    }
    case MoveCommand::Kind::Root: m_character.setRooted(true); break;
    case MoveCommand::Kind::Unroot: m_character.setRooted(false); break;
    case MoveCommand::Kind::Ability: m_character.setAbility(command.flag, command.apply); break;
    case MoveCommand::Kind::Knockback:
        m_character.launch(command.knockCos, command.knockSin, command.knockSpeedXY, command.knockSpeedZ);
        break;
    }
    m_clientTime = std::max(m_clientTime, nowMs);
    const net::MovementInfo info = movementInfo(m_character.flags());
    session.acknowledge(command, info);
    m_sentFlags = m_character.flags();
    m_sentFacing = m_character.facing();
    m_lastSendMs = nowMs;
    ++m_acks;
}

void PlayerController::animateLocal(Actors& actors) {
    if (m_actor == ~0u) {
        return;
    }
    const f32 speed = m_character.speed();
    const u32 flags = m_character.flags();
    u16 animation = anim::animation::kStand;
    switch (m_character.mode()) {
    case physics::MotionMode::Air:
        animation = (flags & mf::kFallingFar) ? anim::animation::kFall : anim::animation::kJump;
        break;
    case physics::MotionMode::Swim:
        animation = speed > 0.0f ? anim::animation::kSwim : anim::animation::kSwimIdle;
        break;
    case physics::MotionMode::Ground:
        if (speed > 0.0f) {
            animation = (flags & mf::kBackward) ? anim::animation::kWalkBackwards
                        : (flags & mf::kWalkMode) ? anim::animation::kWalk
                                                   : anim::animation::kRun;
        }
        break;
    }
    const f32 turn = wrapAngle(m_character.facing() - m_actorYaw);
    m_actorYaw = wrapAngle(m_actorYaw + std::clamp(turn, -0.4f, 0.4f));
    const game::ActorLook* look = actors.look(m_actor);
    actors.setPlacement(m_actor, m_character.position(), m_actorYaw, look ? look->scale : 1.0f);
    actors.play(m_actor, animation, true, 1.0f, m_character.mode() == physics::MotionMode::Air ? 0.0f : speed);
}

bool PlayerController::followServerSpline(GameSession& session, u32 nowMs) {
    const game::Entity* me = session.entities().find(m_unit);
    const game::ServerSplineStep step = me ? game::serverSplineStep(*me, handledSpline(), nowMs) : game::ServerSplineStep{};
    const f32 facing = step.hasFacing ? step.facing : m_character.facing();
    switch (step.kind) {
    case game::ServerSplineStep::Kind::Follow:
        if (!m_onSpline) {
            VD_LOG_INFO("The server moves the player: spline %u, %zu points, %u ms%s", step.splineId, me->path.size(),
                        me->splineDuration, me->segmentEnds.empty() ? "" : " (flying)");
        }
        m_onSpline = true;
        m_character.place(glm::dvec3(step.position), facing);
        // The camera drifts back behind the character (the player may still turn it).
        m_cameraYaw = wrapAngle(m_cameraYaw + std::clamp(wrapAngle(facing - m_cameraYaw), -1.5f * m_dt, 1.5f * m_dt));
        return true;
    case game::ServerSplineStep::Kind::Done:
        finishServerSpline(session, step.position, facing, step.splineId, nowMs);
        return false;
    default: m_onSpline = false; return false;
    }
}

void PlayerController::finishServerSpline(GameSession& session, const glm::vec3& at, f32 facing, u32 splineId,
                                          u32 nowMs) {
    m_onSpline = false;
    handledSpline() = splineId;
    m_character.place(glm::dvec3(at), facing);
    m_waiting = true; // until the collision there is loaded
    m_clientTime = std::max(m_clientTime, nowMs);
    // The server only accepts movement again once the client says where its copy of the spline ended
    // (MovementHandler.cpp HandleMoveSplineDoneOpcode; the anticheat wants it within 10 yards of the end).
    const net::MovementInfo info = movementInfo(0);
    session.sendSplineDone(info, splineId);
    session.setMoverMovement(m_unit, info, nowMs);
    m_sentFlags = 0;
    m_sentFacing = m_character.facing();
    m_lastSendMs = nowMs;
    ++m_splinesDone;
    VD_LOG_INFO("Spline %u done at (%.2f, %.2f, %.2f)", splineId, at.x, at.y, at.z);
}

void PlayerController::carryOnTransport() {
    if (m_transport == 0) {
        return;
    }
    game::TransportPlacement placement;
    if (!m_transportQuery.placement || !m_transportQuery.placement(m_transportQuery.context, m_transport, placement)) {
        VD_LOG_INFO("Transport %llx gone: off it", static_cast<unsigned long long>(m_transport));
        m_transport = 0; // it left sight (or changed maps without us)
        return;
    }
    const glm::vec3 world = game::transportToWorld(placement, m_transportOffset);
    const f32 turn = placement.orientation - m_transportOrientation;
    m_character.carry(glm::dvec3(world) - m_character.position(), turn);
    m_cameraYaw = wrapAngle(m_cameraYaw + turn);
    m_transportOrientation = placement.orientation;
}

void PlayerController::updateTransport(const physics::CollisionWorld& world, GameSession* session, u32 nowMs) {
    if (!m_transportQuery.transportOfObject) {
        return;
    }
    // What is under the feet: a transport's deck within a step below them.
    const glm::dvec3& p = m_character.position();
    const u32 object = world.objectBelow(p.x, p.y, p.z + 0.5, p.z - (m_character.stepHeight + 0.5));
    u64 under = object != physics::CollisionWorld::kNoObject
                    ? m_transportQuery.transportOfObject(m_transportQuery.context, object)
                    : 0;
    if (under == 0 && m_transport != 0 && m_character.mode() == physics::MotionMode::Air) {
        under = m_transport; // jumping on the deck: still aboard
    }
    game::TransportPlacement placement;
    if (under != 0 && m_transportQuery.placement(m_transportQuery.context, under, placement)) {
        if (under != m_transport) {
            ++m_boardings;
            VD_LOG_INFO("On transport %llx", static_cast<unsigned long long>(under));
        }
        const bool boarded = under != m_transport;
        m_transport = under;
        m_transportOffset = game::worldToTransport(placement, glm::vec3(p));
        m_transportOrientation = placement.orientation;
        if (boarded && m_online && session) {
            send(*session, Opcode::MSG_MOVE_HEARTBEAT, m_character.flags(), nowMs); // the server adds a passenger
        }
    } else if (m_transport != 0) {
        VD_LOG_INFO("Off transport %llx", static_cast<unsigned long long>(m_transport));
        m_transport = 0;
        if (m_online && session) {
            send(*session, Opcode::MSG_MOVE_HEARTBEAT, m_character.flags(), nowMs); // and removes it
        }
    }
}

void PlayerController::update(GameSession* session, Actors* actors, const physics::CollisionWorld& world, u32 nowMs,
                              f32 dt, f32 contentScale) {
    ++m_frame;
    m_dt = dt;
    if (!m_active) {
        return;
    }
    VD_PROFILE_ZONE("PlayerController::update");
    m_clientTime = std::max({m_clientTime, nowMs, 1u}); // the client's clock: never 0, never backwards
    const u32 elapsedMs = m_lastUpdateMs != 0 ? nowMs - m_lastUpdateMs : 0;
    m_lastUpdateMs = nowMs;
    if (m_online && session) {
        m_playerGuid = session->playerGuid();
        if (m_unit == 0) {
            m_unit = m_playerGuid; // a new world: the player moves itself
        }
        MoveCommand command;
        while (session->pollMoveCommand(command)) {
            applyCommand(*session, command, nowMs);
        }
        // Phase 13: fear, charms, Mind Control, Eyes of the Beast (game::MoverControl decides; the switch is told
        // here, before any movement of the new mover).
        game::MoverControl& control = session->play().control();
        if (control.switchPending()) {
            switchMover(*session, nowMs);
        }
        m_controlling = m_unit != 0 && m_unit == control.activeMover();
        // The camera looks through a far sight object the client does not move (Far Sight): there, not here.
        const game::Entity* view =
            control.viewTarget() != m_unit ? session->entities().find(control.viewTarget()) : nullptr;
        m_viewElsewhere = view && view->hasPosition;
        if (m_viewElsewhere) {
            m_viewPosition = glm::dvec3(view->positionAt(nowMs));
        }
        if (!m_controlling) {
            followUnit(*session, nowMs); // nothing simulated nor sent; the camera still turns
            readInput(contentScale);
            return;
        }
        if (elapsedMs > game::MoverControl::kMaxStepMs && !m_waiting && !m_onSpline) {
            // CMovement steps at most 250 ms: the rest of a long frame (an Android pause, a hitch) is reported.
            const u32 skipped = elapsedMs - game::MoverControl::kMaxStepMs;
            control.timeSkipped(skipped);
            ++m_timeSkips;
            m_skippedMs += skipped;
            VD_LOG_INFO("Movement time skipped: %u ms", skipped);
        }
        // Phase 8: on the server's spline (a taxi) the character only follows it; the camera still turns.
        if (followServerSpline(*session, nowMs)) {
            readInput(contentScale);
            return;
        }
    } else if (m_fakeLost && static_cast<i32>(nowMs - m_fakeLostUntilMs) >= 0) {
        m_fakeLost = false; // development: control comes back
    }
    if (m_boardPending) {
        // Arrived on a transport: stand on it once it is here and its deck collides.
        game::TransportPlacement placement;
        if (m_transportQuery.placement && m_transportQuery.placement(m_transportQuery.context, m_transport, placement) &&
            (!m_transportQuery.solid || m_transportQuery.solid(m_transportQuery.context, m_transport))) {
            m_boardPending = false;
            const f32 facing = wrapAngle(placement.orientation + m_character.facing());
            m_character.place(glm::dvec3(game::transportToWorld(placement, m_transportOffset)), facing);
            m_transportOrientation = placement.orientation;
            m_cameraYaw = facing;
            VD_LOG_INFO("Standing on transport %llx", static_cast<unsigned long long>(m_transport));
        }
    }
    const glm::dvec3& p = m_character.position();
    if (m_waiting) {
        m_waiting = m_boardPending || !world.isOpen() || !world.ready(p.x, p.y, 2.0f);
    }
    if (!m_waiting) {
        carryOnTransport();
        physics::CharacterInput input = drive.active ? drive.input : readInput(contentScale);
        // Phase 18 (extras): the jump key on a mount standing still on the ground rears the mount instead of jumping
        // (Jump, 0x60DEA0: CMSG_MOUNTSPECIAL_ANIM, once per press).
        const bool jumpKey = input.jump;
        if (m_online && session && jumpKey && m_unit == m_playerGuid && !input.forward && !input.backward &&
            !input.strafeLeft && !input.strafeRight && m_character.mode() == physics::MotionMode::Ground) {
            const game::Entity* me = session->entities().find(m_playerGuid);
            if (me && me->value(net::uf::UNIT_FIELD_MOUNTDISPLAYID) != 0) {
                if (!m_mountSpecialKey) {
                    session->play().mountSpecial();
                }
                input.jump = false;
            }
        }
        m_mountSpecialKey = jumpKey;
        if (m_fakeLost && !m_online) { // development (VD_FAKE_CONTROL): the input does nothing
            input = physics::CharacterInput{};
            input.facing = m_character.facing();
        }
        m_character.step(world, input, dt);
        updateTransport(world, m_online ? session : nullptr, nowMs);
        if (m_online && session) {
            sendChanges(*session, nowMs);
        }
    } else {
        readInput(contentScale); // the camera still turns
    }
    if (m_online && session) {
        session->setMoverMovement(m_unit, movementInfo(m_character.flags()), nowMs);
    }
    if (actors) {
        animateLocal(*actors);
    }
}

void PlayerController::switchMover(GameSession& session, u32 nowMs) {
    game::MoverControl& control = session.play().control();
    const u64 from = control.activeMover();
    const u64 to = control.wantedMover();
    game::MoverControl::Switch s;
    // The old mover's last movement, as the server knows it (the flags it was sent) at the place it is now.
    const net::MovementInfo oldInfo = movementInfo(m_sentFlags);
    if (from != 0 && from == m_unit && m_controlling) {
        s.oldInfo = &oldInfo;
        s.oldOnSpline = m_onSpline;
    }
    // The simulation goes to the new mover (the player again when control is lost: it follows the server there).
    const u64 unit = to != 0 ? to : m_playerGuid;
    const game::Entity* e = session.entities().find(unit);
    if (unit != m_unit) {
        if (e && e->hasPosition) {
            m_character.place(glm::dvec3(e->positionAt(nowMs)), e->orientation);
            physics::CharacterSpeeds& speeds = m_character.speeds;
            speeds.walk = e->speed(game::SpeedType::Walk);
            speeds.run = e->speed(game::SpeedType::Run);
            speeds.runBack = e->speed(game::SpeedType::RunBack);
            speeds.swim = e->speed(game::SpeedType::Swim);
            speeds.swimBack = e->speed(game::SpeedType::SwimBack);
            speeds.turnRate = e->speed(game::SpeedType::TurnRate);
            m_character.setRooted((e->moveFlags & mf::kRoot) != 0);
            for (const u32 flag : {mf::kWaterWalking, mf::kSafeFall, mf::kHover}) {
                m_character.setAbility(flag, (e->moveFlags & flag) != 0);
            }
            m_cameraYaw = e->orientation; // from behind it
        }
        m_otherHandledSpline = 0;
        m_transport = 0;
        m_onSpline = false;
        m_unit = unit;
    }
    // Its spline that ended while another unit was moved (the fear's last one, the stop of a possession) is answered
    // once the server knows it as the mover.
    net::MovementInfo newInfo;
    const game::ServerSplineStep step =
        e && to != 0 ? game::serverSplineStep(*e, handledSpline(), nowMs) : game::ServerSplineStep{};
    if (to != 0) {
        if (step.kind == game::ServerSplineStep::Kind::Done) {
            m_character.place(glm::dvec3(step.position), step.hasFacing ? step.facing : m_character.facing());
            s.newFinishedSpline = step.splineId;
            handledSpline() = step.splineId;
            ++m_splinesDone;
        }
        m_clientTime = std::max(m_clientTime, nowMs);
        newInfo = movementInfo(0);
        s.newInfo = &newInfo;
    }
    control.switchMover(s);
    m_controlling = to != 0;
    m_sentFlags = 0;
    m_sentFacing = m_character.facing();
    m_lastSendMs = nowMs;
    ++m_switches;
    // Its entity follows the simulation again (a spline still running is followed to its end first); without control
    // it stands where it is until the server moves it (not carried on by the flags it had: the fear's run comes).
    if (e && !e->moving) {
        session.setMoverMovement(m_unit, to != 0 ? newInfo : movementInfo(0), nowMs);
    }
}

void PlayerController::followUnit(GameSession& session, u32 nowMs) {
    const game::Entity* e = session.entities().find(m_unit);
    if (!e || !e->hasPosition) {
        return;
    }
    // Its spline (a fear's run, a charm's follow), or the movement another client sends for it (a Mind Control by a
    // player), as any other unit's.
    m_onSpline = e->moving;
    f32 facing = e->orientation;
    if (f32 heading = 0.0f; e->moving && e->headingAt(nowMs, heading)) {
        facing = heading;
    }
    m_character.place(glm::dvec3(e->positionAt(nowMs)), facing);
    // The camera drifts back behind the character (the player may still turn it).
    m_cameraYaw = wrapAngle(m_cameraYaw + std::clamp(wrapAngle(facing - m_cameraYaw), -1.5f * m_dt, 1.5f * m_dt));
}

void PlayerController::updateCamera(const physics::CollisionWorld& world, Camera& camera, f32 dt) {
    // Phase 13: through a far sight object the client does not move (PLAYER_FARSIGHT), else around the character.
    const glm::dvec3 target = m_viewElsewhere
                                  ? m_viewPosition + glm::dvec3(0.0, 0.0, 1.0)
                                  : m_character.position() + glm::dvec3(0.0, 0.0, m_character.height * 0.9);
    // Phase 15 (options): MAX_FOLLOW_DIST; FOLLOW_TERRAIN's tilt with the slope the character runs on (the climb over
    // the ground covered, smoothed); HEAD_BOB's sway of the first person view while moving.
    m_cameraDistance = std::min(m_cameraDistance, m_cameraOptions.maxDistance);
    const glm::dvec3 feet = m_character.position();
    const glm::dvec3 moved = feet - m_lastFeet;
    m_lastFeet = feet;
    const f64 run = std::sqrt(moved.x * moved.x + moved.y * moved.y);
    f32 slope = 0.0f;
    if (m_cameraOptions.terrainTilt && m_character.mode() == physics::MotionMode::Ground && run > 0.01 && run < 5.0) {
        slope = std::clamp(static_cast<f32>(std::atan2(moved.z, run)), -0.6f, 0.6f);
    }
    m_terrainTilt += (slope - m_terrainTilt) * std::min(1.0f, dt * 2.0f);
    const f32 viewPitch = std::clamp(m_cameraPitch + m_terrainTilt * 0.7f, -kMaxPitch, kMaxPitch);
    const glm::vec3 forward(std::cos(m_cameraYaw) * std::cos(viewPitch), std::sin(m_cameraYaw) * std::cos(viewPitch),
                            std::sin(viewPitch));
    f32 distance = m_cameraDistance;
    if (m_cameraCollision && world.isOpen()) {
        // A small sphere from the head back to the wanted spot: the camera stops before what it would enter
        // (terrain, buildings; not doodads, nor what the data marks as camera-transparent).
        const glm::vec3 back = -forward * m_cameraDistance;
        m_cameraSet.reset(target);
        const glm::dvec3 end = target + glm::dvec3(back);
        world.gather(glm::min(target, end) - glm::dvec3(1.0), glm::max(target, end) + glm::dvec3(1.0), m_cameraSet);
        physics::Hit hit;
        if (physics::sweepSphere(m_cameraSet, glm::vec3(0.0f), kCameraRadius, back, hit,
                                 pack::kCollisionNoCamera | pack::kCollisionDoodad)) {
            distance = std::max(0.3f, hit.t * m_cameraDistance - 0.05f);
        }
    }
    // In at once, out gently (no pumping when a wall passes by).
    m_shownDistance = distance < m_shownDistance ? distance : std::min(distance, m_shownDistance + kCameraEaseOut * dt);
    camera.yaw = m_cameraYaw;
    camera.pitch = viewPitch;
    camera.roll = 0.0f;
    camera.position = target - glm::dvec3(forward) * static_cast<f64>(m_shownDistance);
    if (m_cameraOptions.bobbing && m_cameraDistance <= 0.0f && m_character.mode() == physics::MotionMode::Ground &&
        run > 0.005) {
        m_bobPhase += static_cast<f32>(run) * 2.2f; // about a step a yard and a half
        camera.position.z += 0.04 * std::sin(static_cast<f64>(m_bobPhase));
    }
    // WATER_COLLISION: the camera stays above a water surface while the character is above it, under it while the
    // character swims under it.
    if (m_cameraOptions.waterCollision && world.isOpen()) {
        if (const std::optional<physics::LiquidSample> water =
                world.liquidAt(camera.position.x, camera.position.y, camera.position.z + 50.0, camera.position.z - 50.0)) {
            const f64 level = water->level;
            const bool characterUnder = target.z < level;
            if (!characterUnder && camera.position.z < level + 0.15) {
                camera.position.z = level + 0.15;
            } else if (characterUnder && camera.position.z > level - 0.15) {
                camera.position.z = level - 0.15;
            }
        }
    }
    // Phase 14: first person hides the character; a camera inside it shows it translucent.
    if (m_cameraDistance <= 0.0f && !m_viewElsewhere) {
        m_cameraFade = 0.0f;
    } else {
        const f32 wanted = m_shownDistance < kCameraFadeDistance && !m_viewElsewhere ? kPlayerFadeOutAlpha : 1.0f;
        const f32 from = std::max(m_cameraFade, kPlayerFadeOutAlpha);
        m_cameraFade = wanted < from ? std::max(wanted, from - kPlayerFadeRate * dt) : std::min(wanted, from + kPlayerFadeRate * dt);
    }
}

void PlayerController::drawPanel() {
    const glm::dvec3& p = m_character.position();
    static constexpr const char* kModes[] = {"ground", "air", "swimming"};
    ImGui::Text("%s%s | %s | (%.2f, %.2f, %.2f) | facing %.0f", m_online ? "online" : "local",
                m_waiting ? " (waiting for collision)" : "", kModes[static_cast<u32>(m_character.mode())], p.x, p.y,
                p.z, positiveAngle(m_character.facing()) * 57.29578f);
    ImGui::Text("flags 0x%08X | speed %.2f | fall %u ms | shape r %.2f h %.2f", m_character.flags(),
                m_character.speed(), m_character.fallTimeMs(), m_character.radius, m_character.height);
    if (m_online) {
        ImGui::Text("packets %u (heartbeats %u, jumps %u, landings %u) | acks %u | server corrections %u",
                    m_packets, m_heartbeats, m_jumps, m_landings, m_acks, m_corrections);
        ImGui::Text("server splines: %s | done %u", m_onSpline ? "following one" : "none",
                    m_splinesDone);
        ImGui::Text("moves %llx%s | mover switches %u | time skipped %u (%u ms)",
                    static_cast<unsigned long long>(m_unit), m_controlling ? "" : " (no control)", m_switches,
                    m_timeSkips, m_skippedMs);
    }
    if (m_transport != 0) {
        ImGui::Text("transport %llx: (%.2f, %.2f, %.2f) on its deck | boardings %u",
                    static_cast<unsigned long long>(m_transport), m_transportOffset.x, m_transportOffset.y,
                    m_transportOffset.z, m_boardings);
    }
    ImGui::Checkbox("Camera collision", &m_cameraCollision);
    ImGui::SameLine();
    ImGui::Checkbox("Walk", &m_walk);
}

} // namespace vd
