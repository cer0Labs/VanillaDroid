#pragma once

#include "core/Types.h"
#include "physics/Collision.h"

#include <glm/vec3.hpp>

#include <optional>

namespace vd::physics {

class CollisionWorld;

// MOVEFLAG_* of the protocol (vMaNGOS src/game/Objects/MovementInfo.h).
namespace moveflag {
constexpr u32 kForward = 0x00000001;
constexpr u32 kBackward = 0x00000002;
constexpr u32 kStrafeLeft = 0x00000004;
constexpr u32 kStrafeRight = 0x00000008;
constexpr u32 kTurnLeft = 0x00000010;
constexpr u32 kTurnRight = 0x00000020;
constexpr u32 kPitchUp = 0x00000040;
constexpr u32 kPitchDown = 0x00000080;
constexpr u32 kWalkMode = 0x00000100;
constexpr u32 kRoot = 0x00001000;
constexpr u32 kJumping = 0x00002000; // airborne: jumping or falling
constexpr u32 kFallingFar = 0x00004000;
constexpr u32 kSwimming = 0x00200000;
constexpr u32 kSplineEnabled = 0x00400000;
constexpr u32 kOnTransport = 0x02000000;
constexpr u32 kSplineElevation = 0x04000000;
constexpr u32 kWaterWalking = 0x10000000;
constexpr u32 kSafeFall = 0x20000000;
constexpr u32 kHover = 0x40000000;
constexpr u32 kDirections = kForward | kBackward | kStrafeLeft | kStrafeRight;
constexpr u32 kAirborne = kJumping | kFallingFar;
// MOVEFLAG_MASK_MOVING: while any is set the client sends a heartbeat every 500 ms.
constexpr u32 kMoving = kDirections | kPitchUp | kPitchDown | kJumping | kFallingFar | kSplineElevation;
} // namespace moveflag

// Movement constants of the 1.12 client, as vMaNGOS reproduces them (src/game/Movement/spline/util.cpp and
// MovementHandler.cpp).
constexpr f32 kGravity = 19.29110527f;        // yards/s^2
constexpr f32 kTerminalVelocity = 60.148003f; // yards/s
constexpr f32 kSafeFallVelocity = 7.0f;       // terminal velocity with feather fall / slow fall
constexpr f32 kJumpVelocity = 7.95797334f;    // initial upward speed of a jump

// Base speeds (baseMoveSpeed in Unit.cpp); the server changes them with SMSG_FORCE_*_SPEED_CHANGE.
struct CharacterSpeeds {
    f32 walk = 2.5f;
    f32 run = 7.0f;
    f32 runBack = 4.5f;
    f32 swim = 4.722222f;
    f32 swimBack = 2.5f;
    f32 turnRate = 3.141594f;
};

// What the player asks for this frame. The protocol only knows 8 directions: movement is digital.
struct CharacterInput {
    bool forward = false;
    bool backward = false;
    bool strafeLeft = false;
    bool strafeRight = false;
    bool jump = false; // held: jumps again on landing, like the client
    bool walk = false;
    f32 facing = 0.0f; // radians from +X (north) towards +Y, the server's orientation
    f32 pitch = 0.0f;  // radians, where a swimming character heads
};

enum class MotionMode : u8 { Ground, Air, Swim };

// The player's own movement (phase 6): a vertical capsule over the baked collision, moved with the client's
// speeds, gravity and jump. On the ground it slides along walls, climbs steps up to `stepHeight`, follows slopes
// up to `maxSlopeDegrees` and keeps its feet on the surface; off a ledge or a jump it flies with the momentum it
// had (no air control; a wall stops the body, not the momentum) until it lands; in deep liquid it swims. The state is what the protocol reports: flags,
// fall time and jump data. Game thread only; positions are world coordinates (feet).
class CharacterController {
public:
    struct Events {
        bool jumped = false;       // MSG_MOVE_JUMP
        bool fell = false;         // left the ground without jumping (ledge, steep slope)
        bool landed = false;       // MSG_MOVE_FALL_LAND
        bool startedSwim = false;  // MSG_MOVE_START_SWIM
        bool stoppedSwim = false;  // MSG_MOVE_STOP_SWIM
    };
    // The jump in progress, as MovementInfo carries it (zspeed < 0 goes up).
    struct Jump {
        f32 zspeed = 0.0f;
        f32 cosAngle = 1.0f;
        f32 sinAngle = 0.0f;
        f32 xySpeed = 0.0f;
    };

    CharacterController();

