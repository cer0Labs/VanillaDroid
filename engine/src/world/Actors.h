#pragma once

#include "anim/Animator.h"
#include "anim/Skeleton.h"
#include "core/Clock.h"
#include "core/Result.h"
#include "core/Types.h"
#include "fx/EffectSet.h"
#include "game/Appearance.h"
#include "pack/PackReader.h"
#include "render/Renderer.h"
#include "world/AssetStreamer.h"
#include "world/Camera.h"
#include "world/InteriorLighting.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace vd {

class JobSystem;
namespace fx {
class ParticleSystem;
}

struct ActorStats {
    u32 actors = 0;   // alive
    u32 visible = 0;  // drawn this frame
    u32 sampled = 0;  // skeletons sampled this frame (the rest reused their last pose: animation LOD)
    u32 bones = 0;    // matrices written this frame
    u32 draws = 0;    // ActorDraws (models + attachments)
    u32 events = 0;   // phase 14: events crossed this frame (Actors::events)
    u32 modelsLoaded = 0;
    u32 texturesLoaded = 0;
    u32 composedSkins = 0;
    u32 pendingAssets = 0;
    f32 animateMs = 0.0f; // collect(): culling + sampling + lists, main thread wall time
    f32 sampleMs = 0.0f;  // of which the parallel sampling (workers + main thread)
    f32 worstUploadMs = 0.0f;
};

// Output of Actors::collect for FrameParams. Capacities are reserved once by the owner.
struct ActorFrame {
    std::vector<ActorDraw> draws;
    std::vector<ActorInstanceData> instances;
    std::vector<u16> geosets;
    std::vector<glm::vec4> colors;
    std::vector<UvTransform> uvTransforms; // phase 9: texture animations (ActorDraw::firstUvTransform)
    u32 boneCount = 0;
};

// Animated actors: characters, creatures and what they carry. Models (actors.vdpk) and textures (textures.vdpk)
// are streamed on the "vd-actors" thread and reference-counted; a character's skin is composed on the GPU once
// its layers are loaded (identical skins are shared). Every frame the visible actors' skeletons are sampled on the
// job workers straight into the renderer's bone palette, with fewer updates per second the further away they are
// (the last pose is reused in between), and nothing is sampled off screen. Game thread only.
class Actors {
public:
    static constexpr u32 kNone = ~0u;
    static constexpr u64 kStagingBytes = 16ull << 20;
    static constexpr u32 kMaxVisible = 1024;

    ~Actors();

    // `db` and `textures` must stay open while the actors are.
    Result<void> open(const std::filesystem::path& packsDir, const pack::PackReader& db,
                      const pack::PackReader& textures, Renderer& renderer);
    void close(Renderer& renderer);
    bool isOpen() const { return m_textures != nullptr; }
    const game::AppearanceDb& appearance() const { return m_appearance; }
    const anim::AnimFallbacks& fallbacks() const { return m_fallbacks; }

    // `effect`: a spell effect model (phase 9): its particles start cold instead of warm.
    u32 create(bool effect = false);
    void destroy(u32 actor);
    // Loads what the look needs; nothing changes on screen until it is there (the previous look stays).
    void setLook(u32 actor, const game::ActorLook& look);
    const game::ActorLook* look(u32 actor) const;
    // World position (server coordinates), facing (radians, 0 = +X north) and uniform scale.
    void setPlacement(u32 actor, const glm::dvec3& position, f32 yaw, f32 scale);
    // Animation to loop (or hold at the end), played `speed` times faster than authored. With a `groundSpeed`
    // (yards per second the actor moves at), movement cycles play at the rate that keeps the feet on the ground.
    // `legsHeld`: a stand state keeps the legs (sitting, on a chair, kneeling, asleep): the actions that may play on
    // the upper body do so over it instead of standing the actor up (WoW.exe 0x5FE6C8).
    void play(u32 actor, u16 animation, bool loop, f32 speed, f32 groundSpeed = 0.0f, bool legsHeld = false);
    // Rides `mount` (another actor) on its saddle; kNone = on foot.
    void setMount(u32 actor, u32 mount);
    // Phase 11: how the actor and what it carries are drawn (ActorDraw::fade: rgb tint, a < 1 translucent; a ghost,
    // stealth, invisibility: game/UnitFade). (1, 1, 1, 1) = as it is.
    void setFade(u32 actor, const glm::vec4& fade);
    // Phase 14 (spellfx): the color its spell kits give it (tints, flashes: SpellVisuals), multiplied with
    // the fade's; (1, 1, 1) = none.
    void setTint(u32 actor, const glm::vec3& rgb);
    // Phase 14: the player's character as the camera comes close: its opacity on top of setFade() (1 as it is, below
    // 1 translucent, 0 not drawn at all: first person). What it carries goes with it.
    void setCameraFade(u32 actor, f32 alpha);
    // Phase 9: follows another actor's attachment point (attach::kHandRight, kHelm...; spell effects on a hand or
    // the chest), like a rider on its mount's saddle; kNone = free again.
    void attach(u32 actor, u32 parent, u32 attachmentId);
    // Phase 9: follows an attachment point of what another actor holds in an equipment slot (an enchantment's glow
    // along a weapon: the weapon model's points 0-4); not drawn while the slot holds nothing.
    void attachToItem(u32 actor, u32 parent, u8 equipSlot, u32 itemAttachmentId);
    // What the actor rides or follows (kNone: nothing; also once that one is destroyed).
    u32 parent(u32 actor) const { return actor < m_actors.size() && m_actors[actor].alive ? m_actors[actor].mount : kNone; }

