#include "game/Entities.h"

#include "game/Transports.h"
#include "net/ByteBuffer.h"
#include "net/UpdateFields.gen.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace vd::game {
namespace {

// UPDATETYPE_* and UPDATEFLAG_* (src/game/Objects/UpdateData.h, "checked for 1.12.1").
constexpr u8 kUpdateValues = 0;
constexpr u8 kUpdateMovement = 1;
constexpr u8 kUpdateCreate = 2;
constexpr u8 kUpdateCreate2 = 3;
constexpr u8 kUpdateOutOfRange = 4;
constexpr u8 kUpdateNear = 5;

constexpr u8 kFlagTransport = 0x02;
constexpr u8 kFlagMeleeAttacking = 0x04;
constexpr u8 kFlagHighGuid = 0x08;
constexpr u8 kFlagAll = 0x10;
constexpr u8 kFlagLiving = 0x20;
constexpr u8 kFlagHasPosition = 0x40;

// MoveSplineFlag (src/game/Movement/spline/MoveSplineFlag.h).
constexpr u32 kSplineRunMode = 0x00000100;
constexpr u32 kSplineFlying = 0x00000200; // Mask_CatmullRom: every point written in full
constexpr u32 kSplineFinalPoint = 0x00010000;
constexpr u32 kSplineFinalTarget = 0x00020000;
constexpr u32 kSplineFinalAngle = 0x00040000;
constexpr u32 kSplineCyclic = 0x00100000;
constexpr u32 kSplineEnterCycle = 0x00200000; // the first point is a fake one, dropped after the first loop

// MOVEFLAG_* (MovementInfo.h).
constexpr u32 kMoveForward = 0x1;
constexpr u32 kMoveBackward = 0x2;
constexpr u32 kMoveStrafeLeft = 0x4;
constexpr u32 kMoveStrafeRight = 0x8;
constexpr u32 kMoveWalkMode = 0x100;
constexpr u32 kMoveRoot = 0x1000;
constexpr u32 kMoveJumping = 0x2000;
constexpr u32 kMoveFallingFar = 0x4000;
constexpr u32 kMoveSwimming = 0x200000;
constexpr u32 kMoveDirections = kMoveForward | kMoveBackward | kMoveStrafeLeft | kMoveStrafeRight;
// Phase 18 (motion).
constexpr u32 kMovePitches = 0x40 | 0x80;          // MOVEFLAG_PITCH_UP | MOVEFLAG_PITCH_DOWN
constexpr u32 kMoveWaterWalking = 0x10000000;      // MOVEFLAG_WATERWALKING
constexpr u32 kMoveSafeFall = 0x20000000;          // MOVEFLAG_SAFE_FALL
constexpr u32 kMoveHover = 0x40000000;             // MOVEFLAG_HOVER
constexpr u32 kMoveRootClears = 0x000000FF | 0x001F8000; // CMovement 0x7C7340: directions, turns, pitch, "pending"
constexpr u32 kExtrapolateMs = 1000;
constexpr f32 kMaxCorrection = 8.0f; // yards: further means a teleport, shown at once

// Falls as the client computes them (vMaNGOS src/game/Movement/spline/util.cpp).
constexpr f32 kGravity = 19.29110527f;
constexpr f32 kTerminalVelocity = 60.148003f;

// MonsterMoveType (packet_builder.cpp).
constexpr u8 kMonsterMoveStop = 1;
constexpr u8 kMonsterMoveFacingSpot = 2;
constexpr u8 kMonsterMoveFacingTarget = 3;
constexpr u8 kMonsterMoveFacingAngle = 4;

u32 valueCount(ObjectType type) {
    switch (type) {
    case ObjectType::Item: return net::uf::ITEM_END;
    case ObjectType::Container: return net::uf::CONTAINER_END;
    case ObjectType::Unit: return net::uf::UNIT_END;
    case ObjectType::Player: return net::uf::PLAYER_END;
    case ObjectType::GameObject: return net::uf::GAMEOBJECT_END;
    case ObjectType::DynamicObject: return net::uf::DYNAMICOBJECT_END;
    case ObjectType::Corpse: return net::uf::CORPSE_END;
    default: return net::uf::OBJECT_END;
    }
}

glm::vec3 readVec3(net::ByteReader& r) {
    const f32 x = r.f32v();
    const f32 y = r.f32v();
    const f32 z = r.f32v();
    return {x, y, z};
}

// ByteBuffer::appendPackXYZ: 11, 11 and 10 bit signed quarter-yards.
glm::vec3 unpackXyz(u32 packed) {
    const i32 x = static_cast<i32>(packed << 21) >> 21;
    const i32 y = static_cast<i32>(packed << 10) >> 21;
    const i32 z = static_cast<i32>(packed) >> 22;
    return glm::vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)) * 0.25f;
}

// Height after `t` seconds of a fall that started with vertical speed `v` (up > 0), capped at terminal speed.
f32 fallDisplacement(f32 v, f32 t) {
    const f32 toTerminal = (v + kTerminalVelocity) / kGravity; // time until -terminal is reached
    if (t <= toTerminal) {
        return v * t - 0.5f * kGravity * t * t;
    }
    return v * toTerminal - 0.5f * kGravity * toTerminal * toTerminal - kTerminalVelocity * (t - toTerminal);
}

