#pragma once

#include "audio/GroundSounds.h"
#include "core/Types.h"
#include "game/BodyData.h"
#include "pack/PackReader.h"
#include "physics/Collision.h"
#include "render/Renderer.h"
#include "world/Actors.h"
#include "world/BodyDecals.h"
#include "world/Effects.h"

#include <glm/vec3.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vd {

class Effects;
class Terrain;
namespace physics {
class CollisionWorld;
}
namespace game {
class AreaData;
}

// Phase 14 (bodyfx): what the 1.12 client draws of its units' bodies besides their models (WoW.exe rules in
// the comments, game/BodyData for its tables):
//  - a blob shadow under each unit drawn (world/BodyDecals);
//  - at each foot that comes down ($FL0/$FR0 walking, $RL0/$RR0 running and the other legs' 1-3 of the M2
//  events;
//    0x5FFBD0 -> 0x5FBF70): within 50 yd a footprint where the ground keeps them (snow, sand), within 25 yd
//    the ground's spray (snow) or, wading (water under half its height), the water's;
//  - ripples where it stands or swims in water up to twice its height, a ring when it goes in or out
//  (0x5FA760);
//  - at its $BTH events, condensing breath in cold zones and bubbles deep under water (0x5FFFE7, 0x607710);
//  - the blood spurt of a melee hit (0x625010) and the poof of mounting (0x5FFA50).
// The client's own unit effects that 1.12.1 never shows are left out: the ground blood of UnitBlood.dbc
// (0x60A260 only rolls its timer), the hit sprites and MorphFX (only spell kits name them; app/SpellEffects
// plays those). Game thread only; nothing allocates per frame once warmed up.
class UnitBodyEffects {
public:
    // A model event of a body this frame (the M2 events its animation crossed, Actors::events()): its id
    // (pack::actorEventId) and where it happened.
    struct Event {
        u32 actor = Actors::kNone;
        u32 id = 0;
        glm::dvec3 position{0.0};
    };

    // `db`: the client tables (db.vdpk); bodyfx.vdpk and sound.vdpk (its ground tables) are opened from
    // `packsDir`. Without bodyfx.vdpk there are no blob shadows; without sound.vdpk no footprints nor sprays
    // on the ground.
    bool init(const pack::PackReader& db, const std::filesystem::path& packsDir);
    void shutdown();
    // A new map (its Map.dbc directory; empty: none): forgets every mark.
    void setMap(std::string_view map);
    const game::BodyData& data() const { return m_data; }

    // Phase 10 quality: shadows reach `distance` times their base distance (the environment detail),
    // footprints and ripples keep `marks` times their pools (the spell detail); the dynamic quality lowers
    // both under heat.
    void setQuality(f32 distance, f32 marks);

    struct Frame {
        const Actors* actors = nullptr;
        Effects* effects = nullptr;
        const Terrain* terrain = nullptr;
        const physics::CollisionWorld* collision = nullptr;
        const game::AreaData* areas = nullptr;
        glm::dvec3 camera{0.0};
        u32 playerActor = Actors::kNone; // the player's own body (its own footprint and ripple rings)
        f32 dt = 0.0f;
    };
    // Once per frame after Actors::finishCollect: the bodies drawn, their events, the marks' ages, the
    // effects to spawn (they show from the next frame).
    void update(const Frame& frame);

    // A melee hit landed on `victim` (an actor standing at `feet` facing `yaw`): the blood spurt of its model
    // on its chest, in front or from behind as `attacker` stands (nullptr: unknown, in front); `large` for
    // HITINFO_BLOOD_SPURT.
    void bleed(Effects& effects, const Actors& actors, u32 victim, const glm::dvec3& feet, f32 yaw,
               const glm::dvec3* attacker, bool large);
    // A unit just mounted: the poof at its feet.
    void mounted(Effects& effects, const glm::dvec3& feet, f32 yaw, f32 scale);
    // Phase 16 (alerts): the player joined a meeting stone's queue: "HARDCODED Meeting Stone Join" at its feet.
    void meetingStoneJoined(Effects& effects, const glm::dvec3& feet, f32 yaw, f32 scale);
    // Development (--spawn bleed:<display>): `actor`, standing at `feet` facing `yaw`, bleeds every 0.6 s,
    // hit in front and from behind, small and large in turn.
    void demoBleed(u32 actor, const glm::dvec3& feet, f32 yaw);
    // Development (--spawn wake): an unseen swimmer going back and forth over 12 yards of water from `start`
    // along `yaw` at 4.7 yd/s (the swim speed), its wake and, where it turns, rings.
    void demoWake(const glm::dvec3& start, f32 yaw) { m_demoWake = {start, yaw, 0.0f, 0.0f, true}; }
    // Development (--spawn poof): the mounting poof at `feet`, every 2 s.
    void demoPoof(const glm::dvec3& feet, f32 yaw) { m_demoPoof = {feet, yaw, 0.0f, 0.0f, true}; }

    // The frame's quads after the first `quads` of the renderer's particle buffer: the shadows and footprints
    // into `decals` (drawn right after the opaque world), the ripples into `particles` (with the other
    // particles). Returns the quads in all. `texture` turns a texture path hash into a handle (invalid while
    // it loads: that kind waits).
    using TextureQuery = TextureHandle (*)(const void* context, u64 pathHash);
    u32 writeQuads(u32 quads, const glm::dvec3& camera, std::span<ParticleVertex> vertices,
                   TextureQuery texture, const void* textureContext, std::vector<ParticleDraw>& decals,
                   std::vector<ParticleDraw>& particles);

