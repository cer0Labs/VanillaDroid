#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <optional>
#include <vector>

namespace vd {

class GameSession;
class Transports;
class Units;

// Phase 18 (motion, development): synthetic server units offline, fed to the session as the server's own
// packets (SMSG_UPDATE_OBJECT, SMSG_MONSTER_MOVE[_TRANSPORT], SMSG_SPLINE_MOVE_*, MSG_MOVE_*:
// net/SplineMessages' builders), so the entity store, app/Units and everything that reads them run as online.
// VD_MOTION_DEMO=<scenario>:
//   modes      a row of units around the character, one movement mode each, back and forth: a creature
//              walking, one running, one running slowed to 3.5 yd/s (it walks, by WoW.exe's rule), one
//              sprinting at 14 yd/s (a pet catching up), a stealthed player, a player walking backwards, a
//              player in walk mode, a murloc swimming (SMSG_SPLINE_MOVE_START_SWIM), a wolf rooted half way
//              (SMSG_SPLINE_MOVE_ROOT, then UNROOT), a gryphon on a flying spline (Fly), west to east;
//   crowd[:N]  N units (40 by default) walking and running between random points, a third of them losing and
//              regaining health every 2 s, a player changing a weapon every 10 s: the change mask's measure
//              (with VD_MOTION_FULL_LOOKS=1 every look is read every frame, as before it);
//   ship       units walking on the deck of the preview transport (VD_TRANSPORT_PREVIEW) with
//              SMSG_MONSTER_MOVE_TRANSPORT, and a player standing on board (MSG_MOVE_* with
//              MOVEFLAG_ONTRANSPORT).
// Every 5 s the log tells what Units::update cost. Game thread; nothing allocated per frame once spawned.
class MotionDemo {
public:
    // The ground under (x, y) between `top` and `bottom` (the collision's: terrain, buildings, decks).
    using HeightQuery = std::optional<f32> (*)(const void* context, f32 x, f32 y, f32 top, f32 bottom);

    void init(); // reads VD_MOTION_DEMO
    bool active() const { return m_scenario != Scenario::None; }
    // Once per frame before Units::update. `center`: where the character stands (spawned around it once the
    // ground there is known).
    void update(GameSession& session, const Units& units, const Transports& transports,
                const glm::vec3& center, u32 nowMs, HeightQuery height, const void* context);

private:
    enum class Scenario : u8 { None, Modes, Crowd, Ship };
    enum class Role : u8 {
        Walk,
        Run,
        SlowRun,
        Sprint,
        Stealth,
        Backwards,
        WalkMode,
        Swim,
        Rooted,
        Fly,
        Wander,
        Deck,
        Aboard
    };
    struct Member {
        u64 guid = 0;
        Role role = Role::Walk;
        bool player = false;
        glm::vec3 a{0.0f}, b{0.0f}; // its two ends (a transport's frame for the deck walkers)
        bool towardsB = true;
        u32 nextMs = 0; // when it starts its next leg
        u32 rootMs = 0; // Rooted: when the root comes, then when it goes
        bool rooted = false;
        u32 health = 100;
        u32 splineId = 0;
        // A player's leg (its own client's MSG_MOVE_*): where from and to, since when and how long, the last
        // heartbeat.
        bool moving = false;
        glm::vec3 from{0.0f}, to{0.0f};
        u32 legStartMs = 0, legMs = 0, beatMs = 0;
        u32 moveFlags = 0;
        f32 facing = 0.0f;
    };
    void playerMove(GameSession& session, const Member& m, u16 opcode, const glm::vec3& at, u32 flags,
                    u32 nowMs);

    bool spawn(GameSession& session, const Transports& transports, const glm::vec3& center, u32 nowMs,
               HeightQuery height, const void* context);
    void leg(GameSession& session, Member& m, u32 nowMs);
    void create(GameSession& session, const Member& m, u32 display, u32 race, u32 nowMs, u32 flags = 0,
                u32 visFlags = 0, f32 scale = 1.0f);
    void send(GameSession& session, u16 opcode, u32 nowMs);

    Scenario m_scenario = Scenario::None;
    u32 m_count = 40;
    bool m_spawned = false;
    u32 m_seed = 0x2545F491u;
    u32 m_healthAtMs = 0;
    u32 m_gearAtMs = 0;
    u32 m_gearToggle = 0;
    u32 m_logAtMs = 0;
    std::vector<Member> m_members;
    std::vector<u8> m_body; // the packet being built
};

} // namespace vd