// Catmull-Rom of the server's splines (Movement/spline/spline.cpp s_catmullRomCoeffs) over the segment p[1]-p[2].
glm::vec3 catmullRom(const glm::vec3 (&p)[4], f32 t) {
    const f32 t2 = t * t, t3 = t2 * t;
    return p[0] * (-0.5f * t3 + t2 - 0.5f * t) + p[1] * (1.5f * t3 - 2.5f * t2 + 1.0f) +
           p[2] * (-1.5f * t3 + 2.0f * t2 + 0.5f * t) + p[3] * (0.5f * t3 - 0.5f * t2);
}

glm::vec3 catmullRomDerivative(const glm::vec3 (&p)[4], f32 t) {
    const f32 t2 = t * t;
    return p[0] * (-1.5f * t2 + 2.0f * t - 0.5f) + p[1] * (4.5f * t2 - 5.0f * t) +
           p[2] * (-4.5f * t2 + 4.0f * t + 0.5f) + p[3] * (1.5f * t2 - t);
}

// The four control points of segment `i` (path[i] to path[i + 1]), with the server's virtual end points
// (SplineBase::InitCatmullRom: the first mirrored, the last repeated).
void segmentControls(const std::vector<glm::vec3>& path, usize i, glm::vec3 (&p)[4]) {
    const usize n = path.size();
    p[0] = i == 0 ? path[0] * 2.0f - path[1] : path[i - 1];
    p[1] = path[i];
    p[2] = path[i + 1];
    p[3] = i + 2 < n ? path[i + 2] : path[n - 1];
}

void setPathLength(Entity& e) {
    e.pathLength = 0.0f;
    e.segmentEnds.clear();
    const bool smooth = (e.splineFlags & kSplineFlying) && !e.cyclic && e.path.size() >= 2;
    if (!smooth) {
        for (usize i = 1; i < e.path.size(); ++i) {
            e.pathLength += glm::length(e.path[i] - e.path[i - 1]);
        }
        return;
    }
    // Segment lengths as SplineBase::SegLengthCatmullRom measures them (3 steps each).
    for (usize i = 0; i + 1 < e.path.size(); ++i) {
        glm::vec3 p[4];
        segmentControls(e.path, i, p);
        glm::vec3 previous = p[1];
        for (u32 step = 1; step <= 3; ++step) {
            const glm::vec3 next = catmullRom(p, static_cast<f32>(step) / 3.0f);
            e.pathLength += glm::length(next - previous);
            previous = next;
        }
        e.segmentEnds.push_back(e.pathLength);
    }
    for (f32& end : e.segmentEnds) {
        end = e.pathLength > 1e-4f ? end / e.pathLength : 1.0f;
    }
}

// Point at `progress` (0..1 of the duration) of a smooth spline, and its direction.
glm::vec3 alongSmoothPath(const Entity& e, f32 progress, glm::vec3* direction = nullptr) {
    usize i = 0;
    while (i + 1 < e.segmentEnds.size() && progress > e.segmentEnds[i]) {
        ++i;
    }
    const f32 start = i == 0 ? 0.0f : e.segmentEnds[i - 1];
    const f32 span = e.segmentEnds[i] - start;
    const f32 u = span > 1e-6f ? std::clamp((progress - start) / span, 0.0f, 1.0f) : 1.0f;
    glm::vec3 p[4];
    segmentControls(e.path, i, p);
    if (direction) {
        const glm::vec3 d = catmullRomDerivative(p, u);
        const f32 length = glm::length(d);
        if (length > 1e-4f) {
            *direction = d / length;
        }
    }
    return catmullRom(p, u);
}

// Point `distance` yards along the path, and the direction of the segment it is on.
glm::vec3 alongPath(const std::vector<glm::vec3>& path, f32 distance, glm::vec3* direction = nullptr) {
    for (usize i = 1; i < path.size(); ++i) {
        const glm::vec3 segment = path[i] - path[i - 1];
        const f32 length = glm::length(segment);
        if (distance <= length || i + 1 == path.size()) {
            if (direction && length > 1e-4f) {
                *direction = segment / length;
            }
            return length > 1e-4f ? path[i - 1] + segment * std::min(1.0f, distance / length) : path[i];
        }
        distance -= length;
    }
    return path.empty() ? glm::vec3(0.0f) : path.back();
}

// Fraction of the spline done at `nowMs`, looping for cyclic ones.
f32 splineProgress(const Entity& e, u32 nowMs) {
    const f32 t = static_cast<f32>(nowMs - e.splineStartMs) / static_cast<f32>(e.splineDuration);
    if (e.cyclic) {
        return t - std::floor(t);
    }
    return std::clamp(t, 0.0f, 1.0f);
}

bool splineActive(const Entity& e, u32 nowMs) {
    return e.moving && e.splineDuration != 0 && e.path.size() >= 2 &&
           (e.cyclic || nowMs - e.splineStartMs < e.splineDuration);
}

