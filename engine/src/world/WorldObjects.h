#pragma once

#include "core/Clock.h"
#include "core/Result.h"
#include "core/Types.h"
#include "fx/EffectSet.h"
#include "pack/ModelFormat.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"
#include "render/Renderer.h"
#include "world/AssetStreamer.h"
#include "world/Camera.h"
#include "world/DoodadAnimation.h"
#include "world/InteriorLighting.h"
#include "world/Terrain.h"
#include "world/WmoBsp.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace vd {

struct ObjectStats {
    u32 instances = 0;        // placed (doodads + WMOs + WMO doodads)
    u32 visibleInstances = 0; // drawn this frame
    u32 visibleGroups = 0;    // WMO groups drawn this frame
    u32 totalGroups = 0;      // groups of the visible WMOs
    u32 portalCulledGroups = 0; // interiors in view whose portals are not
    u32 modelsDrawn = 0;
    u32 modelsLoaded = 0;
    u32 texturesLoaded = 0;
    u32 pendingAssets = 0; // requested, not yet uploaded
    f32 lastUploadMs = 0.0f;
    f32 worstUploadMs = 0.0f;
    // Phase 14 (doodads): instances drawn with their skeleton, and in the distance fade, in the last collect().
    u32 animatedInstances = 0;
    u32 fadingInstances = 0;
    // Phase 18 (world): where the camera is (the 1.12 rule) and what the portals let it see from there.
    bool cameraInside = false;
    u16 cameraGroup = 0xFFFF;
    bool outdoorsVisible = true;
    u32 portalWalkGroups = 0; // groups of the camera's building the walk reached
    u32 portalsTested = 0;
    f32 locateMs = 0.0f; // locating the camera and walking the portals, last frame
};

// Doodads (M2) and buildings (WMO) of the loaded terrain tiles: instances from the tile placements (a
// building spanning several tiles is one instance, keyed by its uniqueId; its doodad set is spawned once
// the building's model is known), reference-counted models and textures streamed from models.vdpk and
// textures.vdpk on an IO thread, and per-frame culling into instanced draws. Game thread only.
class WorldObjects final : public TerrainListener, public InstanceLightSource {
public:
    static constexpr u64 kStagingBytes = 48ull << 20; // holds the largest model (Stormwind: 29 MB)
    static constexpr u32 kMaxVisible = 16384;

    ~WorldObjects();

    // `textures` must stay open while objects are in use.
    Result<void> open(const std::filesystem::path& packsDir, const pack::PackReader& textures, Renderer& renderer);
    void close(Renderer& renderer);
    bool isOpen() const { return m_textures != nullptr; }

    void onTileLoaded(u32 tile, const u8* blob, usize size) override;
    void onTileUnloaded(u32 tile) override;

    // Uploads what the IO thread finished and frees what nothing uses any more.
    void update(Renderer& renderer);
    // Visible instances grouped by model. Reserve nothing: capacities are fixed at open().
    void collect(const glm::dvec3& camera, const Frustum& frustum, f32 drawDistance, std::vector<ModelDraw>& draws,
                 std::vector<ModelInstanceData>& instances, std::vector<u16>& groups);

    const ObjectStats& stats() const { return m_stats; }
    void resetWorst() { m_stats.worstUploadMs = 0.0f; }

    // Objects the server places (chests, quest objects, doors...): an M2 of models.vdpk at `transform` (model ->
    // world, 3x4 by rows). They live until removed, across maps and tiles. Phase 8: `wmo` for a WMO (transports:
    // drawn group by group like the map's buildings, without their doodads, which would stay behind as it moves).
    static constexpr u32 kNoInstance = ~0u;
    u32 addDynamic(u64 modelHash, const f32 (&transform)[12], bool wmo = false);
    void moveDynamic(u32 instance, const f32 (&transform)[12]);
    void removeDynamic(u32 instance);

