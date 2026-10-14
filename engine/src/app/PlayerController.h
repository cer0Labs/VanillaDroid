#pragma once

#include "core/Types.h"
#include "game/Transports.h"
#include "net/Messages.h"
#include "net/Opcodes.gen.h"
#include "physics/CharacterController.h"
#include "physics/Collision.h"
#include "platform/Input.h"
#include "world/Camera.h"

#include <glm/vec3.hpp>

#include <array>
#include <utility>

namespace vd {

namespace physics {
class CollisionWorld;
}
class Actors;
class GameSession;
struct MoveCommand;

// The player's character (phase 6): input -> the character controller over the map's collision -> the movement
// packets the server expects (MSG_MOVE_* when the state changes, a heartbeat every 500 ms while moving, the
// answers to its orders) -> a third-person camera that walls pull in. Online it moves the server's player; offline
// (development) a character of its own. Game thread only.
//
// Controls. Desktop: W/S forward and back, A/D strafe, Space jump, / walk or run, right mouse drag turns the camera
// (and the character with it while moving), wheel zooms. Touch: the HUD's stick moves the character where it points,
// relative to the camera (a short push walks), dragging the world turns the camera, pinching zooms, its jump button
// jumps (setTouchInput). Game controller: the same with the left stick, the right stick turns the camera, A jumps
// (setGamepadInput).
class PlayerController {
public:
    PlayerController();

    void onKey(Key key, bool down);
    void onLook(f32 dxPixels, f32 dyPixels);
    void onWheel(f32 steps);
    // Touch controls of this frame (the HUD's stick and camera area).
    struct TouchInput {
        glm::vec2 stick{0.0f};  // -1..1: x right, y forward (up on the screen), relative to the camera
        f32 yawDelta = 0.0f;    // camera turn, radians
        f32 pitchDelta = 0.0f;
        f32 zoomSteps = 0.0f;   // as mouse wheel steps (positive: closer)
        bool jump = false;
    };
    void setTouchInput(const TouchInput& input) { m_touchInput = input; }
    // Phase 15 (options): UIOptionsFrame's camera options as they map to this controller. CAMERA_FOLLOWING_STYLE
    // (cameraSmoothStyle): Smart swings the camera behind the character while it runs, Always also brings it back once
    // the player stops turning it, Never leaves it where it is; at AUTO_FOLLOW_SPEED (cameraYawSmoothSpeed, degrees a
    // second). Only towards a heading within the cone in front of the camera: the stick turns the character to where it
    // points, so a camera chasing a character that runs at it or sideways would spin (a run forward or diagonally is
    // what the original's camera follows). MAX_FOLLOW_DIST: the farthest zoom, cameraDistanceMax (15 yards) times its
    // factor. FOLLOW_TERRAIN tilts the view with the slope run on, HEAD_BOB bobs the first person view, WATER_COLLISION
    // keeps the camera on the character's side of a water surface.
    struct CameraOptions {
        u32 followStyle = 0;        // 0 never (the camera before phase 15), 1 smart, 2 always
        f32 followSpeed = 180.0f;   // degrees a second
        f32 maxDistance = 25.0f;    // yards (the controller's before phase 15)
        bool terrainTilt = false;
        bool bobbing = false;
        bool waterCollision = false;
    };
    void setCameraOptions(const CameraOptions& options) { m_cameraOptions = options; }
    // Game controller, held state (set every frame, added to the touch and keyboard input): the left stick moves like
    // the touch stick (relative to the camera, a short push walks), the right stick turns the camera, A jumps.
    void setGamepadInput(const GamepadState& pad) { m_gamepad = pad; }
    f32 cameraYaw() const { return m_cameraYaw; }
    // This frame, while the player does not move it: turn the character to `facing` (radians) — the auto attack
    // and targeted spells face their target, as the original client's interact does.
    void setFacingGoal(f32 facing) {
        m_facingGoal = facing;
        m_hasFacingGoal = true;
    }
    void releaseAll();
    // Phase 13 (social): auto-follow (FollowUnit, app/UnitMenuScreens). This frame, unless the player moves by
    // itself: run facing `facing` (radians) when `move`, else stand. Any movement input of the player's own breaks
    // it instead: takeFollowBroken() says so once.
    void setFollowStep(bool move, f32 facing) {
        m_followActive = true;
        m_followMove = move;
        m_followFacing = facing;
    }
    bool takeFollowBroken() { return std::exchange(m_followBroken, false); }