// Direction its spline goes at `nowMs` (radians in its own frame), false when it is not on one.
bool splineHeading(const Entity& e, u32 nowMs, f32& heading) {
    if (!splineActive(e, nowMs)) {
        return false;
    }
    glm::vec3 direction(0.0f);
    if (!e.segmentEnds.empty()) {
        alongSmoothPath(e, splineProgress(e, nowMs), &direction);
    } else {
        alongPath(e.path, splineProgress(e, nowMs) * e.pathLength, &direction);
    }
    if (direction.x * direction.x + direction.y * direction.y < 1e-6f) {
        return false;
    }
    heading = std::atan2(direction.y, direction.x);
    return true;
}

// Phase 18 (motion): the transports' places (app/Transports), for the passengers.
TransportResolver g_resolver = nullptr;
const void* g_resolverContext = nullptr;

bool resolveTransport(u64 transport, u32 nowMs, TransportPlacement& out) {
    return g_resolver && transport != 0 && g_resolver(g_resolverContext, transport, nowMs, out);
}

// Where its last MSG_MOVE_* carries it at `nowMs` from `origin` facing `facing` (dead reckoning in the frame of
// both): momentum and gravity in the air, its directions and speed on the ground or in the water.
glm::vec3 reckon(const Entity& e, const glm::vec3& origin, f32 facing, u32 nowMs) {
    if ((e.moveFlags & kMoveRoot) || nowMs < e.moveTimeMs) {
        return origin;
    }
    const f32 seconds = static_cast<f32>(std::min(nowMs - e.moveTimeMs, kExtrapolateMs)) / 1000.0f;
    if (e.moveFlags & kMoveJumping) {
        // Momentum and gravity, as the client computes a jump or a fall.
        return origin + glm::vec3(e.airVelocity * seconds, fallDisplacement(e.verticalSpeed, seconds));
    }
    if ((e.moveFlags & kMoveDirections) == 0) {
        return origin;
    }
    // Dead reckoning between the heartbeats of a moving player.
    glm::vec2 direction(0.0f);
    const glm::vec2 forward(std::cos(facing), std::sin(facing));
    const glm::vec2 left(-forward.y, forward.x);
    direction += (e.moveFlags & kMoveForward) ? forward : glm::vec2(0.0f);
    direction -= (e.moveFlags & kMoveBackward) ? forward : glm::vec2(0.0f);
    direction += (e.moveFlags & kMoveStrafeLeft) ? left : glm::vec2(0.0f);
    direction -= (e.moveFlags & kMoveStrafeRight) ? left : glm::vec2(0.0f);
    if (glm::dot(direction, direction) < 1e-6f) {
        return origin;
    }
    const f32 distance = e.speedAt(nowMs) * seconds;
    glm::vec3 offset(glm::normalize(direction) * distance, 0.0f);
    if ((e.moveFlags & kMoveSwimming) && (e.moveFlags & (kMoveForward | kMoveBackward))) {
        // Swimming forward or back follows the pitch.
        const f32 sign = (e.moveFlags & kMoveForward) ? 1.0f : -1.0f;
        offset = glm::vec3(glm::vec2(offset) * std::cos(e.pitch), sign * std::sin(e.pitch) * distance);
    }
    return origin + offset;
}

// Phase 18 (motion): `local` is where it is in the frame of `transport` (0: the world): its own place there, and
// its world place when the transport can be placed now.
void placeInFrame(Entity& e, u64 transport, const glm::vec3& local, u32 nowMs) {
    e.transportGuid = transport;
    if (transport == 0) {
        e.position = local;
        e.moveFlags &= ~net::kMoveFlagOnTransport;
        return;
    }
    e.transportOffset = local;
    e.moveFlags |= net::kMoveFlagOnTransport;
    TransportPlacement placement;
    if (resolveTransport(transport, nowMs, placement)) {
        e.position = transportToWorld(placement, local);
        e.orientation = e.transportFacing + placement.orientation;
    }
}

// Phase 18 (motion): what it carries forward from its last packet restarts from where it is now (CMovement 0x7C5CD0,
// before a speed or a mode changes the reckoning). Splines and falls keep their own clock.
void rebase(Entity& e, u32 nowMs) {
    if (!e.hasPosition || e.moving || nowMs == 0 || (e.moveFlags & (kMoveJumping | kMoveFallingFar))) {
        return;
    }
    if (e.transportGuid != 0) {
        e.position = reckon(e, e.position, e.orientation, nowMs); // the world fallback, moved on alike
        e.transportOffset = e.predictedAt(nowMs);
    } else {
        e.position = e.predictedAt(nowMs);
    }
    e.moveTimeMs = nowMs;
}

// Starts easing from what was shown to what `e` now predicts (both in its own frame).
void beginCorrection(Entity& e, bool hadPosition, const glm::vec3& shown, u32 nowMs) {
    e.correction = glm::vec3(0.0f);
    if (!hadPosition) {
        return;
    }
    const glm::vec3 offset = shown - e.predictedAt(nowMs);
    if (glm::dot(offset, offset) < kMaxCorrection * kMaxCorrection) {
        e.correction = offset;
        e.correctionMs = nowMs;
    }
}