    // Phase 9: an action over the animation play() set: a one-shot (a swing, a wound, a cast, an emote) that blends
    // in, plays once and blends back out, or a loop until stopAction(). It takes the whole body, or only the upper
    // body while the legs walk, run, swim or keep a stand state and the animation allows it (anim::upperBodyAnimation;
    // EmoteEat always); an action that does not outrank what the actor is doing (dead, mounted; an emote that needs
    // the legs while running) is not shown. A model without the animation plays its fallbacks; when they end at Stand, nothing plays. Calling it
    // again restarts the action (the next swing).
    static constexpr u16 kNoAction = 0xFFFF;
    void playAction(u32 actor, u16 animation, bool loop = false, f32 speed = 1.0f);
    void stopAction(u32 actor);
    // The action playing (requested and not finished), kNoAction when none.
    u16 action(u32 actor) const;
    // The animation the actor plays under its action (after fallbacks), kNoAction before its model is loaded.
    u16 playing(u32 actor) const;

    // Phase 9: loads a model nothing shows yet (a spell effect about to be needed: its first cast then shows at
    // once); it stays in the cache of unused effect models a while.
    void preload(u64 model);
    // Phase 10 (memory pressure): frees the cache of unused effect models at once. Returns how many went.
    u32 trimIdleModels();

    // Phase 9: particles and ribbons of the actors' models (and what they carry), through this system (nullptr: none).
    // Set once, before any actor is created.
    void setParticles(fx::ParticleSystem* particles) { m_particles = particles; }
    // Phase 11: who tells the light of the place an actor stands in (WorldObjects: the interiors), nullptr: the
    // outdoor light everywhere.
    InstanceLightSource* lightSource = nullptr;
    // Stops the particles of an actor's models (they fade as they die) and hides its meshes.
    void stopEffects(u32 actor);
    // Some particle of the actor (or of what it carries) is still alive.
    bool effectsAlive(u32 actor) const;
    bool ready(u32 actor) const; // its model is loaded (it can be drawn)
    // Last drawn world position of an attachment (name plates use attach::kName); nullopt when not drawn.
    std::optional<glm::dvec3> attachmentPosition(u32 actor, u32 attachmentId) const;
    // Phase 9: the actor's model has that attachment point (true while its model is still loading).
    bool hasAttachment(u32 actor, u32 attachmentId) const;
    // Top of the model (world), from its bounds; nullopt when not drawn last frame.
    std::optional<glm::dvec3> headPosition(u32 actor) const;
    // Phase 14 (spellfx): where a fishing line leaves the pole the actor holds in an equipment slot (world,
    // as drawn last frame): the item model's $CCH event (else its far end along +X); nullopt when nothing is
    // drawn there.
    std::optional<glm::dvec3> itemTip(u32 actor, u8 equipSlot) const;
    bool visible(u32 actor) const;
    // Duration left in the playing one-shot animation.
    bool animationFinished(u32 actor) const;
    // Phase 9: the length of the sequence playing (seconds), 0 before the model is loaded.
    f32 animationSeconds(u32 actor) const;
    // Phase 13 (living world objects): actors.vdpk has this model (a game object's M2 with its animations); the
    // actor's shown model has this animation (false while it loads); the world bounding sphere it was last drawn
    // with (center, radius; nullopt when not drawn last frame).
    bool hasModel(u64 model) const { return m_models.find(model).has_value(); }
    bool hasAnimation(u32 actor, u16 animation) const;
    std::optional<glm::vec4> bounds(u32 actor) const;

