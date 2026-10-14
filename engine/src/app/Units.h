#pragma once

#include "core/Types.h"
#include "game/CombatAnimation.h"
#include "game/UnitFade.h"
#include "game/UnitMotion.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace vd {

namespace pack {
class PackReader;
}
namespace game {
class Gameplay;
class SpellVisualData;
class UnitFades;
struct Entity;
} // namespace game
class Actors;
class Effects;
class GameSession;
class UnitBodyEffects;

// The server's creatures and players as animated actors (phase 5): their look comes from the update fields
// (display ids, race and customization, equipped items asked to the server), their animation from their movement
// and state (walking, running, sitting, dead, NPC emote loops), mounts carry their riders, and name plates float
// over the closest ones. Phase 9 (app/UnitCombat.cpp): they fight like the original — the ready stance in combat,
// swings by weapon and hand, ranged shots, wounds, dodges, parries and blocks from the combat log, deaths (and
// corpses found lying), one-shot emotes — as actions over their movement (world/Actors). Spell animations are
// SpellVisuals'. Game thread only.
class Units {
public:
    // Emotes.dbc (NPC emote states -> animations).
    void init(const pack::PackReader& db);
    // Ground under a point (x, y, z): the highest surface within a few yards below, if known.
    using GroundQuery = std::optional<f32> (*)(const void* context, f32 x, f32 y, f32 z);

    // Follows the entity store: actors for new units, looks refreshed when their fields change, placement and
    // animation every frame, actors released for units that left. `ground` keeps airborne players over the floor.
    void update(GameSession& session, Actors& actors, u32 nowMs, f32 dt, GroundQuery ground = nullptr,
                const void* groundContext = nullptr);
    void clear(Actors& actors);
    // Phase 9: the emote and spell tables (emote animations; spells whose impact animates the victim themselves).
    void setVisualData(const game::SpellVisualData* data) { m_visualData = data; }
    // Phase 11: the auras that make a unit translucent or tint it (ghosts, stealth, invisibility); nullptr: none.
    void setFades(const game::UnitFades* fades) { m_fades = fades; }
    // Phase 14 (bodyfx): the blood of melee hits and the poof of mounting (nullptr: none).
    void setBodyEffects(UnitBodyEffects* body, Effects* effects) {
        m_bodyFx = body;
        m_effects = effects;
    }
    // Phase 9: a unit's actor (~0u when it has none) and where it stands this frame (false: not a unit shown).
    u32 actorOf(u64 guid) const;
    bool placement(u64 guid, glm::dvec3& feet, f32& yaw) const;
    // The selection circle's half width under a unit (yards; 0 while its model loads): its model's, or its mount's
    // while it rides (WoW.exe sizes it by the model the unit stands on), times its OBJECT_FIELD_SCALE_X.
    f32 selectionHalfSize(u64 guid, const Actors& actors) const;
    // Phase 14 (spellfx): what a unit holds (nullptr: not a unit shown); its ranged weapon: a precast's load.
    const game::UnitGear* gear(u64 guid) const;
    // The units that left during the last update() (what was on them stops).
    std::span<const u64> gone() const { return m_gone; }
    struct CombatStats {
        u32 swings = 0;    // actions played since init: melee and ranged attacks
        u32 reactions = 0; // wounds, dodges, parries, blocks
        u32 emotes = 0;
        f32 readMs = 0.0f; // the last update's reading of the combat log and fx rings
    };
    const CombatStats& combatStats() const { return m_combatStats; }
    // Phase 8: passengers ride their transport. Phase 18 (motion): the entity store places them itself
    // (game::setTransportResolver), walking on board too (SMSG_MONSTER_MOVE_TRANSPORT).
    // Phase 18 (motion): the liquid's surface over a point (x, y, z): its level when water, lava or slime is
    // above it (or just below). The server's creatures swim by the client's own rule (game::unitSwims).
    using LiquidQuery = std::optional<f32> (*)(const void* context, f32 x, f32 y, f32 z);
    void setLiquidQuery(LiquidQuery query, const void* context) {
        m_liquidQuery = query;
        m_liquidContext = context;
    }
    // Phase 18 (motion): the cost of update() over the last window of frames, and how many looks it read (their
    // fields changed, or an answer they waited for may have come) and rebuilt: the change mask's measure. With
    // VD_MOTION_FULL_LOOKS=1 (development) every look and fade is read every frame, as before the mask.
    struct UpdateCost {
        f32 meanMs = 0.0f;
        f32 steadyMs = 0.0f; // the mean of the frames under 2 ms (a busy PC's preemptions aside)
        f32 worstMs = 0.0f;
        u32 frames = 0;
        u32 units = 0;
        f32 looksRead = 0.0f;    // per frame
        f32 looksRebuilt = 0.0f; // per frame
    };
    const UpdateCost& updateCost() const { return m_cost; }

    // Names and health over the heads of the visible units within `distance` yards (ImGui foreground list: the
    // development overlay; the game UI comes with phase 7). `viewProj` is camera-relative.
    void drawNamePlates(GameSession& session, const Actors& actors, const glm::mat4& viewProj,
                        const glm::dvec3& camera, f32 width, f32 height, f32 distance = 40.0f) const;
    // A unit drawn last frame: where its feet and the top of its head (its name) are, and about how wide it is.
    struct UnitView {
        u64 guid = 0;
        glm::dvec3 feet{0.0};
        glm::dvec3 head{0.0};
        f32 radius = 0.5f;
    };
    // Units whose model was drawn last frame (tapping them, name plates).
    void collectViews(const Actors& actors, std::vector<UnitView>& out) const;
    // True when the unit is drawn as a model (the debug boxes skip it).
    bool hasModel(u64 guid) const;
    static bool hasModelCallback(const void* units, u64 guid) { return static_cast<const Units*>(units)->hasModel(guid); }