void applyMovementInfo(Entity& e, const net::MovementInfo& info, u32 nowMs) {
    // What was shown, in the frame it was shown in (phase 18: a passenger's on its transport): eased only when the
    // new packet is in the same frame.
    const bool hadPosition = e.hasPosition;
    const u64 frameBefore = e.transportGuid;
    const glm::vec3 shown = hadPosition ? e.localAt(nowMs) : glm::vec3(0.0f);
    e.hasPosition = true;
    e.position = {info.position.x, info.position.y, info.position.z};
    e.orientation = info.position.o;
    e.pitch = info.pitch;
    e.moveFlags = info.flags;
    e.moveTimeMs = nowMs;
    e.moving = false;
    e.path.clear();
    // Its own movement says where it faces: what a server spline told it to face no longer holds (a warrior's
    // Charge faces the victim until the player moves again).
    e.hasFinalAngle = false;
    e.facingTarget = 0;
    if (info.flags & net::kMoveFlagOnTransport) {
        e.transportGuid = info.transportGuid;
        e.transportOffset = {info.transportPosition.x, info.transportPosition.y, info.transportPosition.z};
        e.transportFacing = info.transportPosition.o;
    } else {
        e.transportGuid = 0;
    }
    if (info.flags & kMoveJumping) {
        e.airVelocity = glm::vec2(info.jumpCos, info.jumpSin) * info.jumpXySpeed;
        // zspeed is the protocol's (negative goes up), at the start of the fall.
        e.verticalSpeed = std::max(-info.jumpVelocity - kGravity * static_cast<f32>(info.fallTime) / 1000.0f,
                                   -kTerminalVelocity);
    } else {
        e.airVelocity = glm::vec2(0.0f);
        e.verticalSpeed = 0.0f;
    }
    beginCorrection(e, hadPosition && frameBefore == e.transportGuid, shown, nowMs);
}

// Values block: mask word count, mask, then one u32 per set bit (Object::BuildValuesUpdate). `target` may be
// null (unknown object): the block is read and dropped. Phase 18 (motion): `changed` receives the update-mask
// words (EntityStore::wordOf) of the fields whose value really changed.
bool readValues(net::ByteReader& r, Entity* target, u64& changed) {
    const u8 blocks = r.u8v();
    const std::span<const u8> mask = r.view(usize{blocks} * 4);
    if (r.failed()) {
        return false;
    }
    for (u32 word = 0; word < blocks; ++word) {
        u32 bits = 0;
        std::memcpy(&bits, mask.data() + word * 4, 4);
        while (bits) {
            const u32 bit = static_cast<u32>(std::countr_zero(bits));
            bits &= bits - 1;
            const u32 index = word * 32 + bit;
            const u32 value = r.u32v();
            if (target && index < target->values.size() && target->values[index] != value) {
                target->values[index] = value;
                changed |= EntityStore::wordOf(static_cast<u16>(index));
            }
        }
    }
    return r.ok();
}

// Movement block of create/movement updates (Object::BuildMovementUpdate, build > 1.8.4).
bool readMovement(net::ByteReader& r, Entity* target, u32 nowMs) {
    const u8 flags = r.u8v();
    if (flags & kFlagLiving) {
        const net::MovementInfo info = net::readMovementInfo(r);
        f32 speeds[6];
        for (f32& s : speeds) {
            s = r.f32v();
        }
        if (target) {
            applyMovementInfo(*target, info, nowMs);
            std::copy(std::begin(speeds), std::end(speeds), std::begin(target->speeds));
        }
        if (info.flags & net::kMoveFlagSplineEnabled) {
            const u32 splineFlags = r.u32v();
            f32 finalAngle = 0.0f;
            u64 facingTarget = 0;
            if (splineFlags & kSplineFinalAngle) {
                finalAngle = r.f32v();
            } else if (splineFlags & kSplineFinalTarget) {
                facingTarget = r.u64v();
            } else if (splineFlags & kSplineFinalPoint) {
                readVec3(r);
            }
            const u32 elapsed = r.u32v();
            const u32 duration = r.u32v();
            const u32 splineId = r.u32v();
            const u32 nodes = r.u32v();
            if (nodes > 1024) {
                return false;
            }
            if (target) {
                target->path.clear();
                for (u32 i = 0; i < nodes; ++i) {
                    target->path.push_back(readVec3(r));
                }
            } else {
                r.skip(usize{nodes} * 12);
            }
            const glm::vec3 destination = readVec3(r);
            if (target && r.ok()) {
                target->splineFlags = splineFlags;
                target->splineId = splineId;
                target->splineStopped = false;
                target->cyclic = (splineFlags & kSplineCyclic) != 0;
                target->hasFinalAngle = (splineFlags & kSplineFinalAngle) != 0;
                target->finalAngle = finalAngle;
                target->facingTarget = facingTarget;
                target->destination = destination;
                if (target->path.size() < 2) {
                    // Phase 18 (motion): on board, the spline's points are in the transport's frame (the server's
                    // MoveSpline keeps them there), from its offset on it.
                    target->path.assign(
                        {target->transportGuid != 0 ? target->transportOffset : target->position, destination});
                }
                setPathLength(*target);
                target->moving = duration > 0 && (target->cyclic || duration > elapsed);
                target->splineStartMs = nowMs - elapsed;
                target->splineDuration = duration;
                target->correction = glm::vec3(0.0f);
                // The spline moves it, not its directions (MoveSplineInit::Launch sets FORWARD with it and the server
                // clears them unseen on arrival): WoW.exe clears them when the spline ends (0x61A836 -> 0x7C6C00:
                // flags &= ~0x8003), else the unit walks in place at the end of its path (a Northshire peasant at
                // its tree, over its chopping) or slides on from there.
                target->moveFlags &= ~(kMoveForward | kMoveBackward | net::kMoveFlagSplineEnabled);
            }
        }
    } else if (flags & kFlagHasPosition) {
        const glm::vec3 p = readVec3(r);
        const f32 o = r.f32v();
        if (target) {
            target->hasPosition = true;
            target->position = p;
            target->orientation = o;
            target->moving = false;
        }
    }
    if (flags & kFlagHighGuid) {
        r.u32v();
    }
    if (flags & kFlagAll) {
        r.u32v();
    }
    if (flags & kFlagMeleeAttacking) {
        r.packedGuid();
    }
    if (flags & kFlagTransport) {
        const u32 time = r.u32v(); // the transport's time into its cycle (GetPathProgress)
        if (target) {
            target->hasTransportTime = true;
            target->transportTime = time;
            target->transportTimeAt = nowMs;
        }
    }
    return r.ok();
}

} // namespace