    // Offline: a character of its own (a random look) standing at `feet`.
    void spawnLocal(Actors& actors, const glm::dvec3& feet, f32 facing);
    // Parity: the offline character's actor (~0u online or before spawnLocal): the player frame's portrait.
    u32 localActor() const { return m_active && !m_online ? m_actor : ~0u; }
    // Online: the server's player where the server put it (login, new world). The simulation waits until the
    // collision under it is loaded; nothing is sent meanwhile.
    void enterWorld(const glm::dvec3& feet, f32 facing);
    void leave(Actors* actors);
    bool active() const { return m_active; }
    // Phase 15 (options): the movement last sent moves the character (CLEAR_AFK).
    bool moving() const;
    bool online() const { return m_online; }
    bool waiting() const { return m_waiting; }
    // Phase 12: area triggers may be entered: not waiting, and not right after a near teleport. vMaNGOS reads
    // CMSG_AREATRIGGER and MSG_MOVE_TELEPORT_ACK in different queues (PACKET_PROCESS_SELF_ITEMS, _MOVEMENT): a
    // trigger sent in the frames after the ack can be checked against the position before the teleport and dropped
    // as "too far" (a GM teleport into an instance portal, a ghost moved to its corpse's entrance).
    static constexpr u32 kTriggerAfterTeleportMs = 500;
    bool triggersAllowed(u32 nowMs) const {
        return !m_waiting && nowMs - m_teleportedMs >= kTriggerAfterTeleportMs && m_unit == m_playerGuid;
    }
    // Phase 8: the server is moving the character along a spline of its own (a taxi flight, a scripted move):
    // nothing is simulated nor sent until it ends (then CMSG_MOVE_SPLINE_DONE).
    bool onServerSpline() const { return m_onSpline; }
    // Phase 13 (combat): online, the unit the character simulation stands for (the player; a Mind Controlled unit or
    // the pet with Eyes of the Beast while the client moves it) and whether the client moves it now (false: feared,
    // confused, charmed: it only follows what the server says, nothing is simulated nor sent).
    u64 characterUnit() const { return m_unit; }
    bool controlling() const { return !m_online || m_controlling; }
    // Development (offline, VD_FAKE_CONTROL=lost[:seconds]): the local character loses control for that long, as a
    // fear would take it (the input does nothing, the HUD reacts as to PLAYER_CONTROL_LOST).
    void fakeControlLoss(u32 nowMs, u32 durationMs) { m_fakeLostUntilMs = nowMs + durationMs; m_fakeLost = true; }
    bool fakeControlLost() const { return m_fakeLost; }
    // Phase 8: transports (app/Transports): which transport a moving collision object belongs to, and where a
    // transport is now. Standing on one, the character is carried by it and its movement packets carry
    // MOVEFLAG_ONTRANSPORT with its offset on it.
    struct TransportQuery {
        u64 (*transportOfObject)(const void* context, u32 collisionObject) = nullptr;
        bool (*placement)(const void* context, u64 transport, game::TransportPlacement& out) = nullptr;
        const void* context = nullptr;
        bool (*solid)(const void* context, u64 transport) = nullptr; // its deck collides
    };
    void setTransports(const TransportQuery& query) { m_transportQuery = query; }
    u64 transport() const { return m_transport; } // 0: on solid ground
    // After a far teleport on a transport (a zeppelin changing continents): the character waits, nothing sent,
    // until that transport is in sight and solid, then stands at `offset` on it (its facing on it: `facing`).
    void boardOnArrival(u64 transport, const glm::vec3& offset, f32 facing);

    // Once per frame, after the session's packets: the server's orders, then input, simulation and what to send.
    // `session` is null offline; `actors` draws the offline character.
    void update(GameSession* session, Actors* actors, const physics::CollisionWorld& world, u32 nowMs, f32 dt,
                f32 contentScale);
    // Third person: orbiting the character's head, pulled in by what is in between. Phase 14: zoomed all the way in,
    // first person (the camera at the eyes).
    void updateCamera(const physics::CollisionWorld& world, Camera& camera, f32 dt);
    // Phase 14: the character's opacity for the camera (Actors::setCameraFade): 0 in first person; translucent down
    // to PlayerFadeOutAlpha while the camera is in the character (backed against a wall), back to 1 out of it, at
    // the client's PlayerFadeOut/InRate.
    f32 cameraFade() const { return m_cameraFade; }
    bool firstPerson() const { return m_cameraDistance <= 0.0f; }

    const physics::CharacterController& character() const { return m_character; }
    physics::CharacterController& character() { return m_character; }
    // Character shape from its model (CreatureModelData collision width and height).
    void setShape(f32 radius, f32 height);
    // Debug panel contents (inside an ImGui window).
    void drawPanel();

