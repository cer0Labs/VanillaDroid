#include "app/MotionDemo.h"

#include "app/GameSession.h"
#include "app/Transports.h"
#include "app/Units.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Transports.h"
#include "net/ByteBuffer.h"
#include "net/SplineMessages.h"
#include "net/UpdateFields.gen.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace vd {
namespace {

using net::Opcode;
namespace uf = net::uf;

constexpr f32 kPi = 3.14159265f;
// Speeds (baseMoveSpeed of the server).
constexpr f32 kWalkSpeed = 2.5f;
constexpr f32 kRunSpeed = 7.0f;
constexpr f32 kRunBackSpeed = 4.5f;
constexpr f32 kSwimSpeed = 4.722222f;
// MoveSplineFlag and MOVEFLAG_* (vMaNGOS).
constexpr u32 kSplineRunMode = 0x100;
constexpr u32 kSplineFlying = 0x200;
constexpr u32 kMoveForward = 0x1;
constexpr u32 kMoveBackward = 0x2;
constexpr u32 kMoveWalkMode = 0x100;
constexpr u32 kMoveOnTransport = 0x02000000;
constexpr u32 kUnitFlagUseSwimAnimation = 0x00008000;
constexpr u32 kVisFlagCreep = 0x02; // UNIT_VIS_FLAGS_CREEP
// Displays (CreatureDisplayInfo): a wolf, a murloc, a boar, a gryphon, a human NPC dressed by its extra info,
// and the human and orc male characters.
constexpr u32 kWolf = 165, kMurloc = 31, kBoar = 193, kGryphon = 1148, kHumanNpc = 1276, kHumanMale = 49,
              kOrcMale = 51;
constexpr u32 kSword = 679;                              // ItemDisplayInfo: Sword_1H_Short_A_01
constexpr u32 kSwordInfo = 2u | (7u << 8) | (13u << 24); // class weapon, subclass sword, INVTYPE_WEAPON

u32 bitsOf(f32 value) {
    u32 out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

// Values block (Object::BuildValuesUpdate): the mask over `count` fields, then the values in field order.
void writeValues(net::ByteWriter& w, std::initializer_list<std::pair<u16, u32>> values, u32 count) {
    const u32 words = (count + 31) / 32;
    u32 mask[64] = {};
    for (const auto& [index, value] : values) {
        (void)value;
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (u32 i = 0; i < words; ++i) {
        w.u32v(mask[i]);
    }
    for (u32 index = 0; index < count; ++index) {
        for (const auto& [i, value] : values) {
            if (i == index) {
                w.u32v(value);
            }
        }
    }
}

f32 roleSpeed(u8 role) {
    switch (role) {
    case 0: return kWalkSpeed; // Walk
    case 2: return 3.5f;       // SlowRun: Hamstring's half
    case 3: return 14.0f;      // Sprint: FollowMovementGenerator's catch-up, up to 2.1 times
    default: return kRunSpeed;
    }
}

} // namespace

void MotionDemo::init() {
    const char* spec = VD_DEV_ENV("VD_MOTION_DEMO");
    if (!spec || !*spec) {
        return;
    }
    const std::string_view s(spec);
    if (s.rfind("modes", 0) == 0) {
        m_scenario = Scenario::Modes;
    } else if (s.rfind("crowd", 0) == 0) {
        m_scenario = Scenario::Crowd;
        if (const usize colon = s.find(':'); colon != std::string_view::npos) {
            m_count = std::clamp<u32>(static_cast<u32>(std::atoi(spec + colon + 1)), 1u, 400u);
        }
    } else if (s.rfind("ship", 0) == 0) {
        m_scenario = Scenario::Ship;
    } else {
        VD_LOG_WARN("VD_MOTION_DEMO=%s: modes, crowd[:N] or ship", spec);
        return;
    }
    m_members.reserve(m_scenario == Scenario::Crowd ? m_count : 16);
    m_body.reserve(1024);
    VD_LOG_INFO("Motion demo: %s", spec);
}

void MotionDemo::send(GameSession& session, u16 opcode, u32 nowMs) {
    session.injectPacket(static_cast<Opcode>(opcode), m_body, nowMs);
}

void MotionDemo::create(GameSession& session, const Member& m, u32 display, u32 race, u32 nowMs, u32 flags,
                        u32 visFlags, f32 scale) {
    m_body.clear();
    net::ByteWriter w(m_body);
    // One block, no transport: the creation of a player or a unit (UPDATEFLAG_LIVING and UPDATEFLAG_ALL).
    w.u32v(1).u8v(0);
    w.u8v(2).packedGuid(m.guid).u8v(m.player ? 4 : 3);
    w.u8v(0x20 | 0x10);
    net::MovementInfo info;
    info.time = nowMs;
    const glm::vec3& at = m.towardsB ? m.a : m.b;
    if (m.role == Role::Aboard || m.role == Role::Deck) {
        info.flags = kMoveOnTransport;
        info.transportGuid = Transports::kPreviewGuid;
        info.transportPosition = {at.x, at.y, at.z, 0.0f};
        info.position = {at.x, at.y, at.z, 0.0f}; // the world one comes from the transport
    } else {
        info.position = {at.x, at.y, at.z, std::atan2(m.b.y - m.a.y, m.b.x - m.a.x)};
    }
    net::writeMovementInfo(w, info);
    const f32 run = m.role == Role::Stealth ? kRunSpeed * 0.7f : kRunSpeed; // Stealth's slowed run (rank 1)
    for (f32 speed : {kWalkSpeed, run, kRunBackSpeed, kSwimSpeed, 2.5f, 3.141594f}) {
        w.f32v(speed);
    }
    w.u32v(1);                                                     // UPDATEFLAG_ALL's value
    const u32 bytes0 = race | (1u << 8) | (0u << 16) | (1u << 24); // race, warrior, male, rage
    if (m.player) {
        writeValues(
            w,
            {{uf::OBJECT_FIELD_GUID, static_cast<u32>(m.guid)},
             {static_cast<u16>(uf::OBJECT_FIELD_GUID + 1), static_cast<u32>(m.guid >> 32)},
             {uf::OBJECT_FIELD_TYPE, 0x19},
             {uf::OBJECT_FIELD_SCALE_X, bitsOf(scale)},
             {uf::UNIT_FIELD_HEALTH, m.health},
             {uf::UNIT_FIELD_MAXHEALTH, 100},
             {uf::UNIT_FIELD_LEVEL, 60},
             {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
             {uf::UNIT_FIELD_BYTES_0, bytes0},
             {uf::UNIT_FIELD_FLAGS, flags | 0x8}, // UNIT_FLAG_PLAYER_CONTROLLED
             {uf::UNIT_FIELD_BOUNDINGRADIUS, bitsOf(0.389f)},
             {uf::UNIT_FIELD_COMBATREACH, bitsOf(1.5f)},
             {uf::UNIT_FIELD_DISPLAYID, display},
             {uf::UNIT_FIELD_NATIVEDISPLAYID, display},
             {uf::UNIT_FIELD_BYTES_1, visFlags << 24},
             {uf::UNIT_FIELD_AURA, 1126}, // a few buffs, as everyone has (their look is read from them)
             {static_cast<u16>(uf::UNIT_FIELD_AURA + 1), 21562},
             {static_cast<u16>(uf::UNIT_FIELD_AURA + 2), 467},
             {uf::PLAYER_BYTES, 0x00010001u}, // skin 1, face 0, hair style 1, color 0
             {uf::PLAYER_BYTES_2, 0x01}},
            uf::PLAYER_END);
    } else {
        writeValues(w,
                    {{uf::OBJECT_FIELD_GUID, static_cast<u32>(m.guid)},
                     {static_cast<u16>(uf::OBJECT_FIELD_GUID + 1), static_cast<u32>(m.guid >> 32)},
                     {uf::OBJECT_FIELD_TYPE, 0x9},
                     {uf::OBJECT_FIELD_ENTRY, 1},
                     {uf::OBJECT_FIELD_SCALE_X, bitsOf(scale)},
                     {uf::UNIT_FIELD_HEALTH, m.health},
                     {uf::UNIT_FIELD_MAXHEALTH, 100},
                     {uf::UNIT_FIELD_LEVEL, 10},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 7},
                     {uf::UNIT_FIELD_BYTES_0, bytes0},
                     {uf::UNIT_FIELD_FLAGS, flags},
                     {uf::UNIT_FIELD_BOUNDINGRADIUS, bitsOf(0.5f)},
                     {uf::UNIT_FIELD_COMBATREACH, bitsOf(1.5f)},
                     {uf::UNIT_FIELD_DISPLAYID, display},
                     {uf::UNIT_FIELD_NATIVEDISPLAYID, display},
                     {uf::UNIT_FIELD_AURA, 1126},
                     {static_cast<u16>(uf::UNIT_FIELD_AURA + 1), 467},
                     {uf::UNIT_FIELD_BYTES_1, visFlags << 24}},
                    uf::UNIT_END);
    }
    send(session, static_cast<u16>(Opcode::SMSG_UPDATE_OBJECT), nowMs);
}

bool MotionDemo::spawn(GameSession& session, const Transports& transports, const glm::vec3& center, u32 nowMs,
                       HeightQuery height, const void* context) {
    auto ground = [&](f32 x, f32 y, glm::vec3& out) {
        const std::optional<f32> z = height(context, x, y, center.z + 30.0f, center.z - 30.0f);
        out = glm::vec3(x, y, z ? *z : 0.0f);
        return z.has_value();
    };
    auto random = [&]() {
        m_seed ^= m_seed << 13;
        m_seed ^= m_seed >> 17;
        m_seed ^= m_seed << 5;
        return static_cast<f32>(m_seed & 0xFFFFFF) / static_cast<f32>(0x1000000);
    };
    m_members.clear();
    if (m_scenario == Scenario::Modes) {
        // West to east, 3 yd apart, each going 12 yd north and back, 6 yd ahead of the character.
        constexpr Role kRoles[] = {Role::Walk,    Role::Run,       Role::SlowRun,  Role::Sprint,
                                   Role::Stealth, Role::Backwards, Role::WalkMode, Role::Swim,
                                   Role::Rooted,  Role::Fly};
        for (u32 i = 0; i < std::size(kRoles); ++i) {
            Member m;
            m.role = kRoles[i];
            m.player = m.role == Role::Stealth || m.role == Role::Backwards || m.role == Role::WalkMode;
            m.guid = m.player ? 0x0000000000F00000ull + i : 0xF130000001000000ull + i;
            const f32 x = center.x + (static_cast<f32>(i) - 4.5f) * 3.0f;
            if (!ground(x, center.y + 6.0f, m.a) || !ground(x, center.y + 18.0f, m.b)) {
                return false;
            }
            m_members.push_back(m);
        }
    } else if (m_scenario == Scenario::Crowd) {
        for (u32 i = 0; i < m_count; ++i) {
            Member m;
            m.role = Role::Wander;
            m.player = i % 4 == 3;
            m.guid = m.player ? 0x0000000000F10000ull + i : 0xF130000002000000ull + i;
            const f32 angle = random() * 2.0f * kPi, radius = 5.0f + random() * 25.0f;
            if (!ground(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, m.a)) {
                return false;
            }
            m.b = m.a;
            m.nextMs = nowMs + static_cast<u32>(random() * 2000.0f);
            m_members.push_back(m);
        }
    } else {
        // The deck: where the character stands when it is on board (its place in the ship's frame), else
        // under the transport's middle (its collision).
        game::TransportPlacement ship;
        if (!transports.placementAt(Transports::kPreviewGuid, nowMs, ship)) {
            return false;
        }
        glm::vec3 middle = game::worldToTransport(ship, center);
        if (std::fabs(middle.x) > 25.0f || std::fabs(middle.y) > 8.0f || middle.z < 1.0f ||
            middle.z > 20.0f) {
            // From under the yards (a ship's deck stands 6 to 10 yd over its waterline).
            const std::optional<f32> deck = height(context, ship.position.x, ship.position.y,
                                                   ship.position.z + 11.0f, ship.position.z - 2.0f);
            if (!deck) {
                return false;
            }
            middle = glm::vec3(0.0f, 0.0f, *deck - ship.position.z);
        }
        for (u32 i = 0; i < 4; ++i) {
            Member m;
            m.role = i == 3 ? Role::Aboard : Role::Deck;
            m.player = m.role == Role::Aboard;
            m.guid = m.player ? 0x0000000000F20000ull : 0xF130000003000000ull + i;
            const f32 side = -1.5f + static_cast<f32>(i);
            m.a = middle + glm::vec3(-6.0f + static_cast<f32>(i), side, 0.0f);
            m.b = middle + glm::vec3(6.0f - static_cast<f32>(i), side, 0.0f);
            m_members.push_back(m);
        }
        VD_LOG_INFO("Motion demo: the deck at (%.1f, %.1f, %.1f) of the ship's frame", middle.x, middle.y,
                    middle.z);
    }
    // The units, as their create blocks.
    u32 index = 0;
    for (Member& m : m_members) {
        u32 display = kWolf, race = 0, flags = 0, vis = 0;
        f32 scale = 1.0f;
        switch (m.role) {
        case Role::Walk: display = kHumanNpc; break;
        case Role::Run: display = kWolf; break;
        case Role::SlowRun: display = kBoar; break;
        case Role::Sprint: display = kWolf; break;
        case Role::Stealth: display = kHumanMale, race = 1, vis = kVisFlagCreep; break;
        case Role::Backwards: display = kOrcMale, race = 2; break;
        case Role::WalkMode: display = kHumanMale, race = 1; break;
        case Role::Swim: display = kMurloc, flags = kUnitFlagUseSwimAnimation; break;
        case Role::Rooted: display = kWolf; break;
        case Role::Fly: display = kGryphon; break;
        case Role::Wander: {
            constexpr u32 kDisplays[] = {kWolf, kMurloc, kBoar, kHumanNpc};
            display = m.player ? (index % 2 ? kOrcMale : kHumanMale) : kDisplays[index % 4];
            race = m.player ? (index % 2 ? 2 : 1) : 0;
            break;
        }
        case Role::Deck: display = index % 2 ? kHumanNpc : kWolf; break;
        case Role::Aboard: display = kHumanMale, race = 1; break;
        }
        create(session, m, display, race, nowMs, flags, vis, scale);
        ++index;
        if (m.role == Role::Walk) { // a creature walking: the server's walk mode first (Unit::SetWalk)
            m_body.clear();
            net::writeSplineMove(m_body, m.guid);
            send(session, static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE), nowMs);
        } else if (m.role == Role::Swim) {
            m_body.clear();
            net::writeSplineMove(m_body, m.guid);
            send(session, static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_START_SWIM), nowMs);
        } else if (m.role == Role::WalkMode) {
            m_body.clear();
            net::ByteWriter w(m_body);
            w.packedGuid(m.guid);
            net::MovementInfo info;
            info.flags = kMoveWalkMode;
            info.time = nowMs;
            info.position = {m.a.x, m.a.y, m.a.z, 0.0f};
            net::writeMovementInfo(w, info);
            send(session, static_cast<u16>(Opcode::MSG_MOVE_SET_WALK_MODE), nowMs);
        }
        if (m.nextMs == 0) {
            m.nextMs = nowMs + 500;
        }
    }
    VD_LOG_INFO("Motion demo: %zu units around (%.1f, %.1f, %.1f)", m_members.size(), center.x, center.y,
                center.z);
    return true;
}

