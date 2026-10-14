#pragma once

#include "core/Types.h"
#include "db/DbcView.h"
#include "game/Entities.h"
#include "game/Transports.h"
#include "pack/AsyncReader.h"
#include "pack/PackReader.h"

#include <glm/vec3.hpp>

#include <filesystem>
#include <unordered_map>

namespace vd {

namespace physics {
class CollisionWorld;
}
class GameSession;
class WorldObjects;

// The server's transports in the world (phase 8): ships and zeppelins (MO_TRANSPORT) walk their TaxiPathNode
// cycle and elevators (TRANSPORT) their TransportAnimation.dbc offsets, timed by the transport time of their
// create block (game/Transports). Each is drawn as a dynamic instance of its model (WMO or M2) and collides
// through a moving object of the CollisionWorld (collision_objects.vdpk, read and decompressed on an IO
// thread), both moved every frame. Passengers (other units, the player) are placed from their offset on it.
// Game thread only.
class Transports {
public:
    bool init(const pack::PackReader& db, const game::TaxiData* taxi, const std::filesystem::path& packsDir);
    void shutdown(WorldObjects& objects, physics::CollisionWorld& collision);

    // Once per frame, after the session's packets.
    void update(GameSession& session, WorldObjects& objects, physics::CollisionWorld& collision, u32 nowMs);
    void clear(WorldObjects& objects, physics::CollisionWorld& collision);

    // Where a transport game object is now (false: not a transport known here, or not placed yet).
    bool placement(u64 guid, game::TransportPlacement& out) const;
    // Where a ship or zeppelin of `entry` is at `nowMs`, from the cycle time it had when last seen (across
    // maps: the arrival of a far teleport on it). False when its path or time are not known yet.
    bool placementOfEntry(u32 entry, u32 nowMs, game::TransportPlacement& out) const;
    // Its collision is in the world (a passenger can stand on it).
    bool solid(u64 guid) const;
    // The GUID of a ship or zeppelin (HIGHGUID_MO_TRANSPORT: the entry is its counter, the same on every
    // map).
    static u64 moTransportGuid(u32 entry) { return 0x1FC0000000000000ull | entry; }
    // The transport a moving collision object belongs to (0: none).
    u64 transportOfObject(u32 collisionObject) const;
    u32 count() const { return static_cast<u32>(m_transports.size()); }
    // Phase 18 (motion): where a transport is at `nowMs` exactly (not where the last update() left it: the units
    // placed before it in a frame ride with it), from its path and time (ships, zeppelins) or its animation
    // (elevators). The entity store's resolver (game::setTransportResolver) for the passengers.
    bool placementAt(u64 guid, u32 nowMs, game::TransportPlacement& out) const;
    static bool placementAtCallback(const void* self, u64 transport, u32 nowMs, game::TransportPlacement& out) {
        return static_cast<const Transports*>(self)->placementAt(transport, nowMs, out);
    }
    // For the player's controller (PlayerController::TransportQuery).
    static u64 transportOfObjectCallback(const void* self, u32 collisionObject) {
        return static_cast<const Transports*>(self)->transportOfObject(collisionObject);
    }
    static bool placementCallback(const void* self, u64 transport, game::TransportPlacement& out) {
        return static_cast<const Transports*>(self)->placement(transport, out);
    }
    static bool solidCallback(const void* self, u64 transport) {
        return static_cast<const Transports*>(self)->solid(transport);
    }
    static constexpr u64 kPreviewGuid = 0x1FC0000000000001ull; // HIGHGUID_MO_TRANSPORT

private:
    struct Transport {
        u32 entry = 0;
        u32 type = 0;
        u32 displayId = 0;
        u32 instance = ~0u;  // WorldObjects dynamic instance
        u32 collision = ~0u; // CollisionWorld moving object
        bool collisionAsked = false;
        bool placed = false;
        game::TransportPlacement placement;
        u32 seen = 0;
        // Phase 18 (motion): what places it at any moment: its cycle time minus the client clock, and an elevator's
        // own place and facing.
        u32 timeBase = 0;
        glm::vec3 origin{0.0f};
        f32 facing = 0.0f;
    };
    // Model and collision asset of a display (0: none).
    u64 modelOf(u32 displayId) const;
    u64 collisionOf(u32 displayId) const;
    bool isWmo(u32 displayId) const;
    bool place(GameSession& session, const game::Entity& e, Transport& t, u32 nowMs);
    // Draws a placed transport (its model's dynamic instance) and moves its collision (asked once).
    void show(u64 guid, Transport& t, u32 displayId, f32 scale, WorldObjects& objects,
              physics::CollisionWorld& collision);
    void forget(Transport& t, WorldObjects& objects, physics::CollisionWorld& collision);
    void updatePreview(WorldObjects& objects, physics::CollisionWorld& collision, u32 nowMs);

    const game::TaxiData* m_taxi = nullptr;
    db::DbcView m_displays;
    std::unordered_map<u32, u32> m_displayRecords; // GameObjectDisplayInfo id -> record
    game::TransportAnimations m_animations;
    std::unordered_map<u32, game::TransportPath>
        m_paths; // by game object entry (built once its template is known)
    std::unordered_map<u64, Transport> m_transports; // by GUID
    std::unordered_map<u32, u32> m_timeBases;        // by entry: cycle time - client clock when last seen
    pack::PackReader m_collisionPack;
    pack::AsyncReader m_reader;
    bool m_readerStarted = false;
    u32 m_frame = 0;
    // Development: VD_TRACE_TRANSPORTS=1 logs every transport's place every 2 s (online transport tests: when a ship
    // waits at the player's pier).
    bool m_trace = false;
    u32 m_traceAtMs = 0;
    // Development (VD_TRANSPORT_PREVIEW=<TaxiPath id>:<display id>[:speed:acceleration]): without a server, a
    // transport of that path and model docked at its first stop at start, to walk onto (the --walk
    // character).
    struct Preview {
        bool active = false;
        game::TransportPath path;
        u32 displayId = 0;
        u32 startMs = 0;
        u32 firstStop = 0; // ms into the cycle: its first arrival at a stop
    } m_preview;
};

} // namespace vd