    // Bot input (route tests): overrides the player's input while `drive` is set.
    struct Drive {
        bool active = false;
        physics::CharacterInput input;
    } drive;
    // Development: logs every movement packet sent (opcode, flags, fall time, position).
    bool traceMovement = false;

private:
    physics::CharacterInput readInput(f32 contentScale);
    void applyCommand(GameSession& session, const MoveCommand& command, u32 nowMs);
    // Phase 13 (combat): game::MoverControl wants another mover (or none): what the server is told, in its order
    // (CMSG_MOVE_NOT_ACTIVE_MOVER, CMSG_SET_ACTIVE_MOVER, CMSG_MOVE_SPLINE_DONE), and the simulation moves to it.
    void switchMover(GameSession& session, u32 nowMs);
    // Without control: the character stands where the server's movement of its unit puts it.
    void followUnit(GameSession& session, u32 nowMs);
    // CMSG_MOVE_TIME_SKIPPED when this frame's step is longer than the client's (an Android pause, a long hitch).
    void reportSkippedTime(GameSession& session, u32 nowMs);
    u32& handledSpline() { return m_unit == m_playerGuid ? m_handledSpline : m_otherHandledSpline; }
    net::MovementInfo movementInfo(u32 flags) const;
    void send(GameSession& session, net::Opcode opcode, u32 flags, u32 nowMs);
    void sendChanges(GameSession& session, u32 nowMs);
    void animateLocal(Actors& actors);
    // Transports: moved with the one it stands on (before stepping), then which one it stands on (after).
    void carryOnTransport();
    void updateTransport(const physics::CollisionWorld& world, GameSession* session, u32 nowMs);
    // Follows the server's spline of the player's entity; true while it lasts. Its end (or a stop) is answered.
    bool followServerSpline(GameSession& session, u32 nowMs);
    void finishServerSpline(GameSession& session, const glm::vec3& at, f32 facing, u32 splineId, u32 nowMs);

    physics::CharacterController m_character;
    bool m_active = false;
    bool m_online = false;
    bool m_waiting = false;
    u32 m_teleportedMs = 0; // client clock of the last near teleport applied (triggersAllowed)

    std::array<bool, static_cast<usize>(Key::Count)> m_keys{};
    bool m_walk = false;
    TouchInput m_touchInput;
    f32 m_facingGoal = 0.0f;
    bool m_hasFacingGoal = false;
    bool m_followActive = false; // phase 13 (social): setFollowStep this frame
    bool m_followMove = false;
    f32 m_followFacing = 0.0f;
    bool m_followBroken = false;
    GamepadState m_gamepad;
    f32 m_dt = 0.0f; // this frame's, for the gamepad's turn rate
    f32 m_lookX = 0.0f;
    f32 m_lookY = 0.0f;
    u32 m_frame = 0;

    f32 m_cameraYaw = 0.0f;
    f32 m_cameraPitch = -0.3f;
    f32 m_cameraDistance = 7.0f; // wanted
    f32 m_shownDistance = 7.0f;  // after walls
    f32 m_cameraFade = 1.0f;     // phase 14
    CameraOptions m_cameraOptions; // phase 15 (options)
    f32 m_stillCameraSeconds = 0.0f; // since the player last turned the camera
    f32 m_terrainTilt = 0.0f;        // FOLLOW_TERRAIN: the slope's angle, smoothed
    f32 m_bobPhase = 0.0f;           // HEAD_BOB
    glm::dvec3 m_lastFeet{0.0};
    bool m_cameraCollision = true;
    physics::TriangleSet m_cameraSet;

    // What the server last received.
    u32 m_sentFlags = 0;
    u32 m_clientTime = 1;
    u32 m_lastSendMs = 0;
    f32 m_sentFacing = 0.0f;
    f32 m_sentPitch = 0.0f;
    u32 m_packets = 0;
    u32 m_heartbeats = 0;
    u32 m_acks = 0;
    u32 m_corrections = 0;
    u32 m_landings = 0;
    u32 m_jumps = 0;
    bool m_mountSpecialKey = false; // phase 18 (extras): the jump key held after rearing the mount

    TransportQuery m_transportQuery;  // phase 8
    u64 m_transport = 0;              // the transport it stands on
    glm::vec3 m_transportOffset{0.0f};
    f32 m_transportOrientation = 0.0f; // the transport's, when the offset was taken
    u32 m_boardings = 0;
    bool m_boardPending = false; // boardOnArrival: waiting for the transport
    bool m_onSpline = false;  // following the server's spline (phase 8)
    u32 m_handledSpline = 0;  // the last spline id answered with CMSG_MOVE_SPLINE_DONE
    u32 m_splinesDone = 0;

    // Phase 13 (combat): the unit simulated online and whether the client moves it; the last answered spline of a
    // unit other than the player; the last frame's clock (the time skipped); the camera on a far sight object.
    u64 m_playerGuid = 0;
    u64 m_unit = 0;
    bool m_controlling = true;
    u32 m_otherHandledSpline = 0;
    u32 m_lastUpdateMs = 0;
    u32 m_timeSkips = 0;
    u32 m_skippedMs = 0;
    u32 m_switches = 0;
    bool m_viewElsewhere = false;
    glm::dvec3 m_viewPosition{0.0};
    u32 m_fakeLostUntilMs = 0;
    bool m_fakeLost = false;

    u32 m_actor = ~0u; // offline character
    u32 m_lookSeed = 11;
    f32 m_actorYaw = 0.0f;
};

} // namespace vd