    u32 count() const { return static_cast<u32>(m_units.size()); }
    u32 pendingLooks() const { return m_pendingLooks; }

private:
    struct Unit {
        u32 actor = ~0u;
        u32 mount = ~0u;
        u64 lookKey = 0;
        u32 mountDisplay = 0;
        f32 yaw = 0.0f;
        glm::dvec3 feet{0.0}; // last placement
        f32 scale = 1.0f;
        u32 seen = 0; // frame
        bool complete = false; // every item it wears was known when its look was built
        bool turned = false;   // yaw initialized
        bool drawn = false;    // its model is loaded (else the debug box stands in)
        // Phase 9: combat.
        game::UnitGear gear;      // what it holds (with the look)
        bool aliveSeen = false;   // seen alive: dying plays Death; dead from the start is a corpse lying there
        bool attacking = false;   // auto attack on (SMSG_ATTACKSTART .. ATTACKSTOP)
        u32 combatUntilMs = 0;    // the ready stance lasts a while after its last swing or hit
        u32 rangedUntilMs = 0;    // shooting: the ranged ready stance
        u32 swingUntilMs = 0;     // its own swing is playing: a wound does not cut it
        u32 emoteMs = 0;          // last SMSG_EMOTE (its text emote then plays nothing more)
        glm::vec4 fade{1.0f};     // phase 11: as drawn (ActorDraw::fade), moving towards its auras' look
        // Phase 18 (motion).
        bool lookWaiting = false; // its look waits for something but its fields (a guild's emblem)
        bool swimming = false;    // the client's own swim decision (creatures)
        // Ours (seated animations): its stand state (UNIT_FIELD_BYTES_1) and whether it channels, last frame's.
        u8 standState = 0;
        bool channeling = false;
        u32 liquidAtMs = 0;       // when the liquid under it was last looked at
        f32 depth = 0.0f;         // of the liquid over its feet then (0: none)
        glm::vec3 liquidFrom{0.0f};
        u16 motionShown = 0xFFFF; // the movement animation last traced (VD_TRACE_MOTION)
        game::UnitFade fadeTarget;  // its auras' look (game::UnitFades::ofUnit) when they last changed
        // Where its weapons are (0 put away, 1 melee, 2 ranged; the player's is game::Gameplay::sheath()): the
        // server's field as last seen, the state shown, the animation that last decided it, the one its look has.
        u8 sheathField = 0xFF;
        u8 sheath = 0;
        u16 sheathAnimation = 0xFFFF;
        u8 lookSheath = 0xFF;
    };

    // Phase 9 (app/UnitCombat.cpp): the combat log and fx rings since the last frame, into actions.
    void readCombat(const game::Gameplay& play, Actors& actors, u64 player, u32 nowMs);
    // Ours (seated animations): whether a one-shot emote plays on it (WoW.exe's filters).
    static bool emoteShown(const Actors& actors, const Unit& unit, u16 animation);
    void playAction(Actors& actors, Unit& unit, u16 animation);
    game::UnitGear gearOf(GameSession& session, const game::Entity& e, u8 sheath) const;

    std::unordered_map<u64, Unit> m_units;
    std::vector<u64> m_gone; // scratch
    std::vector<u16> m_emoteAnimation; // Emotes.dbc id -> AnimationData id (0 = none)
    u32 m_frame = 0;
    u32 m_pendingLooks = 0;
    u32 m_meetingStoneJoins = 0; // phase 16 (alerts): game::MeetingStone::joins() last seen
    // Phase 18 (motion).
    game::UnitShapes m_shapes;
    LiquidQuery m_liquidQuery = nullptr;
    const void* m_liquidContext = nullptr;
    u32 m_storeFrame = 0; // EntityStore::frame() at the last update
    bool m_fullLooks = false;
    bool m_traceMotion = false; // development: VD_TRACE_MOTION=1 logs each unit's movement animation as it changes
    bool m_logCost = false;     // development: VD_MOTION_COST=1 logs updateCost() every window
    UpdateCost m_cost;
    struct CostWindow {
        f64 totalMs = 0.0;
        f64 steadyMs = 0.0;
        u32 steadyFrames = 0;
        f32 worstMs = 0.0f;
        u32 frames = 0;
        u32 units = 0;
        u32 looksRead = 0;
        u32 looksRebuilt = 0;
    } m_window;
    // Phase 9.
    const game::SpellVisualData* m_visualData = nullptr;
    const game::UnitFades* m_fades = nullptr; // phase 11
    UnitBodyEffects* m_bodyFx = nullptr;      // phase 14 (bodyfx)
    Effects* m_effects = nullptr;
    u64 m_cursorsPlayer = 0; // the player the ring cursors follow (reset when it changes)
    u32 m_fxCursor = 0;
    u32 m_combatCursor = 0;
    CombatStats m_combatStats;
};

} // namespace vd
