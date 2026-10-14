#pragma once

#include "core/Types.h"
#include "game/CombatAnimation.h"
#include "game/SpellKitFx.h"
#include "game/SpellVisualData.h"
#include "world/Effects.h"
#include "world/SpellBeams.h"

#include <glm/vec3.hpp>

#include <array>
#include <span>
#include <vector>

namespace vd {

class CameraShakes;

// What SpellVisuals drives and asks: the effect API of world/Effects, the actions of world/Actors, and where
// the units are. Implemented over Units and the entity store online (app/SpellEffects), over the crowd's
// actors in the offline group fight (app/CrowdFight), and by a recorder in the tests.
class SpellVisualWorld {
public:
    virtual ~SpellVisualWorld() = default;

    struct Unit {
        u32 actor = ~0u;      // its actor (~0u: none yet: nothing is shown on it)
        glm::dvec3 feet{0.0}; // world position
        f32 yaw = 0.0f;
        f32 height = 2.0f; // about how tall (missiles leave and land at its middle when it has no points)
        game::WeaponKind ranged = game::WeaponKind::None; // phase 14: its ranged weapon (the precast's load)
    };
    // A unit by its key: the server's GUID online, an actor id offline. False when unknown.
    virtual bool unit(u64 key, Unit& out) const = 0;

    virtual EffectHandle spawnAt(u64 model, const glm::dvec3& position, f32 yaw, f32 seconds) = 0;
    virtual EffectHandle spawnOn(u64 model, u32 actor, u32 attachment, f32 seconds) = 0;
    virtual void move(EffectHandle effect, const glm::dvec3& position, f32 yaw) = 0;
    virtual void stop(EffectHandle effect) = 0;
    virtual bool alive(EffectHandle effect) const = 0;
    virtual u32 liveEffects() const = 0;

    // Where an attachment point of an actor was drawn last frame; false when it was not drawn or lacks it.
    virtual bool attachmentPosition(u32 actor, u32 attachment, glm::dvec3& out) const = 0;
    // The actor's model has the attachment (true while its model is still loading).
    virtual bool hasAttachment(u32 actor, u32 attachment) const = 0;
    virtual void playAction(u32 actor, u16 animation, bool loop) = 0;
    virtual void stopAction(u32 actor) = 0;
    virtual u16 action(u32 actor) const = 0; // Actors::kNoAction when none
    // Loads an effect model ahead of its first use (Actors::preload); nothing by default.
    virtual void preload(u64 model) { (void)model; }
    // Phase 14 (spellfx): the color the kits give a unit's model (proc 1 tints, proc 13 flashes), multiplied
    // with its own; (1, 1, 1) = none. Nothing by default.
    virtual void setTint(u32 actor, const glm::vec3& rgb) {
        (void)actor;
        (void)rgb;
    }
};

// Phase 9: the spell visuals of SpellVisual.dbc / SpellVisualKit.dbc, scheduled like the original client
// shows them: the precast kit on the caster's hands (and its loop animation) while a cast lasts, stopped by
// the cast or an interruption; the cast kit when it goes off; a missile flying from the caster to each target
// it hits at the spell's speed (the same flight time as the audio's impact sound: the distance between the
// two units over Spell.dbc's speed), and the impact kit where it lands; the channel kit while channeling; the
// state kit while an aura is on a unit; over a ground spell's dynamic object, its area model falling again
// and again at random points of its radius (Blizzard's shards, Rain of Fire's rocks); the kits the server
// names (SMSG_PLAY_SPELL_VISUAL / _IMPACT). Kits play their animation on the unit (one-shots over what it
// does; loops held while their stage lasts).
//
// Budget: fixed tables (casts, missiles, auras, areas), nothing farther than the distances below from the
// camera (unless it involves the player), and past `maxLiveEffects` effect models alive only the player's
// own. Game thread; no allocation per frame once its tables are reserved.
class SpellVisuals {
public:
    struct Settings {
        f32 kitDistance = 70.0f;   // one-shot kits and missiles (the closer of caster and target)
        f32 holdDistance = 60.0f;  // precasts, channels and aura states: shown while within
        f32 areaDistance = 110.0f; // ground areas
        u32 maxLiveEffects = 256;  // effect models alive (world/Effects): above, only the player's
        u32 maxAreaCopies = 6;     // area models alive at once over one dynamic object
    };
    struct Stats {
        u32 holds = 0;    // precasts and channels in progress
        u32 missiles = 0; // in flight
        u32 auras = 0;    // aura states known (shown or not)
        u32 shownAuras = 0;
        u32 areas = 0;
        u32 effects = 0; // effect models alive (all of world/Effects)
        u32 kits = 0;    // kits played since init
        u32 skipped = 0; // effects not spawned (distance, budget) since init
        u32 beams = 0;   // phase 14: beams drawn this frame
        u32 tints = 0;   // phase 14: units colored by their kits this frame
        u32 shakes = 0;  // phase 14: camera shakes started since init
        f32 updateMs = 0.0f;
    };

