#pragma once

#include "app/WorldTap.h"
#include "core/Types.h"
#include "db/DbcView.h"
#include "game/GameObjectAnim.h"
#include "pack/AsyncReader.h"
#include "pack/PackReader.h"

#include <glm/vec3.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace vd {

namespace physics {
class CollisionWorld;
}
class Actors;
class GameSession;
class WorldObjects;

// The game objects the server spawns (chests, quest objects, doors, mailboxes...) drawn with the world's models
// (phase 7): each GameObject entity of the session gets a dynamic instance of its display's M2
// (GameObjectDisplayInfo.dbc), placed by its position, orientation and scale, and loses it when the entity goes.
// Phase 13 (living world objects): those whose M2 animates (doors, chests, levers, the fishing bobber: actors.vdpk
// has them) are actors instead, playing what game::GameObjectAnimator says from their GAMEOBJECT_STATE and the
// server's SMSG_GAMEOBJECT_CUSTOM_ANIM / SPAWN_ANIM / DESPAWN_ANIM / RESET_STATE (Gameplay::fx()). Closed doors
// block the way with their model's collision (collision_objects.vdpk, read on an IO thread), open ones let pass.
// Game thread only.
class ServerObjects {
public:
    bool init(const pack::PackReader& db);
    // Phase 13: where animated game objects are drawn (nullptr: every object a static instance).
    void setActors(Actors* actors) { m_actors = actors; }
    // Phase 13: where closed doors collide (GameObject::UpdateCollisionState: a door collides while
    // GO_STATE_READY), their collision from collision_objects.vdpk in `packsDir` (nullptr: doors never block).
    void setCollision(physics::CollisionWorld* world, const std::filesystem::path& packsDir);
    // Releases the collision objects and stops the IO thread (before the collision world and the packs close).
    void shutdown(WorldObjects& objects);
    // Once per frame, after the session's packets.
    void update(const GameSession& session, WorldObjects& objects, u32 nowMs);
    void clear(WorldObjects& objects);
    u32 count() const { return static_cast<u32>(m_instances.size()); }
    // Parity (tapping objects): the objects whose model is loaded, with its world bounding sphere (`out`
    // cleared). The templates of those the player may use are asked for once (their name and lock are known
    // by the time they are tapped).
    void collectViews(GameSession& session, const WorldObjects& objects,
                      std::vector<WorldObjectView>& out) const;

    // Development (phase 13, --spawn gameobject:<displayId>): the display's model at `position`, going through what
    // the server makes objects do every few seconds (closed, open, Custom0, closed, destroyed, rebuilt) without a
    // server. False when the display has no model.
    bool spawnDemo(u32 displayId, const glm::dvec3& position, f32 facing, u32 nowMs);

private:
    static constexpr u32 kNone = ~0u;
    struct Placed {
        u32 instance = kNone; // WorldObjects dynamic instance (static models)
        u32 actor = kNone;    // Actors actor (phase 13: animated models)
        u32 displayId = 0;
        u32 type = 0; // GAMEOBJECT_TYPE_ID
        f32 x = 0.0f, y = 0.0f, z = 0.0f, facing = 0.0f, scale = 0.0f;
        u32 seen = 0; // m_frame when last found among the entities
        // Phase 13: the animation of an actor.
        game::GameObjectAnimator animator;
        game::GameObjectAnimRequest playing{game::goanim::kNone, true};
        u32 state = 0xFFFFFFFFu; // GAMEOBJECT_STATE last seen (none yet)
        bool gone = false;       // the entity went while its despawn animation plays (kept until it ends)
        u32 goneMs = 0;
        // Phase 13: a closed door's collision (CollisionWorld object), asked for once while it should have one.
        u32 collision = kNone;
        bool collisionAsked = false;
    };
    struct Demo {
        Placed placed;
        u32 step = 0;
        u32 nextMs = 0;
    };

    u64 modelOf(u32 displayId) const;
    // Whether the display's model is drawn as an actor (cached per display).
    bool animated(u32 displayId);
    void readFx(const GameSession& session);
    // Plays what the animator asks for on the actor (once its model is there).
    void animate(Placed& placed);
    void release(Placed& placed, WorldObjects& objects);
    void updateDemos(u32 nowMs);
    // Phase 13: a door's collision follows its state (asked for, added when it arrives, removed when it opens).
    void updateCollision(u64 guid, Placed& placed);
    void takeCollision();
    u64 collisionOf(u32 displayId) const;

    db::DbcView m_displays;
    std::unordered_map<u32, u32> m_displayRecords; // displayId -> record
    std::unordered_map<u64, Placed> m_instances;   // GUID -> instance
    std::unordered_map<u32, bool> m_animated;      // displayId -> drawn as an actor
    Actors* m_actors = nullptr;
    physics::CollisionWorld* m_collision = nullptr;
    pack::PackReader m_collisionPack; // collision_objects.vdpk (before the reader: it outlives its requests)
    pack::AsyncReader m_reader;
    bool m_readerStarted = false;
    u32 m_fxCursor = 0;
    u64 m_fxPlayer = 0; // the character the cursor belongs to
    std::vector<Demo> m_demos;
    u32 m_frame = 0;
};

} // namespace vd