void setTransportResolver(TransportResolver resolver, const void* context) {
    g_resolver = resolver;
    g_resolverContext = context;
}

const char* objectTypeName(ObjectType type) {
    switch (type) {
    case ObjectType::Item: return "item";
    case ObjectType::Container: return "container";
    case ObjectType::Unit: return "unit";
    case ObjectType::Player: return "player";
    case ObjectType::GameObject: return "gameobject";
    case ObjectType::DynamicObject: return "dynamicobject";
    case ObjectType::Corpse: return "corpse";
    default: return "object";
    }
}

glm::vec3 Entity::predictedAt(u32 nowMs) const {
    if (moving && splineDuration != 0 && path.size() >= 2) {
        if (!segmentEnds.empty()) {
            return alongSmoothPath(*this, splineProgress(*this, nowMs));
        }
        return alongPath(path, splineProgress(*this, nowMs) * pathLength);
    }
    // Phase 18 (motion): a passenger's from its place and facing on the transport.
    return transportGuid != 0 ? reckon(*this, transportOffset, transportFacing, nowMs)
                              : reckon(*this, position, orientation, nowMs);
}

glm::vec3 Entity::localAt(u32 nowMs) const {
    const glm::vec3 predicted = predictedAt(nowMs);
    if (correction == glm::vec3(0.0f) || nowMs < correctionMs) {
        return predicted;
    }
    const f32 fade = 1.0f - static_cast<f32>(nowMs - correctionMs) / static_cast<f32>(EntityStore::kSmoothMs);
    return fade > 0.0f ? predicted + correction * fade : predicted;
}

glm::vec3 Entity::positionAt(u32 nowMs) const {
    const glm::vec3 local = localAt(nowMs);
    if (transportGuid == 0) {
        return local;
    }
    // Phase 18 (motion): on board, wherever the transport is now; unplaced, where its last packet was in the world.
    TransportPlacement placement;
    if (resolveTransport(transportGuid, nowMs, placement)) {
        return transportToWorld(placement, local);
    }
    return moving ? position : reckon(*this, position, orientation, nowMs);
}

f32 Entity::speedAt(u32 nowMs) const {
    if (moving && splineDuration != 0) {
        if (!splineActive(*this, nowMs)) {
            return 0.0f;
        }
        return pathLength / (static_cast<f32>(splineDuration) / 1000.0f);
    }
    if (moveFlags & kMoveRoot) {
        return 0.0f;
    }
    if (moveFlags & kMoveJumping) {
        return glm::length(airVelocity);
    }
    if ((moveFlags & kMoveDirections) == 0) {
        return 0.0f;
    }
    // Unit::GetSpeedForMovementInfo.
    if (moveFlags & kMoveSwimming) {
        return (moveFlags & kMoveBackward) ? speed(SpeedType::SwimBack) : speed(SpeedType::Swim);
    }
    if (moveFlags & kMoveWalkMode) {
        return speed(SpeedType::Walk);
    }
    return (moveFlags & kMoveBackward) ? speed(SpeedType::RunBack) : speed(SpeedType::Run);
}

bool Entity::headingAt(u32 nowMs, f32& heading) const {
    if (!splineHeading(*this, nowMs, heading)) {
        return false;
    }
    heading = worldAngle(heading, nowMs);
    return true;
}