    // Phase 14: the M2 events the actors' animations crossed in the last frame ("$FD1" a foot down, "$CSD" an
    // emote's sound...: pack::actorEventId), for the actors within eventDistance of the camera, drawn or not: who,
    // which event of its model (index, id and data word) and where its point was (world: posed when the actor was
    // drawn, else at its bind pose where the actor stands). A one-shot action over the whole body silences its base
    // animation's events. Filled by finishCollect(), valid until the next beginCollect(); at most kMaxEvents.
    struct EventHit {
        u32 actor = kNone;
        u32 id = 0;
        u32 data = 0;
        u32 index = 0;
        glm::dvec3 position{0.0};
    };
    static constexpr u32 kMaxEvents = 256;
    std::span<const EventHit> events() const { return m_eventHits; }
    f32 eventDistance = 80.0f;
    // The events of the actor's shown model (empty while none is shown). Those without pack::kActorEventTimed are
    // always on while it shows (a sound loop): eventPosition() tells where one is (world, as in events()), nullopt
    // while no model is shown.
    std::span<const pack::ActorEvent> modelEvents(u32 actor) const;
    std::optional<glm::dvec3> eventPosition(u32 actor, u32 index) const;

    // Phase 14 (bodyfx): a unit's body — a character, a creature, a mount — has a blob shadow, leaves footprints and
    // ripples, bleeds and breathes (world/UnitBodyEffects); spell effects, game objects and previews are no bodies.
    void setBody(u32 actor, bool body);
    struct BodyView {
        u32 actor = kNone;
        u32 mount = kNone;    // what it rides (kNone: on its own feet)
        u64 model = 0;        // the shown model (actors.vdpk asset hash)
        glm::dvec3 feet{0.0}; // world
        f32 yaw = 0.0f;
        f32 scale = 1.0f;
        f32 top = 0.0f;       // the model's bounds' top times its scale (yards)
        f32 opacity = 1.0f;   // its fade's alpha (setFade)
        u16 animation = 0;    // the base animation it plays (after fallbacks)
    };
    // The bodies drawn this frame (after finishCollect), into `out` (cleared).
    void collectBodies(std::vector<BodyView>& out) const;

    // Uploads what the IO thread finished, composes skins whose layers arrived, frees what nothing uses.
    void update(Renderer& renderer);
    // Advances the animations by dt, culls, and starts sampling the visible skeletons on the job workers (straight
    // into the renderer's bone palette). The caller may do other work (not using `jobs`) before finishCollect(),
    // which waits for the samples and fills `out` (cleared here).
    void beginCollect(const glm::dvec3& camera, const Frustum& frustum, const glm::vec3& cameraForward, f32 dt,
                      u32 nowMs, JobSystem& jobs, Renderer& renderer, ActorFrame& out);
    void finishCollect(JobSystem& jobs, ActorFrame& out);
    void collect(const glm::dvec3& camera, const Frustum& frustum, const glm::vec3& cameraForward, f32 dt, u32 nowMs,
                 JobSystem& jobs, Renderer& renderer, ActorFrame& out) {
        beginCollect(camera, frustum, cameraForward, dt, nowMs, jobs, renderer, out);
        finishCollect(jobs, out);
    }

    const ActorStats& stats() const { return m_stats; }
    void resetWorst() { m_stats.worstUploadMs = 0.0f; }

    // Parity (unit frame portraits, app/Portraits). The shown look's key (world/PortraitCache:
    // portraitLookKey), 0 while nothing is shown; it changes when a new look replaces the shown one.
    u64 portraitKey(u32 actor) const;
    // Its model's portrait framing (pack::ActorPortrait, model space), nullptr while no model is shown.
    const pack::ActorPortrait* portraitCamera(u32 actor) const;
    // The shown look standing still — the first frame of Stand (the bind pose without it), no action, what it
    // carries on its points in their own stand pose — at the model's origin, for a camera at `eye` looking at
    // `target` (model space; the instances are placed relative to `eye`, as the world's are to the camera).
    // Its draws go to `draws`; its instances, geosets, colors, texture animations and bones after the frame's
    // in `out` (the renderer's bone palette from out.boneCount on, which grows). Call after finishCollect().
    // Returns the draws added (0: no model loaded, or no room in the palette).
    u32 appendStill(u32 actor, const glm::vec3& eye, const glm::vec3& target, Renderer& renderer,
                    ActorFrame& out, std::vector<ActorDraw>& draws);

