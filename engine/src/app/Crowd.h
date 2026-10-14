#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vector>

namespace vd {

class Actors;
class Terrain;

// Synthetic actors for tests and benchmarks: characters of every race with random customization and gear (what
// NPCs wear), some walking or running in circles, some idling, dancing or sitting, around a point. Also places
// single creatures for inspection. Game thread only.
class Crowd {
public:
    // `count` characters around `center` (world, z = ground level when the terrain is not loaded yet).
    // A single character stands at `center` facing `yaw`. `floor`: walkable height they never go below (a city's
    // WMO floor over the terrain; collision arrives with phase 6).
    void spawnCharacters(Actors& actors, const glm::dvec3& center, u32 count, u32 seed, f32 yaw = 0.0f,
                         f32 floor = -1.0e6f);
    // One creature display, idle, facing `yaw`.
    void spawnCreature(Actors& actors, const glm::dvec3& position, f32 yaw, u32 displayId);
    // A random character on a mount display, standing, facing `yaw`.
    void spawnRider(Actors& actors, const glm::dvec3& position, f32 yaw, u32 mountDisplay, u32 seed);
    // Phase 13 (development): a random character (of `race`, 0: any) with a ranged weapon drawn (sheath state 2) in its
    // ready stance, facing `yaw`: `kind` 0 a bow, 1 a gun, 2 a crossbow, 3 a wand, 4 a thrown weapon. With `sheathed`,
    // the same character with it put away (and a melee weapon), as everyone stands outside a fight.
    void spawnArcher(Actors& actors, const glm::dvec3& position, f32 yaw, u32 kind, u32 seed, u8 race = 0,
                     bool sheathed = false);
    void clear(Actors& actors);
    // Moves the walkers and keeps everyone on the ground.
    void update(Actors& actors, f32 dt, const Terrain* terrain);
    // Every member plays `animation` (0xFFFF: back to their own behavior).
    void setAnimation(Actors& actors, u16 animation);
    // Phase 11 (development): every member drawn with this fade (Actors::setFade: a crowd of ghosts).
    void setFade(Actors& actors, const glm::vec4& fade);
    // The actor of the member spawned last (~0u: none).
    u32 lastActor() const { return m_members.empty() ? ~0u : m_members.back().actor; }

    u32 size() const { return static_cast<u32>(m_members.size()); }

    // A new actor with a random character look (race, customization, NPC gear) from `random`'s state.
    static u32 randomLook(Actors& actors, u32& state);
    // Phase 9: the random character itself, without weapons (the group fight arms its fighters by role).
    static game::CharacterLook randomCharacter(const game::AppearanceDb& db, u32& state);

private:
    enum class Behavior : u8 { Stand, Walk, Run, Dance, Sit };
    struct Member {
        u32 actor;
        glm::dvec3 center;
        f64 radius;
        f64 angle;
        f32 speed; // yards per second along the circle (negative: the other way)
        Behavior behavior;
        f32 z;
        f32 yaw;
        f32 floor;
        u32 mount; // Actors id of its mount, or ~0u on foot
    };
    std::vector<Member> m_members;
    u16 m_override = 0xFFFF;
};

} // namespace vd