    // Development switches (VD_BLOB_SHADOWS=0, VD_BLOOD=0, VD_FOOTSTEP_FX=0, VD_SWIM_FX=0,
    // VD_BREATH=0|cold|water, VD_VIOLENCE=0..2, VD_FAKE_BODY_EVENTS=1: steps and breaths made up from the
    // animations instead of the models' own events).
    bool shadows = true;
    bool blood = true;
    bool footsteps = true;
    bool swimming = true;
    bool breath = true;
    bool fakeEvents = false;
    bool trace = false; // VD_TRACE_BODYFX: what it did, every 2 s, to the log
    u8 forceBreath = 0; // VD_BREATH=cold|water: every body breathes so (1 cold, 2 bubbles)
    u32 violence = game::kDefaultViolence;

    struct Stats {
        u32 bodies = 0;
        u32 shadows = 0;
        u32 floorQueries = 0; // this frame
        u32 footprints = 0;
        u32 ripples = 0;
        u32 spawned = 0; // effects since init: sprays, breaths, blood, poofs
        f32 updateMs = 0.0f;
    };
    const Stats& stats() const { return m_stats; }

    // Base distances (yards; the quality scales them).
    static constexpr f32 kShadowDistance = 60.0f;
    static constexpr f32 kFootprintDistance = 50.0f; // 0x5FC00A: 2500 = 50 squared
    static constexpr f32 kSprayDistance = 25.0f;     // 0x5FC0B6: 625
    static constexpr f32 kRippleDistance = 60.0f;
    static constexpr u32 kMaxShadows = 128;
    // Sprays, breaths, blood and poofs alive at once (each is an effect actor of Effects); the spell detail
    // lowers it.
    static constexpr u32 kMaxEffects = 32;
    static constexpr u32 kFloorQueriesPerFrame = 12;

private:
    // What is kept per actor id between frames.
    struct BodyState {
        u64 model = 0;
        u32 seenFrame = 0;
        glm::dvec3 feet{0.0};
        f32 speed = 0.0f; // yards per second, smoothed
        // The floor under it (the shadow's plane): where it was found, its height and normal, on the terrain
        // or not.
        glm::dvec3 floorAt{1e30};
        f64 floorZ = 0.0;
        glm::vec3 floorNormal{0.0f, 0.0f, 1.0f};
        u32 floorFrame = 0;
        bool floorFound = false;
        bool onTerrain = false;
        // Water.
        f32 rippleTimer = 0.0f;
        bool inWater = false;    // deeper than 0.4 x its height last frame (0x603145)
        bool waterKnown = false; // inWater was measured (the first frame in view makes no splash)
        // Breath, decided every 10 s (0x607710).
        f32 breathTimer = 0.0f;
        u8 breath = 0; // 0 none, 1 cold, 2 under water
        // Development: the made-up steps and breaths.
        f32 stepTimer = 0.0f;
        f32 breathEventTimer = 0.0f;
        bool leftFoot = false;
    };
    struct Body {
        Actors::BodyView view;
        const game::BodyModel* model = nullptr;
        BodyState* state = nullptr;
        f32 distance = 0.0f; // to the camera
        f32 height = 2.0f;   // collision height x scale
    };

    BodyState& stateOf(u32 actor);
    // The floor under a body (cached, extrapolated along its plane, re-asked within the frame's budget).
    void findFloor(Body& body, const Frame& frame);
    void readEvents(const Frame& frame);
    void makeUpEvents(const Frame& frame);
    void onEvent(const Frame& frame, const Body& body, const Event& event);
    void footDown(const Frame& frame, const Body& body, const glm::dvec3& at, bool left);
    void breathe(const Frame& frame, const Body& body);
    void ripples(const Frame& frame, Body& body);
    std::optional<f32> waterLevel(const Frame& frame, const glm::dvec3& feet, f32 above) const;
    // A free place for one more body effect (nullptr: as many alive as the budget allows); the caller stores
    // the handle of what it spawns there.
    EffectHandle* effectSlot(const Effects& effects);
    f32 random();

    game::BodyData m_data;
    pack::PackReader m_bodyPack;
    pack::PackReader m_soundPack;
    audio::GroundSounds m_ground;
    bool m_ready = false;
    std::vector<Actors::BodyView> m_views; // scratch
    std::vector<Body> m_bodies;            // this frame's
    std::vector<BodyState> m_states;       // by actor id
    std::vector<i32> m_bodyOf;             // actor id -> m_bodies index this frame (-1)
    std::vector<ShadowQuad> m_shadowQuads; // this frame's
    std::vector<f32> m_distances;          // scratch: the bodies' distances (the shadows' budget)
    std::vector<Event> m_events;           // scratch
    physics::TriangleSet m_floorSet{512};
    FootprintPool m_footprints;
    RipplePool m_ripples;
    std::vector<u64> m_footprintTextures;  // index -> texture hash (Footprint::texture)
    std::vector<ParticleVertex> m_scratch; // writeQuads() builds here, then copies to the mapped buffer
    f32 m_distanceScale = 1.0f;
    f32 m_marksScale = 1.0f;
    u32 m_frame = 0;
    u32 m_random = 0x9E3779B9u;
    Stats m_stats;
    struct DemoBleed {
        u32 actor = Actors::kNone;
        glm::dvec3 feet{0.0};
        f32 yaw = 0.0f;
        f32 timer = 0.0f;
        u32 hits = 0;
    };
    std::array<DemoBleed, 4> m_demoBleeds{};
    std::array<EffectHandle, kMaxEffects> m_effectSlots{};
    struct DemoWake {
        glm::dvec3 start{0.0};
        f32 yaw = 0.0f;
        f32 travelled = 0.0f;
        f32 timer = 0.0f;
        bool active = false;
    } m_demoWake;
    DemoWake m_demoPoof; // the same fields: where, facing, -, timer, on
};

} // namespace vd