    // Phase 15 (the interface's model frames, app/ModelViews). An offscreen actor is never drawn in the world (not
    // culled, sampled, placed, heard or shadowed; no particles), but its animations advance every frame: set it right
    // after create(), before its look.
    void setOffscreen(u32 actor, bool offscreen);
    // The shown model's bounding box (model space, its bind pose); false while no model is shown.
    bool shownBounds(u32 actor, glm::vec3& min, glm::vec3& max) const;
    // The selection circle's half width under the shown model, unscaled (pack::actorSelectionHalfSize); 0 while no
    // model is shown.
    f32 selectionHalfSize(u32 actor) const;
    // Like appendStill(), but in the pose the actor's animations are in now (its animation and what it carries in
    // theirs), turned by `yaw` radians about the model's up axis. Call after finishCollect().
    u32 appendPosed(u32 actor, const glm::vec3& eye, const glm::vec3& target, f32 yaw, Renderer& renderer,
                    ActorFrame& out, std::vector<ActorDraw>& draws);

    // Sample the skeletons on the job workers (while the caller culls the world) or on the calling thread. Workers
    // only pay off where the system schedules them on fast cores: measured on the Mi 11X without ADPF, the workers'
    // short bursts ran on idle little cores (132 us a skeleton against 9-15 us on the game thread's big core).
    bool sampleOnWorkers = true;
    // Culling and animation level of detail (yards, scaled by the actor's size for small creatures).
    f32 drawDistance = 150.0f;
    f32 fullRateDistance = 30.0f; // closer: sampled every frame
    f32 halfRateDistance = 60.0f; // every 2nd frame, then every 4th up to twice this

private:
    enum class State : u8 { Requested, Ready, Failed };

    struct TextureEntry {
        u64 hash = 0;
        State state = State::Requested;
        TextureHandle handle;
        u32 refs = 0;
        std::vector<std::pair<u32, u32>> waiting; // (model, material) to update when it arrives
    };
    struct ModelEntry {
        u64 hash = 0;
        State state = State::Requested;
        ModelHandle handle;
        u32 refs = 0;
        u8* blob = nullptr; // CPU copy: the skeleton samples from it
        usize size = 0;
        anim::Skeleton skeleton;
        std::vector<u16> geosets;  // sorted geoset ids of the model's parts
        std::vector<u32> textures; // per material: TextureEntry of its file texture, or kNone
        u32 pendingTextures = 0;
        bool animatedColors = false;
        // Phase 9: a spell effect's model stays loaded a while after its last effect ends (the next cast of the
        // spell shows at once, without loading it again).
        bool cached = false;
        bool idle = false; // in m_idleModels
        u32 idleFrame = 0;
        // Phase 9: particles, ribbons and texture animations (on the heap: particle instances point at it).
        std::unique_ptr<fx::EffectSet> effects;
        std::vector<u32> effectTextures; // per emitter, then per ribbon: TextureEntry or kNone
        glm::vec3 center{0.0f};
        f32 radius = 1.0f;
        f32 top = 2.0f; // bounds max z
    };
    struct Attached {
        u32 attachment = 0;
        u8 slot = 0xFF; // phase 9: the equipment slot of the item (AttachedModel::slot)
        u32 model = kNone;   // ModelEntry
        u32 texture = kNone; // TextureEntry of its object skin (slot Cape)
        anim::Animator animator;
        std::vector<anim::BoneMatrix> bones; // last pose
        anim::BoneMatrix transform{};        // model -> world (camera-relative), this frame
        bool placed = false;
        u32 fx = kNone;                       // its particle instance (shown attachments only)
        std::vector<UvTransform> uvTransforms; // texture animations of the last pose
        u32 uvBase = 0;
    };
    struct Actor {
        bool alive = false;
        game::ActorLook look;
        u32 model = kNone;
        std::array<u32, 8> slotTextures{};
        std::vector<u32> skinLayers; // TextureEntry per look.skin layer
        u64 skinKey = 0;             // shared composed skin, 0 = none
        bool skinComposed = false;
        std::vector<Attached> attachments;
        std::vector<u16> geosetIndices; // into the model's geoset table; resolved when the model arrives
        bool geosetsResolved = false;
        // The previous look stays on screen until the new one is loaded.
        struct Shown {
            u32 model = kNone;
            std::array<u32, 8> slotTextures{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
            std::array<TextureHandle, kActorTextureSlots> textures{};
            std::vector<u16> geosetIndices;
            std::vector<Attached> attachments;
            u64 skinKey = 0;
            u64 meshKey = 0; // shared packed mesh of its visible geosets, 0 = none (drawn by parts)
            ActorMeshHandle mesh;
            u32 fx = kNone; // phase 9: the particle instance of its model
            u64 portraitKey = 0; // parity: portraitLookKey() of the look shown
        } shown;