    // Phase 9 effects. The doodads whose models have particles, in view and within effectDistance (plus their reach)
    // in the last collect(): instance indices, to place with their model's effects, transform and textures.
    std::span<const u32> visibleEffects() const { return m_visibleEffects; }
    const fx::EffectSet* instanceEffects(u32 instance) const;
    const f32* instanceTransform(u32 instance) const { return m_instances[instance].transform; } // model -> world, 3x4
    u32 instanceSerial(u32 instance) const { return m_instances[instance].serial; } // changes when the slot is reused
    // Parity (tapping objects): an instance's world bounding sphere (xyz centre, w radius) once its model is
    // drawable; nullopt before (or for a free slot).
    std::optional<glm::vec4> instanceSphere(u32 instance) const;
    // Development: the model path of an instance (empty if the pack has no names).
    std::string_view instanceModelName(u32 instance) const;
    // The textures of an effect instance's emitters, into out[0 .. emitters).
    void effectTextures(u32 instance, std::span<TextureHandle> out) const;
    // Texture animations (water, lava...) of the M2 doodads drawn by the last collect() at `globalMs`: appends their
    // matrices to `out` and points the draws at them (ModelDraw::firstUvTransform). `draws` are collect()'s.
    void collectUvTransforms(u32 globalMs, std::span<ModelDraw> draws, std::vector<UvTransform>& out);
    // Phase 11: the M2 doodads' own lights at `globalMs` (their flicker) into the draws of the last collect().
    void collectSelfLights(u32 globalMs, std::span<ModelDraw> draws) const;
    f32 effectDistance = 100.0f;
    // Parity pass: whether a character with its feet at `p` stands inside a building's true interior (a group flagged
    // interior, not lit like the outdoors, with portals), as the client switches the minimap to its indoor zoom. Only
    // buildings already known. Asked every frame: standingAt()'s answer (the boxes without the BSP).
    bool insideInterior(const glm::dvec3& p);
    // Phase 15: the building of insideInterior()'s last yes (its indoor minimap): its WMO (models.vdpk's key) and
    // where it stands (model -> world rows, rotation * scale, and its scale); wmo 0 after a no.
    struct InteriorPlace {
        u64 wmo = 0;
        f32 transform[12]{};
        f32 scale = 1.0f;
    };
    const InteriorPlace& interiorPlace() const { return m_interiorPlace; }
    // Phase 11: the skybox (pack::skyModelPath hash) of the building whose groups' boxes hold `p` (a WMO's
    // MOSB: Stratholme, Dire Maul), 0 = none. Only buildings already known; cheap when none has one.
    u64 skyboxAt(const glm::dvec3& p) const;
    // Phase 11: whether the segment from `from` along the unit `direction` for `length` yards passes through
    // the bounding sphere of a placed M2 of at least `minRadius` yards whose center lies ahead (trees and big
    // rocks: the sun's glare hides behind their crowns, which the collision world does not hold). Tests the
    // instance slots [first, first + count) of instanceSlots(), so a caller can spread the work over frames;
    // only instances already culled once have a sphere.
    bool segmentHitsDoodad(const glm::dvec3& from, const glm::vec3& direction, f32 length, f32 minRadius,
                           u32 first, u32 count) const;
    u32 instanceSlots() const { return static_cast<u32>(m_spheres.size()); }

    // Phase 11 (11.4): the light of interiors. Doodads inside a true interior carry their light from the moment they
    // are placed; actors ask here (InstanceLightSource, cached per actor by Actors). Off: everything is lit like
    // the outdoors, as before (VD_INTERIORS=0, to compare).
    bool interiorLighting = true;
    bool lightAt(const glm::dvec3& position, std::array<u32, 2>& packed) override;
    // MFOG: the fog of the building the camera at `p` is in, when one of its groups there is an interior (neither
    // exterior nor lit like the outdoors: WoW.exe 0x69de8b), as 1.12 builds it (world/InteriorLighting.h interiorFog());
    // nullopt elsewhere. The frame's fog moves towards it over time (Game::updateLightConditions()).
    struct FogSample {
        glm::vec3 color{0.0f};
        f32 end = 0.0f;         // yards
        f32 startScale = 0.25f; // start = end * startScale (0..1)
    };
    std::optional<FogSample> fogAt(const glm::dvec3& p, bool underwater);

    // Doodads fade out with distance: a doodad of radius r is drawn up to base + r * perRadius yards.
    f32 doodadBaseDistance = 80.0f;
    f32 doodadDistancePerRadius = 30.0f;
    // WMO groups by screen size only (~6 px radius at 1080p), so buildings keep their silhouettes. The
    // interior flag can't shorten this: it means "lit by vertex colors", and whole cities carry it. True
    // interiors (interior and not lit like the outdoors) are also drawn only when the camera is inside
    // their box or one of their portals (doors, windows) is in view.
    f32 groupBaseDistance = 150.0f;
    f32 groupDistancePerRadius = 150.0f;
    f32 interiorBoxMargin = 2.0f; // yards around an interior's box that count as inside
    // Through a portal of radius r, an interior is drawn up to base + r * perRadius yards (a door: ~190).
    f32 portalBaseDistance = 40.0f;
    f32 portalDistancePerRadius = 60.0f;

