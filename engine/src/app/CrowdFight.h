#pragma once

#include "core/Types.h"
#include "game/CombatAnimation.h"
#include "world/EffectsVisualWorld.h"
#include "world/SpellVisuals.h"

#include <glm/vec3.hpp>

#include <array>
#include <vector>

namespace vd {

class Actors;
class Effects;
class Terrain;
class CameraShakes;
class UnitBodyEffects;
namespace game {
class SpellKitFx;
class SpellVisualData;
class UnitFades;
}

// Phase 9: an offline group fight for benchmarks and screenshots (route "fight"; --spawn fight:<n>). Two
// teams of characters face each other: melee in front swinging by weapon (shield, two-handers, dual wield,
// staves; some run around their foe while they swing), casters behind with their precasts, bolts and debuffs,
// healers with heals, shields and renews, hunters shooting, channelers raining Blizzard and Rain of Fire on
// the other side or draining a foe; a fighter falls now and then and gets up again; some weapons glow with an
// enchantment (Flametongue, Frostbrand, Windfury, Rockbiter, poisons). It drives the same code as the
// server's units: the combat animations of game/CombatAnimation as world/Actors actions, the spells through
// SpellVisuals with this as its world (fighters by index + 1). Deterministic for a seed. Game thread.
class CrowdFight final : public EffectsVisualWorld {
public:
    void start(Actors& actors, Effects& effects, const game::SpellVisualData& data, const glm::dvec3& center,
               u32 fighters, u32 seed, f32 floor = -1.0e6f);
    // Development (--spawn cast:<spell>): a caster `across` yards to one side casts the spell again and again
    // at a fighter on the other side (its precast, cast, missile, impact, channel, area and state), to look
    // at it.
    void startDuel(Actors& actors, Effects& effects, const game::SpellVisualData& data,
                   const glm::dvec3& center, const glm::dvec3& across, u32 spell);
    void clear();
    // Phase 14 (spellfx): the kits' procs and shakes for its spells (beams, tints, flashes, shakes) and the
    // auras' colors and translucency (game/UnitFade), kept for the next start(); nullptr: none.
    void setKitFx(const game::SpellKitFx* fx, CameraShakes* shakes, const game::UnitFades* fades) {
        m_kitFx = fx;
        m_shakes = shakes;
        m_fades = fades;
    }
    bool active() const { return !m_fighters.empty(); }
    u32 size() const { return static_cast<u32>(m_fighters.size()); }
    // Fights on: decisions, animations and spells, everyone kept on the ground. After the units' placement
    // and before Effects::updateSpells, like SpellEffects.
    void update(u32 nowMs, f32 dt, const glm::dvec3& camera, const Terrain* terrain);
    SpellVisuals& visuals() { return m_visuals; }
    // Phase 14 (bodyfx): the melee blows bleed through this (nullptr: no blood).
    void setBodyEffects(UnitBodyEffects* body) { m_bodyFx = body; }

    // SpellVisualWorld: fighters by index + 1.
    bool unit(u64 key, Unit& out) const override;

    struct Stats {
        u32 swings = 0;
        u32 casts = 0;
        u32 deaths = 0;
        f32 updateMs = 0.0f; // the fight's own decisions and animations (not SpellVisuals)
    };
    const Stats& stats() const { return m_stats; }

private:
    enum class Role : u8 { Shield, TwoHand, DualWield, Staff, Caster, Healer, Hunter, Channeler, Count };
    struct Aura {
        u32 spell = 0;
        u32 untilMs = 0;
    };
    struct Fighter {
        u32 actor = ~0u;
        Role role = Role::Shield;
        u8 team = 0;
        glm::dvec3 home{0.0};
        glm::dvec3 feet{0.0};
        f32 yaw = 0.0f;
        u32 foe = 0; // fighter index it faces (its melee opponent, or whom it casts at)
        game::UnitGear gear;
        u32 nextMs = 0;
        u32 cast = 0;       // the spell being cast or channeled (0: none)
        u32 castTarget = 0; // fighter index + 1 (0: the ground or itself)
        u32 castEndMs = 0;
        bool channel = false;
        bool interrupted = false; // this cast will be interrupted
        glm::dvec3 ground{0.0};   // a ground channel's destination
        u32 swings = 0;
        bool dead = false;
        bool runner = false; // circles its foe while it fights
        f64 angle = 0.0;
        u32 riseMs = 0;
        std::array<Aura, 3> auras{};
        std::array<EffectHandle, game::SpellVisualData::kItemVisualSlots> enchant{}; // its weapon's glow
    };

    void act(u32 index, u32 nowMs);
    void duel(u32 nowMs);
    void melee(u32 index, u32 nowMs);
    void cast(u32 index, u32 spell, u32 target, u32 castMs, u32 nowMs);
    void finishCast(u32 index, u32 nowMs);
    void hit(u32 caster, u32 spell, u32 target, u32 nowMs);
    // Phase 14: a chain (Chain Lightning, Chain Heal) hits `first`, then two more of the same side.
    void chainHit(u32 caster, u32 spell, u32 first, u32 nowMs);
    void addAura(u32 index, u32 spell, u32 seconds, u32 nowMs);
    void die(u32 index, u32 nowMs);
    u32 enemy(u32 index); // a living fighter of the other team (its foe first)
    u32 ally(u32 index);
    u32 random();
    u32 below(u32 n) { return n ? random() % n : 0; }
    static u64 key(u32 index) { return static_cast<u64>(index) + 1; }
    static u64 areaKey(u32 index) { return (0xA4EAull << 32) | index; }

    const game::SpellVisualData* m_data = nullptr;
    const game::SpellKitFx* m_kitFx = nullptr; // phase 14
    CameraShakes* m_shakes = nullptr;
    const game::UnitFades* m_fades = nullptr;
    SpellVisuals m_visuals;
    std::vector<Fighter> m_fighters;
    glm::dvec3 m_center{0.0};
    f32 m_floor = -1.0e6f;
    u32 m_random = 1;
    u32 m_nextDeathMs = 0;
    u32 m_nowMs = 0;
    u32 m_duelSpell = 0;
    bool m_started = false;
    UnitBodyEffects* m_bodyFx = nullptr; // phase 14 (bodyfx)
    Stats m_stats;
};

} // namespace vd