    void init(const game::SpellVisualData* data, SpellVisualWorld* world);
    // Everything stops (a map change, leaving the world).
    void clear();
    Settings& settings() { return m_settings; }
    const Stats& stats() const { return m_stats; }
    // The player's key: what involves the player is always shown.
    void setPlayer(u64 key) { m_player = key; }
    // Phase 14 (spellfx): the kits' procs and camera shakes (game/SpellKitFx: beams, tints, flashes, precast
    // transitions) and where their shakes go (nullptr: none of them).
    void setKitFx(const game::SpellKitFx* fx, CameraShakes* shakes) {
        m_fx = fx;
        m_shakes = shakes;
    }
    // This frame's beams (channels and chain jumps), after update().
    std::span<const BeamInstance> beams() const { return m_beamFrame; }

    // --- What happened (Gameplay::fx() online, the fight offline).
    // A cast with a cast time started: the precast kit while it lasts (castMs, plus a grace for the cast to
    // come). Instants (castMs 0) show nothing here.
    void spellStart(u64 caster, u32 spell, u64 target, u32 castMs, u32 nowMs);
    // It went off: the precast stops, the cast kit plays. `destination` (ground spells) gets a pass of the
    // area model. Then spellHit() for each unit it hit.
    void spellGo(u64 caster, u32 spell, u64 target, const glm::dvec3* destination, u32 nowMs);
    // A unit it hit: a missile flies there first, else the impact kit plays at once.
    void spellHit(u64 caster, u32 spell, u64 target, u32 nowMs);
    void spellInterrupted(u64 caster, u32 nowMs);
    // The caster channels `spell` at `target` (0: none or a place), until channel(caster, 0, 0).
    void channel(u64 caster, u32 spell, u64 target, u32 nowMs);
    // Phase 14 (spellfx): the units a channel goes through (SMSG_SPELL_UPDATE_CHAIN_TARGETS, sent when it
    // starts: one call per unit, `index` of `count`, in the server's order). Its beams go caster -> first ->
    // second...
    void chainTarget(u64 caster, u32 spell, u64 target, u32 index, u32 count, u32 nowMs);
    // A kit by id on a unit (SMSG_PLAY_SPELL_VISUAL / _IMPACT).
    void playKit(u64 unit, u32 kitId, u32 nowMs);
    // Auras: their state kits while they last.
    void auraAdded(u64 unit, u32 spell);
    void auraRemoved(u64 unit, u32 spell);
    // A ground spell's dynamic object (DYNAMICOBJECT_SPELLID, its center and radius): call every frame it
    // exists; areaGone() (or no call for a while) ends it.
    void area(u64 object, u32 spell, const glm::dvec3& center, f32 radius, u32 nowMs);
    void areaGone(u64 object);
    // The unit left: what was on it stops.
    void unitGone(u64 unit);
    // Loads a spell's models (its kits, missile and area) before it is first cast.
    void prewarm(u32 spell);

    // Missiles fly and land, holds expire or come into view, loop animations are kept on their units. Once
    // per frame after the units were placed, before Effects::updateSpells.
    void update(u32 nowMs, f32 dt, const glm::dvec3& camera);