    // --- Phase 14 (doodads) ---
    // Animated doodads (world/DoodadAnimation.h): the M2s baked with a skeleton loop their Stand sequence (one pose per
    // model and phase, skinned on the GPU, billboards turned to the camera). Off (VD_DOODAD_ANIM=0): drawn as baked.
    bool animateDoodads = true;
    // 1.12's doodad ranges and their fade (world/DoodadFade.h), stretched by doodadDistanceScale. Off
    // (VD_DOODAD_FADE=0): the rule above (doodadBaseDistance + r * doodadDistancePerRadius), popping in.
    bool fadeDoodads = true;
    bool traceDoodads = false; // VD_TRACE_DOODADS=1: the animated and fading instances in the log now and then
    f32 doodadDistanceScale = 1.0f;
    f32 doodadFadeCeiling = 1.0f; // development: no M2 doodad above this opacity (the dither seen everywhere)
    // The clock of the doodads' animations (ms), set every frame before update() and collect().
    u32 animationMs = 0;
    // An animated doodad's bones as its last collect() posed them (model space, billboards not turned): what its
    // particle emitters follow. nullptr for one drawn as baked (or not animated).
    const anim::BoneMatrix* instancePose(u32 instance, u32& boneCount);

    // --- Phase 18 (world): the group the camera is in, and the portals walked from it -----------------------------
    // Where a point stands the way 1.12 finds the camera's group (WoW.exe 0x6821f0 -> 0x6be250): a segment from it
    // straight down for 1760 yards meets the nearest face of a group's BSP (models.vdpk v8), or nearer still the
    // nearest portal it crosses (then the group on the point's side of that portal); a hit in an exterior group (MOGP
    // 0x8), or terrain nearer than it (`terrainBelow`: its height under the point, holes left out), means outdoors.
    // Only buildings already known; models without the BSP section (older packs) fall back to their groups' boxes.
    // `exteriorOutdoors` false keeps an exterior group's hit (a city's streets: their rooms in WMOAreaTable).
    struct Location {
        u32 instance = kNoInstance;       // the building's instance; kNoInstance: outdoors
        u16 group = pack::kModelNoGroup;  // the group the point is in
        u16 other = pack::kModelNoGroup;  // across the portal the segment crossed, when not outdoors
        bool inside() const { return instance != kNoInstance; }
    };
    Location locate(const glm::dvec3& p, std::optional<f32> terrainBelow, bool exteriorOutdoors = true) const;
    // Where a character with its feet at `feet` stands, for the minimap's indoors and the room's name and music: the
    // group 1.12's rule finds (locate()) from a yard over the feet, where lightAt() samples them too (a segment starting
    // on the floor itself can pass under it and on to the terrain below the building). Kept until the feet move a
    // quarter of a yard or for 8 calls (a city's groups cost ~5 us). `bsp` false: no building around has its BSP
    // (an older models.vdpk, VD_WORLD_PORTALS=0), nothing is known.
    struct Standing {
        Location location;     // exterior groups kept
        bool placed = false;   // in a building of the map (not one the server placed: a ship)
        u32 uniqueId = 0;      // ...its placement (MODF)
        u32 groupCount = 0;    // the building's groups
        bool interior = false; // a true interior, as insideInterior()
        bool bsp = false;
    };
    static constexpr f64 kStandingProbe = 1.0; // yards over the feet
    const Standing& standingAt(const glm::dvec3& feet);
    Standing standingNow(const glm::dvec3& feet) const; // the same without the cache
    // Whether a located group is a true interior (flagged interior, not lit like the outdoors, with portals).
    bool trueInterior(const Location& where) const;
    u32 groupFlags(const Location& where) const; // pack::kModelGroup*, 0 outdoors

