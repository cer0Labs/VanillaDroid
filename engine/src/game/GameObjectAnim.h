#pragma once

#include "core/Types.h"
#include "game/Entities.h"

namespace vd::game {

// GameobjectTypes (vMaNGOS GameObjectDefines.h) this area reads (the others are in game/UnitInfo.h).
constexpr u32 kGameObjectTrap = 6;
constexpr u32 kGameObjectText = 9;         // plaques and signs: the client reads their page itself
constexpr u32 kGameObjectFishingNode = 17; // the fishing bobber

// How far a tap may use the object from (GameObjectInfo::GetInteractionDistance): the fishing bobber from where it
// landed (100 yards), the rest `otherwise` (the client's usual reach).
f32 gameObjectUseYards(const Entity& object, f32 otherwise);

// GOState (vMaNGOS GameObjectDefines.h), the GAMEOBJECT_STATE update field.
constexpr u32 kGoStateActive = 0;            // used: a door open, a chest being looted, the bobber that bit
constexpr u32 kGoStateReady = 1;             // as spawned: a door closed, a chest shut
constexpr u32 kGoStateActiveAlternative = 2; // the other use: a door broken down, a destroyed object

// The game object animations of AnimationData.dbc (5875) their M2s carry.
namespace goanim {
constexpr u16 kStand = 0;
constexpr u16 kSpawn = 145;
constexpr u16 kClose = 146;
constexpr u16 kClosed = 147;
constexpr u16 kOpen = 148;
constexpr u16 kOpened = 149;
constexpr u16 kDestroy = 150;
constexpr u16 kDestroyed = 151;
constexpr u16 kRebuild = 152;
constexpr u16 kCustom0 = 153; // ..156 Custom3
constexpr u16 kDespawn = 157;
constexpr u16 kNone = 0xFFFF;
// The first and last game object animation (a model with none of them stays a static doodad).
constexpr u16 kFirst = kSpawn;
constexpr u16 kLast = kDespawn;
} // namespace goanim

// The GameObjectDisplayInfo.dbc sound (Sound[10], field 2 + index) of each animation: 0 Stand, 1 Open, 2
// Loop, 3 Close, 4 Destroy, 5 Opened, 6..9 Custom0..3 (read off the 1.12 rows: ChestOpen / ChestClose,
// DeadMineDoorOpen / Close / Destroy, the fishing bobber's "Fishing Hooked" in 6). kNone for one without a
// sound slot.
u32 gameObjectSoundSlot(u16 animation);

// What a game object's model plays.
struct GameObjectAnimRequest {
    u16 animation = goanim::kStand;
    bool loop = true; // false: once, holding its last frame
    bool operator==(const GameObjectAnimRequest&) const = default;
};

// Phase 13 (living world objects): how a game object's M2 follows what the server says about it — the loop of
// its state (closed, open, destroyed), the transition into a new state (Open, Close, Destroy, Rebuild) and
// the one-shot animations of SMSG_GAMEOBJECT_CUSTOM_ANIM / SPAWN_ANIM / DESPAWN_ANIM — with the model's own
// animations: what it lacks is skipped (a bobber has Stand and Custom0 only; a door may have no Closed loop,
// then Stand). Pure logic over a "has this animation" query, so tests need no models; app/ServerObjects plays
// the requests on the actor.
class GameObjectAnimator {
public:
    // Asks whether the model has an animation.
    using Has = bool (*)(const void* context, u16 animation);

    // The object appeared in `state` (no transition: it was already so).
    void appear(u32 state);
    // GAMEOBJECT_STATE changed: its transition, then the new loop.
    void changeState(u32 state);
    // SMSG_GAMEOBJECT_CUSTOM_ANIM: Custom<index> once.
    void custom(u32 index);
    void spawn();   // SMSG_GAMEOBJECT_SPAWN_ANIM
    void despawn(); // SMSG_GAMEOBJECT_DESPAWN_ANIM: Despawn once, held (the object goes away)
    // SMSG_GAMEOBJECT_RESET_STATE: the one-shot stops, back to the state's loop.
    void resetState();

    // What to play now. `oneShotDone`: the one-shot last returned (loop false) reached its end on the model.
    GameObjectAnimRequest next(Has has, const void* context, bool oneShotDone);

    u32 state() const { return m_state; }
    bool despawning() const { return m_oneShot == goanim::kDespawn; }
    u16 oneShot() const { return m_oneShot; } // the one-shot pending or playing, goanim::kNone if none

private:
    u32 m_state = kGoStateReady;
    u16 m_oneShot = goanim::kNone;
    bool m_started = false; // the one-shot was handed out (its end can be waited for)
};

} // namespace vd::game