        anim::Animator animator;
        u16 animation = anim::animation::kStand;
        bool loop = true;
        f32 speed = 1.0f;
        f32 groundSpeed = 0.0f;
        u32 random = 1;
        glm::dvec3 position{0.0};
        f32 yaw = 0.0f;
        f32 scale = 1.0f;
        u32 mount = kNone;
        u32 mountAttachment = 0; // where it sits on `mount` (attach::kShield: the saddle)
        u8 mountItemSlot = 0xFF; // phase 9: on the item `mount` holds in this equipment slot (attachToItem)
        bool unplaced = false;   // phase 9: what it follows is not there this frame: not drawn
        bool effect = false;     // a spell effect: particles start cold
        bool hidden = false;     // stopEffects(): meshes no longer drawn
        glm::vec4 fade{1.0f};    // phase 11: setFade()
        glm::vec3 tint{1.0f};    // phase 14 (spellfx): setTint()
        f32 cameraAlpha = 1.0f;  // phase 14: setCameraFade()
        bool body = false;       // phase 14 (bodyfx): setBody()
        bool offscreen = false;  // phase 15: setOffscreen()

        // Phase 9: the action over the animation (playAction), with its own sequences.
        anim::Animator actionAnimator;
        u16 action = kNoAction;    // requested, kNoAction = none
        u16 actionShown = kNoAction; // what the model plays for it (after fallbacks)
        u16 playing = kNoAction;   // the base animation playing (after fallbacks)
        u32 actionSerial = 0;      // playAction() calls: each one (re)starts the action
        u32 actionStarted = 0;     // the serial started
        f32 actionSpeed = 1.0f;
        f32 actionWeight = 0.0f;
        bool actionLoop = false;
        bool actionUpper = false;  // on the upper body only (the legs keep the base animation)
        bool legsHeld = false;     // the base is a stand state (play()'s legsHeld)
        bool actionEnding = false; // blending out

        // Frame state.
        std::vector<anim::BoneMatrix> bones; // last sampled pose
        std::vector<glm::vec4> colors;       // material colors of the last pose
        std::vector<UvTransform> uvTransforms; // texture animations of the last pose
        u32 uvBase = 0;
        anim::BoneMatrix transform{};        // model -> world, camera-relative
        u32 sampledFrame = 0;
        u32 boneBase = 0;
        u32 colorBase = 0;
        bool visibleNow = false;
        bool sampleNow = false;
        glm::dvec3 drawnCamera{0.0};
        // Phase 11: the interior light it stands in (packed, 0 = outdoors), asked where and how long ago.
        std::array<u32, 2> light{};
        glm::dvec3 lightPoint{1e30};
        u32 lightAge = 0;
    };
    struct SharedSkin {
        TextureHandle handle;
        u32 refs = 0;
    };
    struct SharedMesh {
        ActorMeshHandle handle;
        u32 refs = 0;
    };