void MotionDemo::playerMove(GameSession& session, const Member& m, u16 opcode, const glm::vec3& at, u32 flags,
                            u32 nowMs) {
    m_body.clear();
    net::ByteWriter w(m_body);
    w.packedGuid(m.guid);
    net::MovementInfo info;
    info.flags = flags;
    info.time = nowMs;
    info.position = {at.x, at.y, at.z, m.facing};
    net::writeMovementInfo(w, info);
    send(session, opcode, nowMs);
}

void MotionDemo::leg(GameSession& session, Member& m, u32 nowMs) {
    const glm::vec3 from = m.towardsB ? m.a : m.b;
    const glm::vec3 to = m.towardsB ? m.b : m.a;
    m.towardsB = !m.towardsB;
    const f32 length = glm::length(to - from);
    m_body.clear();
    if (m.player) {
        // Another client's character: MSG_MOVE_START_* and heartbeats (its dead reckoning), then a stop.
        const bool back = m.role == Role::Backwards;
        const f32 speed = m.role == Role::WalkMode  ? kWalkSpeed
                          : back                    ? kRunBackSpeed
                          : m.role == Role::Stealth ? kRunSpeed * 0.7f
                                                    : kRunSpeed;
        const glm::vec3 dir = to - from;
        m.moving = true;
        m.from = from;
        m.to = to;
        m.legStartMs = m.beatMs = nowMs;
        m.legMs = static_cast<u32>(length / speed * 1000.0f);
        m.moveFlags = (back ? kMoveBackward : kMoveForward) | (m.role == Role::WalkMode ? kMoveWalkMode : 0u);
        m.facing = back ? std::atan2(-dir.y, -dir.x) : std::atan2(dir.y, dir.x);
        playerMove(session, m,
                   static_cast<u16>(back ? Opcode::MSG_MOVE_START_BACKWARD : Opcode::MSG_MOVE_START_FORWARD),
                   from, m.moveFlags, nowMs);
        return;
    }
    net::MonsterMove move;
    move.guid = m.guid;
    move.start = from;
    move.splineId = ++m.splineId;
    f32 speed = roleSpeed(static_cast<u8>(m.role));
    move.flags = kSplineRunMode;
    if (m.role == Role::Walk) {
        move.flags = 0;
    } else if (m.role == Role::Swim) {
        speed = kSwimSpeed;
    } else if (m.role == Role::Wander) {
        const bool walk = (m.guid & 1) != 0;
        move.flags = walk ? 0 : kSplineRunMode;
        speed = walk ? kWalkSpeed : kRunSpeed;
    } else if (m.role == Role::Deck) {
        move.transport = Transports::kPreviewGuid;
        move.flags = 0;
        speed = kWalkSpeed;
    }
    if (m.role == Role::Fly) {
        move.flags = kSplineFlying | kSplineRunMode;
        move.points = {(from + to) * 0.5f + glm::vec3(0.0f, 0.0f, 6.0f), to};
    } else {
        move.points = {to};
    }
    move.duration = static_cast<u32>(std::max(length, 0.1f) / speed * 1000.0f);
    net::writeMonsterMove(m_body, move);
    send(session, static_cast<u16>(net::monsterMoveOpcode(move)), nowMs);
    m.from = from;
    m.to = to;
    m.nextMs = nowMs + move.duration + 1000;
    if (m.role == Role::Rooted) {
        m.rootMs = nowMs + move.duration * 2 / 5; // rooted 40% of the way
    }
}