    // Development (VD_TRACE_FX=1): every visual played or skipped, in the log.
    bool trace = false;
    // Phase 14 development switches (VD_BEAMS=0, VD_KIT_FLASH=0, VD_CAMERA_SHAKE=0 for the kits' shakes).
    bool beamsEnabled = true;
    bool tintsEnabled = true;
    bool shakesEnabled = true;

    static constexpr u32 kMaxHolds = 64;
    static constexpr u32 kMaxMissiles = 96;
    static constexpr u32 kMaxAuras = 256;
    static constexpr u32 kMaxAreas = 24;
    static constexpr u32 kHoldModels =
        4; // effect models of one precast, channel or state kit (hands, chest...)
    static constexpr u32 kAreaCopies = 8;
    static constexpr u32 kMaxBeams = 64;       // phase 14: beams alive (each up to kMaxBeamQuads quads)
    static constexpr u32 kMaxChainTargets = 8; // units of one chain
    static constexpr u32 kMaxChains = 16;      // chains being told or waiting for their hits
    static constexpr u32 kMaxTints = 64;       // units colored by kits at once

private:
    // A kit held on a unit while its stage lasts: a precast, a channel or an aura's state.
    enum class HoldKind : u8 { Precast, Channel };
    struct Hold {
        u64 unit = 0;
        u64 target = 0;
        u32 spell = 0;
        u32 kit = 0;
        u32 endMs = 0; // precasts: given up after this (no cast came); channels: 0 (until told)
        u16 loop = game::kNoKitAnimation;
        HoldKind kind = HoldKind::Precast;
        bool active = false;
        bool shown = false; // its models are out
        std::array<EffectHandle, kHoldModels> models{};
    };
    struct Aura {
        u64 unit = 0;
        u32 spell = 0;
        u32 kit = 0;
        u16 loop = game::kNoKitAnimation;
        bool active = false;
        bool shown = false;
        std::array<EffectHandle, kHoldModels> models{};
    };
    struct Missile {
        u64 caster = 0;
        u64 target = 0;
        u32 spell = 0;
        u32 impactKit = 0;
        u32 startMs = 0;
        u32 arriveMs = 0;
        u32 targetAttachment = 34;
        glm::dvec3 from{0.0};
        glm::dvec3 last{0.0}; // where it was drawn last (the target's point if the target goes away)
        bool arched = false;
        bool active = false;
        EffectHandle model;
    };
    struct Area {
        u64 object = 0;
        u32 spell = 0;
        u32 seenMs = 0;
        u32 nextMs = 0;  // the next model falls then
        u32 spawned = 0; // models so far (the next one's slot)
        u32 random = 1;
        glm::dvec3 center{0.0};
        f32 radius = 0.0f;
        bool active = false;
        std::array<EffectHandle, kAreaCopies> models{};
    };

    // Phase 14: a beam from one unit to another (`fromCaster`: the first jump, from the casting hand), held
    // while its owner hold lasts (`endMs` 0) or shown from startMs to endMs.
    struct Beam {
        const game::ChainEffect* effect = nullptr;
        u64 from = 0;
        u64 to = 0;
        u32 spell = 0; // its visual names where it lands on the target
        u32 startMs = 0;
        u32 endMs = 0;
        u32 owner = ~0u; // the hold it belongs to (~0u: none)
        u32 seed = 1;
        bool fromCaster = false;
        bool active = false;
    };
    // Phase 14: the units of a chain: a channel's (SMSG_SPELL_UPDATE_CHAIN_TARGETS) or the ones a cast hit,
    // gathered from spellHit() until the next update().
    struct Chain {
        u64 caster = 0;
        u32 spell = 0;
        u32 kit = 0; // a cast's kit with the chain (0: a channel's list)
        u32 ms = 0;
        u32 count = 0;
        std::array<u64, kMaxChainTargets> targets{};
        bool complete = false;
        bool active = false;
    };
    // Phase 14: the color the kits give a unit: the tints of its holds and a flash.
    struct Tint {
        u64 unit = 0;
        glm::vec3 hold{1.0f};
        glm::vec3 flash{1.0f};
        u32 flashStartMs = 0;
        u32 flashHoldMs = 0;
        u32 flashFadeMs = 0;
        bool flashing = false;
        bool active = false;
    };