    // An actor slot back to its defaults, keeping the memory of its per-frame arrays.
    void resetActor(Actor& actor);
    u32 acquireModel(u64 hash);
    void releaseModel(u32 model);
    void freeModel(u32 model);
    void evictIdleModels();
    u32 acquireTexture(u64 hash);
    void releaseTexture(u32 texture);
    void requestPending();
    void onModelReady(u32 model, AssetStreamer::Done& done, Renderer& renderer);
    void onTextureReady(u32 texture, const AssetStreamer::Done& done, Renderer& renderer);
    bool lookLoaded(const Actor& actor) const;
    void promote(Actor& actor, Renderer& renderer); // the loaded look becomes the shown one
    void releaseShown(Actor::Shown& shown);
    void releaseLook(Actor& actor);
    void resolveGeosets(Actor& actor);
    // The packed mesh of the shown model with the shown geosets (shared by actors wearing the same ones).
    void acquireMesh(Actor& actor, Renderer& renderer);
    void sample(Actor& actor, u32 nowMs, const glm::vec3& cameraForward);
    // Phase 9: starts, shows or ends the actor's action over its base animation `base` (what it plays).
    void animateAction(Actor& actor, const anim::Skeleton& skeleton, u16 base, f32 dt);
    // Phase 14: the events the actor's animators just played over (positions come in finishCollect).
    void collectEvents(u32 index, const Actor& actor, const anim::Skeleton& skeleton);
    glm::dvec3 eventWorld(const Actor& actor, const pack::ActorEvent& event) const;
    // Phase 9: the particle instance of a shown model (kNone if it has no particles).
    u32 createEffects(u32 model, u32 seed, bool warm);
    void placeEffects(const Actor& actor);
    // appendStill() / appendPosed(): `posed` = the actor's current animations, else the first frame of Stand.
    u32 appendModel(u32 actor, const glm::vec3& eye, const glm::vec3& target, f32 yaw, bool posed, Renderer& renderer,
                    ActorFrame& out, std::vector<ActorDraw>& draws);
    void place(Actor& actor, const glm::dvec3& camera);
    // Job body: samples, places and copies out the visible actors [begin, end).
    void sampleRange(u32 begin, u32 end);
    struct SampleWork {
        Actors* self = nullptr;
        u32 offset = 0;
        void operator()(u32 begin, u32 end) const { self->sampleRange(offset + begin, offset + end); }
    };
    // The frame being collected (set by beginCollect for the jobs).
    struct Collect {
        glm::dvec3 camera{0.0};
        glm::vec3 cameraForward{0.0f};
        u32 nowMs = 0;
        anim::BoneMatrix* palette = nullptr;
        ActorFrame* out = nullptr;
        u32 firstRider = 0;
        u32 firstDeep = 0; // phase 9: effects on riders, after the riders
        u32 bones = 0;
        Clock::time_point sampleStart{};
        f64 mainMs = 0.0;
        bool active = false;
    } m_collect;
    SampleWork m_work;
    SampleWork m_riderWork;
    SampleWork m_deepWork;

    game::AppearanceDb m_appearance;
    anim::AnimFallbacks m_fallbacks;
    pack::PackReader m_models;
    const pack::PackReader* m_textures = nullptr;
    StagingBlock m_staging;
    AssetStreamer m_streamer;
    Renderer* m_renderer = nullptr;

    std::vector<ModelEntry> m_modelEntries;
    std::vector<u32> m_freeModels;
    std::unordered_map<u64, u32> m_modelIndex;
    std::vector<TextureEntry> m_textureEntries;
    std::vector<u32> m_freeTextures;
    std::unordered_map<u64, u32> m_textureIndex;
    std::vector<u64> m_requestQueue;
    std::vector<AssetStreamer::Done> m_waiting;
    std::unordered_map<u64, SharedSkin> m_skins;
    std::unordered_map<u64, SharedMesh> m_meshes;
    std::vector<u16> m_meshIndices;        // scratch for acquireMesh
    std::vector<ActorMeshRun> m_meshRuns;  // idem
    std::vector<u8> m_geosetEnabled;       // idem

    std::vector<Actor> m_actors;
    std::vector<u32> m_freeActors;
    std::vector<u32> m_pendingLooks; // actors whose look is still loading
    std::vector<u32> m_idleModels;   // phase 9: cached effect models nothing uses now (ModelEntry::cached)
    std::vector<u32> m_preloads;     // phase 9: models preload() holds until they are loaded
    std::vector<u32> m_visible;      // scratch: this frame's visible actors (riders last)
    std::vector<u32> m_ordered;      // scratch: m_visible by level (mounts, riders, effects on riders)
    std::vector<anim::BoneMatrix> m_stillBones; // scratch: appendStill()'s pose (read back to place items)
    std::vector<anim::BoneMatrix> m_stillItemBones; // idem, of what it carries
    std::vector<EventHit> m_eventHits; // phase 14: reserved kMaxEvents
    u32 m_frame = 0;
    fx::ParticleSystem* m_particles = nullptr;
    ActorStats m_stats;
};

} // namespace vd