f32 Entity::worldAngle(f32 local, u32 nowMs) const {
    if (transportGuid == 0) {
        return local;
    }
    TransportPlacement placement;
    if (resolveTransport(transportGuid, nowMs, placement)) {
        return local + placement.orientation;
    }
    return local + (orientation - transportFacing); // the transport's facing when the last packet came
}

f32 Entity::facingAt(u32 nowMs) const {
    return transportGuid != 0 ? worldAngle(transportFacing, nowMs) : orientation;
}

bool Entity::onSpline(u32 nowMs) const {
    return splineActive(*this, nowMs);
}

bool Entity::walking() const {
    return moving ? (splineFlags & kSplineRunMode) == 0 : (moveFlags & kMoveWalkMode) != 0;
}

bool Entity::airborne() const {
    return !moving && (moveFlags & (kMoveJumping | kMoveFallingFar)) != 0;
}

Entity* EntityStore::get(u64 guid) {
    const auto it = m_index.find(guid);
    return it == m_index.end() ? nullptr : &m_entities[it->second];
}

const Entity* EntityStore::find(u64 guid) const {
    const auto it = m_index.find(guid);
    return it == m_index.end() ? nullptr : &m_entities[it->second];
}

Entity& EntityStore::create(u64 guid, ObjectType type) {
    u32 slot = 0;
    if (const auto it = m_index.find(guid); it != m_index.end()) {
        slot = it->second; // created again (e.g. back in range): start from scratch
    } else if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
        m_index.emplace(guid, slot);
    } else {
        slot = static_cast<u32>(m_entities.size());
        m_entities.emplace_back();
        m_index.emplace(guid, slot);
    }
    Entity& e = m_entities[slot];
    std::vector<glm::vec3> path = std::move(e.path); // keep its capacity
    e = Entity{};
    e.path = std::move(path);
    e.path.clear();
    e.guid = guid;
    e.type = type;
    e.values.assign(valueCount(type), 0);
    e.createdFrame = m_frame; // phase 18 (motion)
    return e;
}

void EntityStore::remove(u64 guid) {
    const auto it = m_index.find(guid);
    if (it == m_index.end()) {
        return;
    }
    m_entities[it->second] = Entity{};
    m_free.push_back(it->second);
    m_index.erase(it);
    m_removed.push_back(guid); // phase 18 (motion)
}

void EntityStore::clear() {
    for (const auto& [guid, slot] : m_index) { // phase 18 (motion)
        m_removed.push_back(guid);
    }
    m_entities.clear();
    m_free.clear();
    m_index.clear();
}

void EntityStore::nextFrame() {
    ++m_frame;
    m_changed.clear();
    m_removed.clear();
    if (m_changed.capacity() == 0) {
        m_changed.reserve(512);
        m_removed.reserve(256);
    }
}

void EntityStore::markChanged(Entity& e, u64 words) {
    e.valuesVersion = ++m_version; // unique across the store: a cached version never matches a newer state
    if (e.changedFrame != m_frame) {
        e.changedFrame = m_frame;
        e.changedWords = 0;
        m_changed.push_back(e.guid);
    }
    e.changedWords |= words;
}

u32 EntityStore::countOf(ObjectType type) const {
    u32 count = 0;
    forEach([&](const Entity& e) { count += e.type == type ? 1 : 0; });
    return count;
}

bool EntityStore::applyUpdateObject(std::span<const u8> payload, u32 nowMs, UpdateStats* stats) {
    UpdateStats local;
    UpdateStats& s = stats ? *stats : local;
    net::ByteReader r(payload);
    const u32 blocks = r.u32v();
    r.u8v(); // has transport
    for (u32 b = 0; b < blocks && r.ok(); ++b) {
        const u8 type = r.u8v();
        switch (type) {
        case kUpdateValues: {
            const u64 guid = r.packedGuid();
            Entity* e = get(guid);
            u64 words = 0;
            if (!readValues(r, e, words)) {
                return false;
            }
            if (e && words != 0) {
                markChanged(*e, words); // phase 18 (motion)
            }
            ++s.updated;
            break;
        }
        case kUpdateMovement: {
            const u64 guid = r.packedGuid();
            if (!readMovement(r, get(guid), nowMs)) {
                return false;
            }
            ++s.moved;
            break;
        }
        case kUpdateCreate:
        case kUpdateCreate2: {
            const u64 guid = r.packedGuid();
            const u8 objectType = r.u8v();
            if (objectType > static_cast<u8>(ObjectType::Corpse)) {
                return false;
            }
            Entity& e = create(guid, static_cast<ObjectType>(objectType));
            u64 words = 0;
            if (!readMovement(r, &e, nowMs) || !readValues(r, &e, words)) {
                return false;
            }
            markChanged(e, ~u64{0}); // phase 18 (motion): everything is new
            ++s.created;
            if (s.createdGuids) {
                s.createdGuids->push_back(guid);
            }
            break;
        }
        case kUpdateOutOfRange: {
            const u32 count = r.u32v();
            for (u32 i = 0; i < count && r.ok(); ++i) {
                remove(r.packedGuid());
                ++s.removed;
            }
            break;
        }
        case kUpdateNear: {
            const u32 count = r.u32v();
            for (u32 i = 0; i < count && r.ok(); ++i) {
                r.packedGuid();
            }
            break;
        }
        default: return false; // unknown block: the rest can't be located
        }
    }
    return r.ok();
}