void MotionDemo::update(GameSession& session, const Units& units, const Transports& transports,
                        const glm::vec3& center, u32 nowMs, HeightQuery height, const void* context) {
    if (m_scenario == Scenario::None || !height) {
        return;
    }
    VD_PROFILE_ZONE("MotionDemo::update");
    if (!m_spawned) {
        m_spawned = spawn(session, transports, center, nowMs, height, context);
        m_logAtMs = nowMs + 5000;
        m_healthAtMs = nowMs + 2000;
        m_gearAtMs = nowMs + 10000;
        return;
    }
    auto random = [&]() {
        m_seed ^= m_seed << 13;
        m_seed ^= m_seed >> 17;
        m_seed ^= m_seed << 5;
        return static_cast<f32>(m_seed & 0xFFFFFF) / static_cast<f32>(0x1000000);
    };
    for (Member& m : m_members) {
        if (m.role == Role::Aboard) {
            continue; // stands on the deck, carried
        }
        if (m.role == Role::Rooted && m.rootMs != 0 && static_cast<i32>(nowMs - m.rootMs) >= 0) {
            m_body.clear();
            if (!m.rooted) {
                // Unit::SetRooted: StopMoving (a stop where it is), then the root.
                m.from += (m.to - m.from) * 0.4f;
                net::MonsterMove stop;
                stop.guid = m.guid;
                stop.start = m.from;
                stop.splineId = ++m.splineId;
                stop.facing = net::MonsterMove::Facing::Stop;
                net::writeMonsterMove(m_body, stop);
                send(session, static_cast<u16>(Opcode::SMSG_MONSTER_MOVE), nowMs);
                m_body.clear();
                net::writeSplineMove(m_body, m.guid);
                send(session, static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_ROOT), nowMs);
                m.rooted = true;
                m.rootMs = nowMs + 2000;
            } else {
                // Freed: the unroot, then on to where it was going (MoveSplineInit::Launch sends both).
                net::writeSplineMove(m_body, m.guid);
                send(session, static_cast<u16>(Opcode::SMSG_SPLINE_MOVE_UNROOT), nowMs);
                net::MonsterMove move;
                move.guid = m.guid;
                move.start = m.from;
                move.splineId = ++m.splineId;
                move.flags = kSplineRunMode;
                move.points = {m.to};
                move.duration =
                    static_cast<u32>(std::max(glm::length(m.to - m.from), 0.1f) / kRunSpeed * 1000.0f);
                m_body.clear();
                net::writeMonsterMove(m_body, move);
                send(session, static_cast<u16>(Opcode::SMSG_MONSTER_MOVE), nowMs);
                m.rooted = false;
                m.rootMs = 0;
                m.nextMs = nowMs + move.duration + 1000;
            }
            continue;
        }
        if (m.player && m.moving) {
            const u32 elapsed = nowMs - m.legStartMs;
            if (elapsed >= m.legMs) { // arrived: MSG_MOVE_STOP there, a second's rest, then back
                m.moving = false;
                playerMove(session, m, static_cast<u16>(Opcode::MSG_MOVE_STOP), m.to,
                           m.moveFlags & kMoveWalkMode, nowMs);
                m.nextMs = nowMs + 1000;
            } else if (nowMs - m.beatMs >= 500) { // MSG_MOVE_HEARTBEAT every 500 ms while it moves
                m.beatMs = nowMs;
                const f32 t = static_cast<f32>(elapsed) / static_cast<f32>(std::max<u32>(m.legMs, 1));
                playerMove(session, m, static_cast<u16>(Opcode::MSG_MOVE_HEARTBEAT),
                           m.from + (m.to - m.from) * t, m.moveFlags, nowMs);
            }
            continue;
        }
        if (static_cast<i32>(nowMs - m.nextMs) < 0 || m.rooted) {
            continue;
        }
        if (m.role == Role::Wander) {
            // A new random point, 5 to 30 yd from the center.
            const f32 angle = random() * 2.0f * kPi, radius = 5.0f + random() * 25.0f;
            const f32 x = center.x + std::cos(angle) * radius, y = center.y + std::sin(angle) * radius;
            if (const std::optional<f32> z = height(context, x, y, center.z + 30.0f, center.z - 30.0f)) {
                (m.towardsB ? m.b : m.a) = glm::vec3(x, y, *z);
            }
        }
        leg(session, m, nowMs);
    }
    // A third of the crowd loses and regains health (a fight's values updates: not the look's fields).
    if (static_cast<i32>(nowMs - m_healthAtMs) >= 0) {
        m_healthAtMs = nowMs + 2000;
        for (usize i = 0; i < m_members.size(); i += 3) {
            Member& m = m_members[i];
            m.health = m.health == 100 ? 60 : 100;
            m_body.clear();
            net::ByteWriter w(m_body);
            w.u32v(1).u8v(0).u8v(0).packedGuid(m.guid); // UPDATETYPE_VALUES
            writeValues(w, {{uf::UNIT_FIELD_HEALTH, m.health}}, m.player ? uf::PLAYER_END : uf::UNIT_END);
            send(session, static_cast<u16>(Opcode::SMSG_UPDATE_OBJECT), nowMs);
        }
    }
    // A creature draws and sheathes a sword (its look's fields).
    if (static_cast<i32>(nowMs - m_gearAtMs) >= 0 && !m_members.empty()) {
        m_gearAtMs = nowMs + 10000;
        m_gearToggle ^= 1u;
        m_body.clear();
        net::ByteWriter w(m_body);
        w.u32v(1).u8v(0).u8v(0).packedGuid(m_members[0].guid);
        writeValues(w,
                    {{uf::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY, m_gearToggle ? kSword : 0u},
                     {uf::UNIT_VIRTUAL_ITEM_INFO, m_gearToggle ? kSwordInfo : 0u}},
                    m_members[0].player ? uf::PLAYER_END : uf::UNIT_END);
        send(session, static_cast<u16>(Opcode::SMSG_UPDATE_OBJECT), nowMs);
    }
    if (static_cast<i32>(nowMs - m_logAtMs) >= 0) {
        m_logAtMs = nowMs + 5000;
        const Units::UpdateCost& cost = units.updateCost();
        VD_LOG_INFO(
            "Motion demo: %u units | Units::update mean %.3f ms (steady %.3f), worst %.3f ms | looks read "
            "%.2f, rebuilt %.2f per frame",
            cost.units, cost.meanMs, cost.steadyMs, cost.worstMs, cost.looksRead, cost.looksRebuilt);
    }
}

} // namespace vd