    // Development (VD_WORLD_CHECK_ROOMS): points on the floors of the true interiors (their faces looking up, every
    // `stride`th) of the map's buildings within `radius` of `around`, where a character would stand.
    struct FloorSample {
        glm::dvec3 feet;
        u32 instance = kNoInstance;
        u16 group = pack::kModelNoGroup;
        u32 uniqueId = 0;
        u32 groupCount = 0;
    };
    void floorSamples(const glm::dvec3& around, f32 radius, u32 stride, std::vector<FloorSample>& out) const;
    // The terrain's height under a point (holes left out) for locate(), from whoever knows it (the collision world);
    // without one a building's floor under the terrain counts as being in the building.
    using TerrainQuery = std::optional<f32> (*)(const void* context, f64 x, f64 y, f64 top);
    void setTerrainQuery(TerrainQuery query, const void* context) {
        m_terrainQuery = query;
        m_terrainContext = context;
    }
    // Once a frame before collect() (and before the terrain is culled): the camera's place and, inside a building, 1.12's
    // walk through its portals (WoW.exe 0x6b3b20): its groups drawn are those the walk reaches, and the world outside
    // only when a portal on the way leads out. `viewProj`: camera-relative world -> clip, as the frame draws.
    void beginView(const glm::dvec3& camera, const glm::mat4& viewProj);
    const Location& cameraLocation() const { return m_cameraLocation; }
    // False when the camera is inside a building and nothing outside it can be seen: no terrain, sky or other building.
    bool outdoorsVisible() const { return m_outdoorsVisible; }
    // Development (VD_WORLD_PORTALS=0): the phase 3 rule (interiors by the camera's box and their portals' spheres),
    // to compare. The minimap's indoors and the interiors' light and fog then use the boxes too.
    bool portalCulling = true;
    // Development (VD_WORLD_TRACE_PORTALS=2): the doodads within `radius` yards of `camera` the portal rule leaves out
    // this frame, with their building's group (or none: the map's), into the log.
    void traceCulledDoodads(const glm::dvec3& camera, f32 radius) const;
    // Development (VD_WORLD_COMPARE_GROUPS=<yards>): points standing on the floors of the buildings within `radius` of
    // `around`, the group the boxes (phase 3, 11) put each in against 1.12's rule; the differences into the log.
    void compareGroupRules(const glm::dvec3& around, f32 radius, u32 maxReports) const;
    // The matrices written into the renderer's doodad palette by the last collect() (FrameParams::doodadBoneCount).
    u32 doodadBoneCount() const { return m_animator.paletteUsed(); }
    const DoodadAnimator::Stats& animationStats() const { return m_animator.stats(); }

private:
    enum class State : u8 { Requested, Ready, Failed };
    static constexpr u32 kNone = ~0u;

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
        bool wmo = false;
        u32 pendingTextures = 0;
        glm::vec3 center{0.0f}; // model-space bounds center and radius around it
        f32 radius = 0.0f;
        std::vector<u32> textures; // per material: TextureEntry index or kNone
        std::vector<pack::ModelDoodadSet> doodadSets;
        std::vector<pack::ModelDoodad> doodads;
        std::vector<pack::ModelGroup> groups; // WMO only
        std::vector<glm::vec4> portalSpheres;   // model space center + radius, per portal
        std::vector<pack::ModelPortalRef> portalRefs; // ranges per group: ModelGroup::firstPortalRef
        // Phase 18 (world): the portals' planes and polygons, and the groups' BSP trees (pack::ModelBspHeader).
        std::vector<pack::ModelPortal> portals;
        std::vector<glm::vec3> portalVertices;
        std::vector<pack::ModelBspGroup> bspGroups;
        std::vector<pack::ModelBspNode> bspNodes;
        std::vector<u16> bspRefs;
        std::vector<std::array<u16, 3>> bspTriangles;
        std::vector<glm::vec3> bspVertices;
        std::vector<std::array<u32, 2>> bspDoodadRanges; // per doodad: the groups listing it (first, count)
        std::vector<u16> bspDoodadGroups;
        // Phase 9: M2 effects (particles, texture animations) over a kept copy of the blob.
        u8* effectBlob = nullptr;
        std::unique_ptr<fx::EffectSet> effects; // on the heap: particle instances keep pointers to it
        std::vector<u32> effectTextures; // per emitter: TextureEntry index or kNone
        u64 skybox = 0; // phase 11: WMO, its MOSB skybox (pack::ModelHeader::reserved2)
        // Phase 11: the interior lighting tables (pack::ModelLightingHeader) and the ambient (MOHD, RGBA).
        u32 ambient = 0;
        std::vector<pack::ModelLight> lights;
        std::vector<pack::ModelLightKey> lightKeys;
        std::vector<u16> lightRefs;
        std::vector<pack::ModelFog> fogs;
        std::vector<pack::ModelGroupLighting> groupLighting; // per group (WMO), empty without the section
        std::vector<u32> lightCells;
        std::vector<u16> doodadGroups;
        // Phase 14: its skeleton in the animator (kNone: drawn as baked; its blob is kept in effectBlob), when it
        // loaded (its instances' loops start no sooner) and the radius 1.12's distance rule reads.
        u32 skeleton = kNone;
        bool skinnedMesh = false; // its vertices follow the skeleton (else only its effects do)
        u32 readyMs = 0;
        f32 fileRadius = 0.0f;
    };
    struct Instance {
        u32 model = kNone;
        f32 transform[12]{}; // model -> world
        f32 scale = 1.0f;
        bool alive = false;
        bool wmo = false;
        u32 serial = 0;
        std::array<u32, 2> light{}; // phase 11: packed InstanceLight, 0 = lit like the outdoors
        u32 spawnMs = 0;            // phase 14: when it appeared (its animation's phase)
        bool dynamic = false;       // phase 14: placed by the server (addDynamic): not a doodad of the map
    };
    // m_modelDrawable values: ready with all its textures (or failed ones), by kind.
    enum Drawable : u8 { kNotDrawable = 0, kDrawableM2 = 1, kDrawableWmo = 2 };
    struct WmoInstance {
        u32 instance = kNone;
        u32 uniqueId = 0; // the placement (MODF); 0 for one the server placed
        u32 refs = 0; // tiles referencing it
        u16 doodadSet = 0;
        bool doodadsSpawned = false;
        u8 spawnStage = 0; // 0: default set, 1: the placement's set, 2: done
        u32 spawnNext = 0; // next doodad of the current set
        std::vector<u32> doodads;          // Instance indices
        std::vector<glm::vec4> groupSpheres; // world center + radius of each group, once the model is known
        std::vector<glm::vec4> portalSpheres; // world, per portal
    };
    struct TileObjects {
        std::vector<u32> doodads; // Instance indices
        std::vector<u32> wmos;    // uniqueIds
    };

    u32 acquireModel(u64 hash);
    void releaseModel(u32 model);
    u32 acquireTexture(u64 hash);
    void releaseTexture(u32 texture);
    u32 addInstance(u32 model, const f32 (&transform)[12], bool wmo);
    void removeInstance(u32 instance);
    void updateSphere(u32 instance);
    void refreshDrawable(u32 model);
    enum class Spawn : u8 { Done, Partial, NotReady };
    Spawn spawnDoodads(WmoInstance& wmo, Clock::time_point deadline);
    void computeGroupSpheres(WmoInstance& wmo);
    void onModelReady(u32 model, AssetStreamer::Done& done, Renderer& renderer);
    void bindEffects(u32 model, AssetStreamer::Done& done, Renderer& renderer);
    // TextureEntry::waiting entries of effect textures carry this bit with the emitter index (not a material).
    static constexpr u32 kEffectTexture = 0x80000000u;
    void onTextureReady(u32 texture, const AssetStreamer::Done& done, Renderer& renderer);
    void requestPending();
    // Phase 11: the group of a WMO whose box holds `p` (the smallest one), restricted to true interiors.
    struct GroupHit {
        u32 instance = kNone; // the building's Instance
        u32 group = 0;
        glm::vec3 local{0.0f}; // `p` in its model space
    };
    std::optional<GroupHit> findInterior(const glm::dvec3& p) const;
    std::optional<GroupHit> findInteriorByBoxes(const glm::dvec3& p) const; // phase 11's rule (no BSP)
    // Phase 18 (world): `p` in a building's model space (rows are rotation * scale).
    static glm::vec3 toModel(const Instance& instance, const glm::dvec3& p);
    void loadBsp(ModelEntry& entry, const u8* blob, usize size, const pack::ModelHeader& header);
    WmoBspView bspView(const ModelEntry& entry, u32 group) const;
    // The light of a point inside `hit`'s group: the baked light there over the building's ambient, from the strongest
    // of the group's lamps (model space `local`).
    std::array<u32, 2> interiorLightAt(const ModelEntry& model, const f32* transform, u32 group, const glm::vec3& baked,
                                       const glm::vec3& local) const;
    void loadLighting(ModelEntry& entry, const u8* blob, usize size, const pack::ModelHeader& header);

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
    std::vector<u64> m_requestQueue; // tags waiting for room in the streamer
    std::vector<AssetStreamer::Done> m_waiting; // finished IO waiting for this frame's upload budget

    std::vector<Instance> m_instances;
    std::vector<u32> m_freeInstances;
    // Culling data, dense and parallel to m_instances so the per-frame pass streams through ~20 bytes per
    // instance: world bounding sphere (w = 0: not computed yet, w < 0: free slot) and model index, plus one
    // byte per model entry.
    std::vector<glm::vec4> m_spheres;
    std::vector<u32> m_instanceModel;
    std::vector<u8> m_modelDrawable;
    std::unordered_map<u32, WmoInstance> m_wmos; // by uniqueId
    std::unordered_map<u32, TileObjects> m_tiles;

    std::vector<std::pair<u32, u32>> m_visible; // (model, instance), scratch
    std::vector<u32> m_visibleWmos;             // instance indices of WMOs, scratch
    std::unordered_map<u32, WmoInstance*> m_wmoByInstance; // instance index -> building
    std::unordered_map<u32, WmoInstance> m_dynamicWmos;    // phase 8: moving buildings (transports), by instance
    std::vector<u32> m_pendingSpawns; // uniqueIds of buildings whose doodads are still being placed
    std::vector<u8> m_modelEffects;   // per model entry: 1 = has particle emitters (parallel to m_modelDrawable)
    std::vector<u32> m_visibleEffects; // instances, last collect()
    std::vector<u32> m_drawModels;     // model entry of each draw of the last collect()
    // Phase 14: the animated doodads' poses; per instance (parallel to m_spheres) the radius 1.12's doodad distance
    // rule reads (set with the sphere) and its fade in the last collect().
    DoodadAnimator m_animator;
    std::vector<u32> m_drawPoses; // per draw of the last collect(): its first instance's pose (kNone: as baked)
    // Phase 18 (world): per instance (parallel to m_spheres), the building whose doodad it is and its index among the
    // building's doodads (kNone: a doodad of the map); the camera's place and the walk through its portals.
    std::vector<u32> m_instanceOwner;
    std::vector<u32> m_instanceDoodad;
    // Whether a doodad of the camera's building is seen: one of the groups listing it was reached (or, while a portal
    // leads out, is an outdoor group).
    bool doodadSeen(const ModelEntry& building, u32 doodad) const;
    Location m_cameraLocation;
    WmoPortalWalk m_walk;
    bool m_outdoorsVisible = true;
    std::array<u16, 2> m_walkStart{};
    TerrainQuery m_terrainQuery = nullptr;
    const void* m_terrainContext = nullptr;
    std::optional<f32> terrainBelow(const glm::dvec3& p) const {
        return m_terrainQuery ? m_terrainQuery(m_terrainContext, p.x, p.y, p.z) : std::nullopt;
    }
    bool locatesByBsp(const ModelEntry& entry) const { return portalCulling && !entry.bspGroups.empty(); }
    std::vector<f32> m_classRadius;
    std::vector<f32> m_instanceFade;
    // Per instance, its range's squared limits for m_fadeScale and m_fadeFar (updateFadeLimits()).
    static constexpr f32 kLargestDoodadRange = 200.0f; // world/DoodadFade.h: the far clip only shortens this one
    std::vector<glm::vec2> m_fadeLimits;
    f32 m_fadeScale = 1.0f;
    f32 m_fadeFar = kLargestDoodadRange;
    void updateFadeLimits(u32 instance);
    glm::dvec3 m_lastCamera{0.0};
    u32 m_traceFrames = 0;
    u32 m_nextSerial = 1;
    ObjectStats m_stats;
    glm::dvec3 m_interiorPoint{1e30}; // insideInterior()'s last answer by the boxes
    InteriorPlace m_interiorPlace;     // phase 15: ...and its building (also standingAt()'s)
    bool m_interiorInside = false;
    u32 m_interiorAge = 0;
    glm::dvec3 m_standingFeet{1e30}; // standingAt()'s last answer
    Standing m_standing;
    u32 m_standingAge = 0;
    bool anyBsp() const; // a known building has its BSP (models.vdpk v8)
    glm::dvec3 m_fogPoint{1e30}; // fogAt()'s last answer
    bool m_fogUnderwater = false;
    std::optional<FogSample> m_fog;
    u32 m_fogAge = 0;
};

} // namespace vd