bool EntityStore::applyDestroy(std::span<const u8> payload) {
    net::ByteReader r(payload);
    const u64 guid = r.u64v();
    if (r.failed()) {
        return false;
    }
    remove(guid);
    return true;
}

bool EntityStore::applyMove(std::span<const u8> payload, u32 nowMs, u64* moverGuid) {
    net::ByteReader r(payload);
    const u64 guid = r.packedGuid();
    const net::MovementInfo info = net::readMovementInfo(r);
    if (r.failed()) {
        return false;
    }
    if (moverGuid) {
        *moverGuid = guid;
    }
    if (Entity* e = get(guid)) {
        applyMovementInfo(*e, info, nowMs);
    }
    return true;
}

bool EntityStore::applyMoveSpeed(std::span<const u8> payload, SpeedType type, u32 nowMs) {
    net::ByteReader r(payload);
    const u64 guid = r.packedGuid();
    const net::MovementInfo info = net::readMovementInfo(r);
    const f32 speed = r.f32v();
    if (r.failed()) {
        return false;
    }
    if (Entity* e = get(guid)) {
        applyMovementInfo(*e, info, nowMs);
        e->speeds[static_cast<u32>(type)] = speed;
    }
    return true;
}

bool EntityStore::applySplineSpeed(std::span<const u8> payload, SpeedType type, u32 nowMs) {
    net::SplineSpeed m;
    if (!net::parseSplineSpeed(payload, m)) {
        return false;
    }
    if (Entity* e = get(m.guid)) {
        rebase(*e, nowMs); // phase 18 (motion): the speed applies from now on
        e->speeds[static_cast<u32>(type)] = m.speed;
    }
    return true;
}

bool EntityStore::applyTimeSkipped(std::span<const u8> payload) {
    net::ByteReader r(payload);
    const u64 guid = r.packedGuid();
    const u32 lagMs = r.u32v();
    if (r.failed()) {
        return false;
    }
    if (Entity* e = get(guid); e && !e->moving) {
        e->moveTimeMs += lagMs; // its last packet is that much later: the dead reckoning does not run on
    }
    return true;
}

void EntityStore::setSpeed(u64 guid, SpeedType type, f32 speed) {
    if (Entity* e = get(guid)) {
        e->speeds[static_cast<u32>(type)] = speed;
    }
}

void EntityStore::setMovement(u64 guid, const net::MovementInfo& info, u32 nowMs) {
    if (Entity* e = get(guid)) {
        applyMovementInfo(*e, info, nowMs);
        e->correction = glm::vec3(0.0f); // the local simulation is exact
    }
}

bool EntityStore::applyMonsterMove(std::span<const u8> payload, u32 nowMs) {
    net::ByteReader r(payload);
    const u64 guid = r.packedGuid();
    if (r.failed()) {
        return false;
    }
    // WoW.exe parses both monster moves with one function (0x6018F0) after putting the unit on the transport the
    // packet names, none here (CMovement 0x617100): a plain one takes a passenger off.
    return monsterMove(get(guid), 0, r.rest(), nowMs);
}

bool EntityStore::applyMonsterMoveTransport(std::span<const u8> payload, u32 nowMs) {
    net::MonsterMoveTransportHeader header;
    if (!net::parseMonsterMoveTransportHeader(payload, header)) {
        return false;
    }
    return monsterMove(get(header.unit), header.transport, header.body, nowMs);
}

