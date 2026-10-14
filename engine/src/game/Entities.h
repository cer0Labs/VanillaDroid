#pragma once

#include "core/Types.h"
#include "net/Messages.h"
#include "net/SplineMessages.h" // phase 18 (motion)

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace vd::game {

// TYPEID_* of the server (ObjectGuid.h): the object type sent in create blocks.
enum class ObjectType : u8 { Object = 0, Item = 1, Container = 2, Unit = 3, Player = 4, GameObject = 5,
                             DynamicObject = 6, Corpse = 7 };
const char* objectTypeName(ObjectType type);

// Speeds of a unit (UnitMoveType order of the server).
enum class SpeedType : u8 { Walk = 0, Run = 1, RunBack = 2, Swim = 3, SwimBack = 4, TurnRate = 5, Count = 6 };

// Phase 18 (motion): where a transport is at a moment (game/Transports.h), for the passengers' world
// positions. The app sets one resolver (app/Transports: ships, zeppelins and elevators by their path and
// time); without one, or for a transport it cannot place, a passenger stays where its last packet put it.
struct TransportPlacement;
using TransportResolver = bool (*)(const void* context, u64 transport, u32 nowMs, TransportPlacement& out);
void setTransportResolver(TransportResolver resolver, const void* context);

// A world object as the server describes it: update fields as the protocol's plain u32 array, plus where it
// is and where it is going. Positions are the server's (map coordinates, yards).
struct Entity {
    u64 guid = 0;
    ObjectType type = ObjectType::Object;
    std::vector<u32> values; // update fields (net/UpdateFields.gen.h), sized for the type at creation
    bool hasPosition = false;
    glm::vec3 position{0.0f}; // where it was last told to be (the start of its spline)
    f32 orientation = 0.0f;
    f32 pitch = 0.0f;   // swimming
    u32 moveFlags = 0;  // MOVEFLAG_*
    u32 moveTimeMs = 0; // client clock when position/moveFlags last came in
    f32 speeds[static_cast<u32>(SpeedType::Count)] = {2.5f, 7.0f, 4.5f, 4.722222f, 2.5f, 3.141594f};
    // Airborne player (MSG_MOVE_* with MOVEFLAG_JUMPING): its motion when the packet came in.
    glm::vec2 airVelocity{0.0f};
    f32 verticalSpeed = 0.0f; // up > 0

    // Spline in progress (SMSG_MONSTER_MOVE or a create block): the path from `position` to `destination`, walked
    // at constant speed in `splineDuration` ms from `splineStartMs` (client clock); cyclic ones loop.
    bool moving = false;
    bool cyclic = false;
    u32 splineFlags = 0; // MoveSplineFlag (0x100 run mode, 0x200 flying)
    u32 splineId = 0;    // the server's id of the last spline (CMSG_MOVE_SPLINE_DONE echoes it for the player)
    bool splineStopped = false; // the last SMSG_MONSTER_MOVE was a stop (MonsterMoveStop)
    std::vector<glm::vec3> path; // every point, start and destination included
    f32 pathLength = 0.0f;
    // Flying splines (taxis, flying creatures) are Catmull-Rom curves through the points, as the server and the
    // client evaluate them; the time of each segment follows its length: where each segment ends, 0..1 of the
    // duration (empty: straight segments at constant speed).
    std::vector<f32> segmentEnds;
    glm::vec3 destination{0.0f};
    u32 splineStartMs = 0;
    u32 splineDuration = 0;
    // Where it faces when the spline ends (SMSG_MONSTER_MOVE facing): an angle, or another object.
    bool hasFinalAngle = false;
    f32 finalAngle = 0.0f;
    u64 facingTarget = 0;

    // A new position that disagrees with the one shown eases in: this offset fades out over kSmoothMs.
    glm::vec3 correction{0.0f};
    u32 correctionMs = 0;

    // Phase 8, transports. A transport game object (elevator, ship, zeppelin): the time into its cycle the create
    // block told (UPDATEFLAG_TRANSPORT) and the client clock then; its position comes from its path (game/Transports).
    bool hasTransportTime = false;
    u32 transportTime = 0;
    u32 transportTimeAt = 0;
    // A unit on a transport (MOVEFLAG_ONTRANSPORT): which, and where on it (its offset and facing in the
    // transport's frame; `position` is the world position at the last packet).
    // Phase 18 (motion): a passenger moves in its transport's frame, as WoW.exe's CMovement does once it is on
    // one (0x617170): its dead reckoning from transportOffset / transportFacing, its spline
    // (SMSG_MONSTER_MOVE_TRANSPORT, or a create block on board) through points of that frame, its final facing
    // an angle of that frame. A plain SMSG_MONSTER_MOVE takes it off (0x617100 with no transport).
    u64 transportGuid = 0;
    glm::vec3 transportOffset{0.0f};
    f32 transportFacing = 0.0f;

    // Phase 18 (motion): the change mask. The values' version (new at every values or create block that
    // changes a field, never repeated in the store), and the store frame (EntityStore::frame) of the last
    // change with the update-mask words it touched then (bit i: fields 32 i .. 32 i + 31; bit 63 also every
    // field past them). A reader that runs once per frame after the packets skips an entity whose
    // changedFrame is not the store's frame.
    u32 valuesVersion = 0;
    u32 changedFrame = 0;
    u64 changedWords = 0;
    u32 createdFrame = 0;
    // Whether fields of any of `words` (an update-mask word mask) changed in store frame `frame`.
    bool changedIn(u32 frame, u64 words) const {
        return changedFrame == frame && (changedWords & words) != 0;
    }

    u32 value(u16 index) const { return index < values.size() ? values[index] : 0; }
    u8 byteValue(u16 index, u32 byte) const { return static_cast<u8>(value(index) >> (byte * 8)); }
    f32 speed(SpeedType kind) const { return speeds[static_cast<u32>(kind)]; }
    // World position at `nowMs`: along its spline, or carried forward from its last MSG_MOVE_* (directions,
    // jumps, falls, swimming) for up to a second, plus the fading correction. Phase 18 (motion): a passenger's
    // is its place on the transport, wherever the transport is then.
    glm::vec3 positionAt(u32 nowMs) const;
    // Phase 18 (motion): the same in its own frame (the transport's for a passenger, else the world).
    glm::vec3 localAt(u32 nowMs) const;
    // Without the correction, in its own frame.
    glm::vec3 predictedAt(u32 nowMs) const;
    // Ground speed (yards per second) at `nowMs`, 0 when standing still.
    f32 speedAt(u32 nowMs) const;
    // Direction it moves in at `nowMs` (world radians), or false when standing.
    bool headingAt(u32 nowMs, f32& heading) const;
    // Phase 18 (motion): where it faces in the world when it stands (a passenger turns with its transport),
    // and an angle of its own frame (a spline's final angle) in the world.
    f32 facingAt(u32 nowMs) const;
    f32 worldAngle(f32 local, u32 nowMs) const;
    // Phase 18 (motion): its spline is running at `nowMs`.
    bool onSpline(u32 nowMs) const;
    // Moving without the run flag (splines) or in walk mode (players).
    bool walking() const;
    bool airborne() const;
};

struct UpdateStats {
    u32 created = 0;
    u32 updated = 0;
    u32 moved = 0;
    u32 removed = 0;
    std::vector<u64>* createdGuids = nullptr; // when set, receives the GUID of every object created (phase 7)
};

// All known objects, by GUID. Fed by the world packets on the main thread.
class EntityStore {
public:
    static constexpr u32 kSmoothMs = 200; // corrections of the shown position blend in over this

    // SMSG_UPDATE_OBJECT (after inflating SMSG_COMPRESSED_UPDATE_OBJECT). False if malformed: whatever was
    // parsed before the error is kept.
    bool applyUpdateObject(std::span<const u8> payload, u32 nowMs, UpdateStats* stats = nullptr);
    // SMSG_DESTROY_OBJECT.
    bool applyDestroy(std::span<const u8> payload);
    // MSG_MOVE_* of another object: packed GUID + MovementInfo (+ a speed for MSG_MOVE_SET_*_SPEED). `guid`
    // receives the mover's GUID.
    bool applyMove(std::span<const u8> payload, u32 nowMs = 0, u64* guid = nullptr);
    // MSG_MOVE_SET_*_SPEED: packed GUID + MovementInfo + speed.
    bool applyMoveSpeed(std::span<const u8> payload, SpeedType type, u32 nowMs);
    // SMSG_SPLINE_SET_*_SPEED: packed GUID + speed. Phase 18 (motion): the movement it carries forward
    // restarts from where it is (WoW.exe's CMovement speed setters, 0x7C7030: 0x7C5CD0).
    bool applySplineSpeed(std::span<const u8> payload, SpeedType type, u32 nowMs = 0);
    // SMSG_MONSTER_MOVE.
    bool applyMonsterMove(std::span<const u8> payload, u32 nowMs);
    // Phase 18 (motion): SMSG_MONSTER_MOVE_TRANSPORT: the same spline in the transport's frame, the unit on it.
    bool applyMonsterMoveTransport(std::span<const u8> payload, u32 nowMs);
    // Phase 18 (motion): SMSG_SPLINE_MOVE_* (net/SplineMessages): a movement state of a unit the server moves,
    // as WoW.exe's CMovement applies it (0x601420): root (MOVEFLAG_ROOT; its directions cleared and a running
    // spline ended, 0x619A10), unroot, water walking, feather fall, hover, swimming (its pitch cleared when it
    // stops, 0x7C6E80) and walk / run mode (0x7C71C0). What it carries forward restarts from where it is.
    bool applySplineMove(std::span<const u8> payload, net::SplineMoveKind kind, u32 nowMs);
    // The player's own movement, simulated locally (phase 6): the server does not echo it back.
    void setMovement(u64 guid, const net::MovementInfo& info, u32 nowMs);
    // Phase 13 (combat): MSG_MOVE_TIME_SKIPPED of another unit: packed GUID + the ms its client did not simulate.
    bool applyTimeSkipped(std::span<const u8> payload);
    // Phase 13: a speed the client itself set (an order it acknowledged for the unit it moves).
    void setSpeed(u64 guid, SpeedType type, f32 speed);
    void clear();

    const Entity* find(u64 guid) const;
    usize size() const { return m_index.size(); }

    // Phase 18 (motion): the change mask's frames. nextFrame() starts one (GameSession::update, before the
    // packets): what changes after it is this frame's. changed(): every entity created or whose values changed
    // in this frame; removed(): every one gone in it (out of range, destroyed, cleared).
    void nextFrame();
    u32 frame() const { return m_frame; }
    std::span<const u64> changed() const { return m_changed; }
    std::span<const u64> removed() const { return m_removed; }
    // Phase 18 (motion): the update-mask word of a field, as Entity::changedWords keeps it.
    static constexpr u64 wordOf(u16 field) {
        return field / 32 < 63 ? (u64{1} << (field / 32)) : (u64{1} << 63);
    }
    template <typename F>
    void forEach(F&& f) const {
        for (const Entity& e : m_entities) {
            if (e.guid != 0) {
                f(e);
            }
        }
    }
    u32 countOf(ObjectType type) const;

private:
    Entity* get(u64 guid);
    Entity& create(u64 guid, ObjectType type);
    void remove(u64 guid);
    bool monsterMove(Entity* e, u64 transport, std::span<const u8> body, u32 nowMs); // phase 18 (motion)
    void markChanged(Entity& e, u64 words);                                           // phase 18 (motion)

    std::vector<Entity> m_entities; // slots; guid 0 = free
    std::vector<u32> m_free;
    std::unordered_map<u64, u32> m_index;
    u32 m_frame = 1;            // phase 18 (motion)
    u32 m_version = 0;          // the last Entity::valuesVersion given
    std::vector<u64> m_changed; // this frame's created and changed entities
    std::vector<u64> m_removed;
};

} // namespace vd::game