    // Shape: collision width / 2 and height of the character's model (CreatureModelData), and the highest step it
    // walks up: the client's max(collision width * tan 50, radius + 1/720) (see setShape).
    f32 radius = 0.306f;
    f32 height = 2.031f;
    f32 stepHeight = 0.728f;
    // Sets radius and height, and the step height from them like the client.
    void setShape(f32 collisionRadius, f32 collisionHeight);
    // Climbing faster than this slope is a wall (the server's WallClimb check starts at 57 degrees); a character
    // only slides off surfaces 10 degrees steeper.
    f32 maxSlopeDegrees = 50.0f;
    CharacterSpeeds speeds;

    // Teleport: feet at `position`, standing (it settles on the ground or starts falling on the next step).
    void place(const glm::dvec3& position, f32 facing);
    // Moves by `dt` seconds (split internally into steps of at most 1/60 s).
    void step(const CollisionWorld& world, const CharacterInput& input, f32 dt);
    // Knockback (SMSG_MOVE_KNOCK_BACK): airborne with that velocity; speedZ < 0 goes up (protocol sign).
    void launch(f32 cosAngle, f32 sinAngle, f32 speedXY, f32 speedZ);
    void setRooted(bool rooted);
    // Phase 8: moved by what it stands on (a transport's deck): `delta` yards and `turn` radians of facing, keeping
    // its state (on the ground, in the air, swimming).
    void carry(const glm::dvec3& delta, f32 turn);
    // Server-granted states: kSafeFall, kWaterWalking, kHover.
    void setAbility(u32 flag, bool on);

    const glm::dvec3& position() const { return m_position; }
    f32 facing() const { return m_facing; }
    f32 pitch() const { return m_pitch; }
    MotionMode mode() const { return m_mode; }
    u32 flags() const { return m_flags; }
    bool rooted() const { return (m_flags & moveflag::kRoot) != 0; }
    // Airborne: ms since the character left the ground (MovementInfo::fallTime).
    u32 fallTimeMs() const { return static_cast<u32>(m_airTime * 1000.0f); }
    // The whole flight that ended last (on the ground or in the water): what the fall-end packet reports, even when a
    // new flight started in the same step.
    u32 endedFallTimeMs() const { return static_cast<u32>(m_endedAirTime * 1000.0f); }
    const Jump& jump() const { return m_jump; }
    const Events& events() const { return m_events; }
    // Horizontal speed the current flags move at (0 when standing).
    f32 speed() const;
    // Liquid surface found by the last step (NaN when none).
    f32 liquidLevel() const { return m_liquidLevel; }
    f32 swimDepth() const { return height * 0.75f; } // Unit::GetMinSwimDepth: the client swims from there

private:
    void updateFlags(const CharacterInput& input);
    void gatherAround(const CollisionWorld& world, f32 dt);
    void stepGround(glm::vec3& p, const glm::vec3& motion, f32 h);
    void stepAir(glm::vec3& p, f32 h);
    void stepSwim(glm::vec3& p, const glm::vec3& motion);
    // Collide-and-slide of the capsule (`full`) or of the body above the step height; `horizontalSlide` treats
    // every surface touched as a vertical wall.
    void slideBody(glm::vec3& p, glm::vec3 motion, bool full, bool horizontalSlide);
    std::optional<f32> groundBelow(const glm::vec3& p, f32 up, f32 down, glm::vec3* normal = nullptr) const;
    // Standing at `feet` on a surface with `normal` too steep to stand on: is there room to slide down?
    bool canSlide(const glm::vec3& feet, const glm::vec3& normal) const;
    void checkLiquid(const CollisionWorld& world, glm::vec3& p);
    void startFalling(const glm::vec3& velocity, bool jumped);
    void land();

    glm::dvec3 m_position{0.0};
    f32 m_facing = 0.0f;
    f32 m_pitch = 0.0f;
    MotionMode m_mode = MotionMode::Ground;
    u32 m_flags = 0;
    glm::vec3 m_velocity{0.0f}; // airborne
    f32 m_airTime = 0.0f;
    f32 m_endedAirTime = 0.0f;
    u32 m_stillInAir = 0; // consecutive air steps without moving while resting on something
    f32 m_fallStartZ = 0.0f; // world Z where the character left the ground
    Jump m_jump;
    Events m_events;
    f32 m_liquidLevel = 0.0f;
    bool m_hasLiquid = false;
    glm::vec3 m_moveDirection{0.0f}; // unit horizontal direction of the flags (zero when none)
    f32 m_moveSpeed = 0.0f;
    TriangleSet m_set;
};

} // namespace vd::physics