bool EntityStore::monsterMove(Entity* e, u64 transport, std::span<const u8> body, u32 nowMs) {
    net::ByteReader r(body);
    const glm::vec3 start = readVec3(r);
    const u32 splineId = r.u32v();
    const u8 moveType = r.u8v();
    // What was shown eases into the new path only in the same frame (phase 18: boarding or leaving jumps).
    const bool hadPosition = e && e->hasPosition && e->transportGuid == transport;
    const glm::vec3 shown = hadPosition ? e->localAt(nowMs) : glm::vec3(0.0f);
    if (moveType == kMonsterMoveStop) {
        if (r.failed()) {
            return false;
        }
        if (e) {
            e->hasPosition = true;
            placeInFrame(*e, transport, start, nowMs);
            e->splineId = splineId;
            e->splineStopped = true;
            e->moving = false;
            e->moveFlags &= ~(kMoveDirections | kMoveJumping | kMoveFallingFar);
            beginCorrection(*e, hadPosition, shown, nowMs);
        }
        return true;
    }
    glm::vec3 spot(0.0f);
    u64 target = 0;
    f32 angle = 0.0f;
    if (moveType == kMonsterMoveFacingSpot) {
        spot = readVec3(r);
    } else if (moveType == kMonsterMoveFacingTarget) {
        target = r.u64v();
    } else if (moveType == kMonsterMoveFacingAngle) {
        angle = r.f32v();
    }
    const u32 splineFlags = r.u32v();
    const u32 duration = r.u32v();
    const u32 count = r.u32v();
    if (r.failed() || count > 1024) {
        return false;
    }
    if (!e) {
        return true;
    }
    e->path.clear();
    e->path.push_back(start);
    if (splineFlags & kSplineFlying) {
        // Every point in full (a smooth path, drawn here as straight segments). A cyclic one repeats the start first.
        for (u32 i = 0; i < count && r.ok(); ++i) {
            const glm::vec3 point = readVec3(r);
            if (i == 0 && (splineFlags & kSplineEnterCycle)) {
                continue;
            }
            e->path.push_back(point);
        }
    } else if (count > 0) {
        // The destination, then the points before it as packed offsets from it (vMaNGOS WriteLinearPath).
        const glm::vec3 destination = readVec3(r);
        const u32 offsets = std::min<u32>(count - 1, static_cast<u32>(r.remaining() / 4));
        for (u32 i = 0; i < offsets; ++i) {
            const glm::vec3 point = destination - unpackXyz(r.u32v());
            if (glm::length(point - e->path.back()) > 0.3f) {
                e->path.push_back(point);
            }
        }
        e->path.push_back(destination);
    }
    if (r.failed()) {
        return false;
    }
    e->hasPosition = true;
    placeInFrame(*e, transport, start, nowMs); // phase 18 (motion): the path is in that frame
    e->splineId = splineId;
    e->splineStopped = false;
    e->destination = e->path.back();
    e->splineStartMs = nowMs;
    e->splineDuration = duration;
    e->splineFlags = splineFlags;
    e->cyclic = (splineFlags & kSplineCyclic) != 0;
    e->moving = duration > 0 && e->path.size() >= 2;
    e->moveFlags &= ~(kMoveForward | kMoveBackward); // the spline moves it (0x7C6C00 at its end, as above)
    e->hasFinalAngle = moveType == kMonsterMoveFacingAngle;
    e->finalAngle = angle;
    e->facingTarget = target;
    if (moveType == kMonsterMoveFacingSpot) {
        e->hasFinalAngle = true;
        e->finalAngle = std::atan2(spot.y - e->destination.y, spot.x - e->destination.x);
    }
    if (e->hasFinalAngle && !e->moving) {
        (transport != 0 ? e->transportFacing : e->orientation) = e->finalAngle;
    }
    setPathLength(*e);
    beginCorrection(*e, hadPosition, shown, nowMs);
    return true;
}

bool EntityStore::applySplineMove(std::span<const u8> payload, net::SplineMoveKind kind, u32 nowMs) {
    u64 guid = 0;
    if (!net::parseSplineMove(payload, guid)) {
        return false;
    }
    Entity* e = get(guid);
    if (!e) {
        return true;
    }
    using Kind = net::SplineMoveKind;
    switch (kind) {
    case Kind::Root:
        // CGUnit 0x619A10: a running spline ends where the unit is (0x616D30), the reckoning restarts from there,
        // and the directions go (0x7C7340).
        if (e->moving && e->hasPosition) {
            const glm::vec3 here = e->predictedAt(nowMs);
            f32 heading = 0.0f;
            if (splineHeading(*e, nowMs, heading)) {
                (e->transportGuid != 0 ? e->transportFacing : e->orientation) = heading;
            }
            e->moving = false;
            placeInFrame(*e, e->transportGuid, here, nowMs);
            e->moveTimeMs = nowMs;
        } else {
            rebase(*e, nowMs);
        }
        e->moveFlags = (e->moveFlags | kMoveRoot) & ~kMoveRootClears;
        break;
    case Kind::Unroot:
        e->moveTimeMs = nowMs; // 0x619A40: what it was doing carries on from here
        e->moveFlags &= ~kMoveRoot;
        break;
    case Kind::WaterWalk: e->moveFlags |= kMoveWaterWalking; break;
    case Kind::LandWalk: e->moveFlags &= ~kMoveWaterWalking; break;
    case Kind::FeatherFall: e->moveFlags |= kMoveSafeFall; break;
    case Kind::NormalFall: e->moveFlags &= ~kMoveSafeFall; break;
    case Kind::SetHover: e->moveFlags |= kMoveHover; break;
    case Kind::UnsetHover: e->moveFlags &= ~kMoveHover; break;
    case Kind::StartSwim:
        rebase(*e, nowMs);
        e->moveFlags |= kMoveSwimming;
        break;
    case Kind::StopSwim: // 0x7C6E80: the pitch goes with it
        rebase(*e, nowMs);
        e->moveFlags &= ~(kMoveSwimming | kMovePitches);
        e->pitch = 0.0f;
        break;
    case Kind::RunMode:
        rebase(*e, nowMs);
        e->moveFlags &= ~kMoveWalkMode;
        break;
    case Kind::WalkMode:
        rebase(*e, nowMs);
        e->moveFlags |= kMoveWalkMode;
        break;
    }
    return true;
}

} // namespace vd::game