    // Spawns a kit's models on a unit: one-shot (Effects::kOnce) or held (0, until stopped; written to
    // `held`). Returns the models spawned.
    u32 spawnKit(const game::VisualKit& kit, const SpellVisualWorld::Unit& unit, bool once,
                 std::array<EffectHandle, kHoldModels>* held);
    // A one-shot kit's models on a unit, and `animation` (kNoKitAnimation: none), if near enough and within
    // budget.
    void playKitOnce(u32 kitId, u64 unitKey, bool important, u32 nowMs, u16 animation);
    void stopModels(std::array<EffectHandle, kHoldModels>& models);
    void endHold(Hold& hold);
    void endAura(Aura& aura);
    void endArea(Area& area);
    bool near(const glm::dvec3& at, f32 distance) const;
    bool budget(bool important) const;
    bool involvesPlayer(u64 a, u64 b) const { return m_player != 0 && (a == m_player || b == m_player); }
    // Where a missile leaves the caster and lands on the target.
    glm::dvec3 launchPoint(const SpellVisualWorld::Unit& caster) const;
    glm::dvec3 landingPoint(const SpellVisualWorld::Unit& target, u32 attachment) const;
    void land(Missile& missile, u32 nowMs);
    // The loops the unit's holds and states want, kept on its actor while its action slot is free.
    void keepLoop(const SpellVisualWorld::Unit& unit, u16 loop);
    void releaseLoop(u64 unitKey, u16 loop);
    void log(const char* what, u64 unit, u32 id) const;
    // Phase 14 (SpellVisualsFx.cpp): what a kit's procs and shake do when it plays on a unit (flash, shake),
    // the beams of a hold or of a cast's chain, the tints, and this frame's beam list.
    void kitPlayed(const game::VisualKit* kit, u64 unitKey, const SpellVisualWorld::Unit& unit, u32 nowMs);
    void startHoldBeams(u32 holdIndex, u32 nowMs);
    void stopHoldBeams(u32 holdIndex);
    void startBeams(const game::KitFx& fx, u64 caster, u32 spell, std::span<const u64> targets, u32 owner,
                    u32 nowMs);
    Chain* findChain(u64 caster, u32 spell, bool cast);
    void finishCastChains(u32 nowMs);
    void updateBeams(u32 nowMs);
    void updateTints(u32 nowMs);
    Tint* tintOf(u64 unit);
    glm::dvec3 beamStart(const SpellVisualWorld::Unit& caster) const;
    glm::dvec3 beamEnd(const SpellVisualWorld::Unit& target, u32 spell) const;
    // The precast's animation and the loop after it (LoadBow -> HoldBow; the kit's, else a ranged spell's
    // load by the caster's weapon).
    void precastAnimations(const game::VisualKit* kit, u32 spell, const SpellVisualWorld::Unit* caster,
                           bool unitTarget, u16& once, u16& loop) const;

    const game::SpellVisualData* m_data = nullptr;
    SpellVisualWorld* m_world = nullptr;
    Settings m_settings;
    Stats m_stats;
    u64 m_player = 0;
    glm::dvec3 m_camera{0.0};
    std::vector<Hold> m_holds;
    std::vector<Missile> m_missiles;
    std::vector<Aura> m_auras;
    std::vector<Area> m_areas;
    // Phase 14 (spellfx).
    const game::SpellKitFx* m_fx = nullptr;
    CameraShakes* m_shakes = nullptr;
    std::vector<Beam> m_beams;
    std::vector<Chain> m_chains;
    std::vector<Tint> m_tints;
    std::vector<BeamInstance> m_beamFrame;
    u32 m_frame = 0;
    u32 m_lastMs = 0; // the time of the last update (events without one: aura states)
};

// The attachment point a spell visual's missile and beams land on (SpellVisual.dbc field 9, the table of
// WoW.exe at 0x860A18): 0 head, 1 chest, 2 base, 3 left hand, 4 right hand, 5 breath, 6-7 special.
u32 missileAttachment(u32 index);

} // namespace vd
