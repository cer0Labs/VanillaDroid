#pragma once

#include "anim/BoneMatrix.h"
#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackFormat.h"
#include "platform/NativeSurface.h"
#include "platform/PlatformInfo.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <glm/vec4.hpp>

#include <array>
#include <memory>
#include <span>

namespace vd {

// Host-visible upload memory that any thread may fill with sequential writes (e.g. memcpy on the IO
// thread). Do not read it back on the CPU: on mobile GPUs it behaves like uncached memory.
struct StagingBlock {
    u32 id = 0; // 0 = invalid
    u8* data = nullptr;
    u64 size = 0;
    bool valid() const { return id != 0; }
};

struct TerrainTileHandle {
    u32 index = 0; // 0 = invalid
    bool valid() const { return index != 0; }
};

// One terrain tile to draw this frame (world axes: X north, Y west, Z up).
struct TerrainDraw {
    TerrainTileHandle tile;
    glm::vec3 origin{0.0f}; // tile north-west corner minus camera position
    u32 lod = 0;            // pack::kLodStep level (0 = full detail)
};

struct ModelHandle {
    u32 index = 0; // 0 = invalid
    bool valid() const { return index != 0; }
};

// Model -> world transform of one instance as three rows of a 3x4 matrix, relative to the camera
// (translation = position - camera). Layout shared with shaders/model.vert.
struct ModelInstanceData {
    f32 rows[12];
    u32 light[2]{}; // phase 11: the interior light it carries (world/InteriorLighting.h), 0 = the outdoor light
    // Phase 14 (doodads): an animated doodad's first matrix in the frame's doodad palette (ModelDraw::kDoodadSkinned),
    // and its opacity in the distance fade (1: none; below 1 only in draws flagged ModelDraw::kDoodadFading).
    u32 boneBase = 0;
    f32 fade = 1.0f;
};

// A model drawn with instances [firstInstance, firstInstance + instanceCount) of FrameParams::modelInstances.
// groupCount > 0 restricts it to the groups listed in FrameParams::modelGroups[firstGroup..] (WMO group
// culling); 0 draws every group.
struct ModelDraw {
    static constexpr u32 kNoUvTransforms = ~0u;
    ModelHandle model;
    u32 firstInstance = 0;
    u32 instanceCount = 0;
    u32 firstGroup = 0;
    u32 groupCount = 0;
    // Its texture animations this frame: FrameParams::uvTransforms[firstUvTransform + i] for the model's i-th UV
    // animation (pack::ModelMaterial::uvAnimation - 1); kNoUvTransforms = none animated.
    u32 firstUvTransform = kNoUvTransforms;
    // Phase 11: an M2's own point light this frame (a brazier's fire on its bowl): model-space position and
    // attenuation end, color times intensity and attenuation start. All zero: none.
    std::array<f32, 8> selfLight{};
    // Phase 14 (doodads): kDoodadSkinned, an M2 drawn with its skeleton (its instances' bones in the frame's doodad
    // palette, Renderer::doodadBoneMatrices()); kDoodadFading, some of its instances fade with the distance (their
    // fade < 1): the opaque parts dither them out, the blended ones scale their opacity.
    static constexpr u32 kDoodadSkinned = 1u << 0;
    static constexpr u32 kDoodadFading = 1u << 1;
    u32 doodadFlags = 0;
};

// Per-instance data of an animated actor. Layout shared with shaders/actor.vert.
struct ActorInstanceData {
    f32 rows[12];     // model -> world, translation relative to the camera (as ModelInstanceData)
    u32 boneBase = 0; // first of its matrices in the frame's bone palette (Renderer::boneMatrices())
    u32 light[2]{};   // phase 11: the interior light it carries (world/InteriorLighting.h), 0 = the outdoor light
    u32 reserved = 0;
};

// Textures an actor gives the materials of its model that take them from the instance (pack::ActorTextureSlot:
// 1 body skin, 2 cape / item skin, 3 hair, 4 skin extra, 5-7 creature skins). Slot 0 is unused.
constexpr u32 kActorTextureSlots = 8;

struct TextureHandle {
    u32 index = 0; // 0 = invalid
    bool valid() const { return index != 0; }
};

// The visible parts of an actor model packed into one index range per material (see Renderer::createActorMesh).
struct ActorMeshHandle {
    u32 index = 0; // 0 = invalid
    bool valid() const { return index != 0; }
};
struct ActorMeshRun {
    u32 firstIndex; // into the mesh's own indices
    u32 indexCount;
    u32 material;   // of the model
};

// An actor model drawn with instances [firstInstance, firstInstance + instanceCount) of FrameParams::actorInstances,
// which share its textures and visible geosets. geosetCount > 0 restricts it to the geoset indices (into the model's
// sorted geoset table) listed in FrameParams::actorGeosets[firstGeoset..]; 0 draws every part. firstColor: its
// materials' colors (RGBA per material, FrameParams::actorColors[firstColor + material]); kNoColors = the model's own.
struct ActorDraw {
    static constexpr u32 kNoColors = ~0u;
    ModelHandle model;
    u32 firstInstance = 0;
    u32 instanceCount = 0;
    u32 firstGeoset = 0;
    u32 geosetCount = 0;
    u32 firstColor = kNoColors;
    std::array<TextureHandle, kActorTextureSlots> textures{};
    ActorMeshHandle mesh; // valid: draw its runs instead of the model's parts (geosets are already chosen)
    // Texture animations, as ModelDraw::firstUvTransform (pack::ActorMaterial::uvAnimation - 1).
    u32 firstUvTransform = ModelDraw::kNoUvTransforms;
    // Phase 11: a unit's look from its auras (game/UnitFade: a ghost, stealth, invisibility): rgb multiplies its
    // colors, a < 1 draws all of it blended (like the client's M2 opaque batches switched to alpha blending) after
    // the opaque world. The common case, (1, 1, 1, 1), costs nothing.
    glm::vec4 fade{1.0f};
};

// One layer of a composed texture (a character's skin, clothes and hair on one atlas): `texture` stretched over the
// rectangle, in pixels of the target. The first layer is copied, the rest are alpha blended over it.
struct ComposeLayer {
    TextureHandle texture;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
};

// One distant horizon tile (WDL) to draw this frame. Layout shared with shaders/horizon.vert.
struct HorizonDraw {
    glm::vec3 origin{0.0f}; // tile north-west corner minus camera position
    u32 slot = 0;           // tile in the map's horizon heights
};

// Outdoor light for the frame (from Light.dbc, see world/Lighting.h). Gamma-space colors.
struct SceneLighting {
    glm::vec3 lightDirection{0.3f, 0.4f, 0.87f}; // unit vector towards the sun (or the moon)
    glm::vec3 directColor{0.85f, 0.8f, 0.7f};
    glm::vec3 ambientColor{0.4f, 0.42f, 0.48f};
    glm::vec3 fogColor{0.55f, 0.68f, 0.85f}; // also the clear color
    f32 fogStart = 300.0f;
    f32 fogEnd = 700.0f;
    std::array<glm::vec3, 5> sky{glm::vec3(0.2f, 0.35f, 0.6f), glm::vec3(0.3f, 0.5f, 0.75f),
                                 glm::vec3(0.45f, 0.6f, 0.8f), glm::vec3(0.55f, 0.68f, 0.85f),
                                 glm::vec3(0.6f, 0.7f, 0.85f)}; // zenith -> horizon
    glm::vec3 riverClose{0.1f, 0.25f, 0.2f};
    glm::vec3 riverFar{0.1f, 0.2f, 0.25f};
    glm::vec3 oceanClose{0.1f, 0.25f, 0.3f};
    glm::vec3 oceanFar{0.0f, 0.1f, 0.15f};
    glm::vec4 waterAlphas{0.5f, 1.0f, 0.75f, 1.0f}; // river shallow/deep, ocean shallow/deep
    // Phase 11: the rest of the bands (sky, sun, clouds) and what selected them (world/LightConditions).
    glm::vec3 shadowColor{0.3f};
    glm::vec3 sunColor{1.0f};
    glm::vec3 sunHalo{1.0f};
    glm::vec3 cloudEmissive{0.0f};
    glm::vec3 cloudLayer1{0.0f};
    glm::vec3 cloudLayer2{0.0f};
    f32 celestialGlow = 1.0f;
    f32 cloudDensity = 0.5f;
    f32 glow = 0.5f;          // LightParams.glow
    u32 skybox = 0;           // LightSkybox.dbc id
    bool highlightSky = false;
    f32 dayFraction = 0.5f;   // time of day, 0..1 from midnight
    f32 storm = 0.0f;         // weather intensity 0..1
    bool underwater = false;  // the camera is under a liquid's surface
    bool dead = false;        // the player is a ghost
    // Phase 11: the client's full-screen effects (render/ScreenFx.h, shaders/screenfx.glsl) this frame: the glow's
    // weight (0 = off) and the ghost's grading (0 or 1).
    f32 screenGlow = 0.0f;
    f32 screenDeath = 0.0f;
    // Phase 11 (f11/interiors): the night glow of WMO windows and lamps (0..1) and the specular highlights of the
    // terrain and water (TERRAIN_HIGHLIGHTS).
    f32 nightGlow = 0.0f;
    bool terrainHighlights = true;
};

// --- Sky (phase 11) -------------------------------------------------------------------------------------------

struct SkyModelHandle {
    u32 index = 0; // 0 = invalid
    bool valid() const { return index != 0; }
};

// A quad facing the screen in the sky (the sun, a moon, a glare), camera-relative: its corners are
// center +- right +- up.
struct SkySprite {
    TextureHandle texture;
    glm::vec3 center{0.0f};
    glm::vec3 right{0.0f}; // half the quad along the screen's right, world axes
    glm::vec3 up{0.0f};    // half the quad along the screen's up
    glm::vec4 color{1.0f}; // RGB, A = opacity
};

// A sky model (Renderer::createSkyModel) around the camera.
struct SkyModelDraw {
    SkyModelHandle model;                     // invalid: none
    glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion (x, y, z, w), model -> world
    f32 alpha = 1.0f;
};

constexpr u32 kMaxSkyBodies = 3; // the sun, the White Lady, the Blue Child
constexpr u32 kMaxSkyGlares = 2; // the sun's, the White Lady's

// The sky of a frame (world/Sky), drawn where nothing else is, at the far plane: the dome, the stars, the sun
// and the moons added over it, the clouds blended over them, then a skybox model; the glares go over the whole
// scene.
struct SkyFrame {
    bool dome = true;   // the dome and everything on it; false while an opaque skybox replaces the sky
    SkyModelDraw stars; // added over the dome
    // The moons' discs, which hide the stars behind them (in 1.12 the moons are blended over the stars before
    // the dome is added): xyz = unit direction, w = cosine of the disc's angular radius (0 = none).
    std::array<glm::vec4, 2> starMasks{};
    std::array<SkySprite, kMaxSkyBodies> bodies{};
    u32 bodyCount = 0;
    TextureHandle clouds; // the clouds' noise (RGBA: four fields); invalid = no clouds
    glm::vec4 cloudDrift{0.0f}; // xy: offset of the first layer, zw: of the second (texture units)
    glm::vec4 cloudShape{0.0f}; // x: cover threshold (1 - density), y: sharpness, z: light strength, w: storm
    glm::vec3 cloudLight{0.0f, 0.0f, 1.0f}; // unit, towards the sun or the moon that lights the clouds
    SkyModelDraw skybox; // LightSkybox.dbc (the ghost's DeathClouds) or the WMO the camera is in (MOSB)
    std::array<SkySprite, kMaxSkyGlares> glares{};
    u32 glareCount = 0;
};

// --- Touch UI (phase 7) ---------------------------------------------------------------------------------------------

// How a UI quad is shaded (UiVertex::params bits 12-15). Layout shared with shaders/ui.frag.
enum class UiMode : u32 {
    Atlas = 0,         // a sprite of the UI atlas (layer = page)
    Icon = 1,          // an icon of the icon cache (layer = slot)
    Text = 2,          // a glyph's distance field (layer = font atlas layer); bits 16-23: outline width
    Texture = 3,       // the draw command's texture
    TextureCircle = 4, // the draw command's texture inside its circle (the minimap)
    Solid = 5,         // the vertex color alone
    // Phase 20 (the touch cluster's round buttons):
    IconCircle = 6, // an icon of the icon cache inside a circle: bits 16-23 its radius in the icon (255 = 0.5)
    Ring = 7,       // a disc or ring over the quad (uv 0..1): bits 16-23 the share of the turn drawn, ending at twelve
                    // o'clock clockwise (255: all), bits 25-31 the hole's share of the radius (0: a disc)
    // Retouches (windows): the client's SetDesaturated (Shaders\Pixel\Desaturate.bls). Numbered from the top
    // so the modes other branches add from 8 up do not collide.
    AtlasDesaturated = 14, // Atlas, its texel's luminance (only the vertex colour's alpha applies)
    IconDesaturated = 15,  // Icon, the same
};

// One corner of a UI quad in logical pixels (as the player sees the screen, y down). Layout shared with
// shaders/ui.vert. Quads are 4 vertices: top-left, top-right, bottom-right, bottom-left.
struct UiVertex {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 u = 0.0f; // normalized, in the atlas page / icon / font layer / texture
    f32 v = 0.0f;
    u32 color = 0xFFFFFFFFu; // RGBA8, red in the low byte
    u32 params = 0; // bits 0-11 layer, 12-15 UiMode, 16-23 text outline (255 = half the distance range), 24 additive,
                    // 25-31 a ring's hole (phase 20)
};
static_assert(sizeof(UiVertex) == 24, "UiVertex layout");

// Quads [firstQuad, firstQuad + quadCount) of FrameParams::uiVertices, clipped to the rectangle (logical pixels;
// clipWidth <= 0: the whole screen). `texture` feeds UiMode::Texture / TextureCircle quads.
struct UiDrawCmd {
    u32 firstQuad = 0;
    u32 quadCount = 0;
    TextureHandle texture;
    f32 clipX = 0.0f, clipY = 0.0f, clipWidth = 0.0f, clipHeight = 0.0f;
    f32 circleX = 0.0f, circleY = 0.0f, circleRadius = 0.0f; // UiMode::TextureCircle
};

// Icon cache slots of the UI (64x64 icons, one array layer each).
constexpr u32 kUiIconSlots = 256;
constexpr u32 kUiIconSize = 64;

// --- Effects (phase 9) ----------------------------------------------------------------------------------------------

// Texture coordinate animation of a material this frame (water, lava, energy fields): u' = dot(rows[0..2], (u, v, 1)),
// v' = dot(rows[4..6], (u, v, 1)). Layout shared with shaders/model_push.glsl (two vec4).
struct UvTransform {
    f32 rows[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
};

// One corner of a particle or ribbon quad: camera-relative position, texture coordinates (unorm16), color (RGBA8,
// red in the low byte; alpha = opacity). Quads are 4 vertices: (-x, +y), (-x, -y), (+x, +y), (+x, -y) of the quad,
// drawn as two triangles. Layout shared with shaders/particle.vert.
struct ParticleVertex {
    f32 position[3];
    u16 uv[2];
    u32 color;
};
static_assert(sizeof(ParticleVertex) == 20, "ParticleVertex layout");

// ParticleDraw::flags.
constexpr u8 kParticleDrawUnfogged = 1u << 0; // the fog does not fade it
constexpr u8 kParticleDrawLit = 1u << 1;      // blended (not additive) particles darken with the scene's light
constexpr u8 kParticleDrawShadow = 1u << 2;   // phase 14 (bodyfx): Mod blend faded towards white by the vertex alpha

// Quads [firstQuad, firstQuad + quadCount) of the frame's particle vertices (Renderer::particleVertices()) with one
// texture and blend mode (pack::ModelBlend). Drawn in order after every other transparent surface.
struct ParticleDraw {
    TextureHandle texture;
    u8 blend = 0;
    u8 flags = 0;
    u32 firstQuad = 0;
    u32 quadCount = 0;
};

// --- Weather (phase 11) ---------------------------------------------------------------------------------------------

// The client's weather textures (textures\Weather\*), given once with Renderer::setWeatherTextures.
enum class WeatherTexture : u32 { RainDrop, RainSplash, SnowFlake, SnowMist, SandGrain, Count };
constexpr u32 kWeatherTextures = static_cast<u32>(WeatherTexture::Count);
// The ground the drops and flakes land on: kWeatherGroundSide x kWeatherGroundSide world points around the camera,
// stored by world point modulo the side (world/WeatherEffect).
constexpr u32 kWeatherGroundSide = 32;

// One kind of weather particle (shaders/weather.glsl). Each is a seed: it lives `life` (plus a random part), then
// starts again somewhere else in a box that follows the camera, moving at the shared velocity plus a random one of
// its own. std140, shared with the shaders.
struct WeatherParticles {
    glm::vec4 box{0.0f};      // xyz: the box they wrap in around the camera (yards); w: its bottom under the camera
    glm::vec4 phase{0.0f};    // xyz: the camera's world position modulo the box
    glm::vec4 velocity{0.0f}; // xyz: shared (yd/s, wind included); w: random horizontal speed of each (yd/s)
    glm::vec4 shape{0.0f};    // x: random vertical speed (yd/s), y: size (yd), z: smallest size (radians), w: opacity
    glm::vec4 fades{0.0f};    // x: fade in, y: fade out before the end, z: fade once landed (0: never lands) (s);
                              // w: height over the ground (mists), 1 = rounded off (grains of sand)
    glm::vec4 color{1.0f};    // rgb: lit color; a: largest size (radians, 0: none)
    glm::uvec4 life{0u};      // x: count, y: life, z: random extra life (both 0.1 ms), w: seed
};

// The frame's weather (shaders/weather.glsl, std140): what kind, the particles, their mist and the ground.
struct WeatherUniforms {
    glm::uvec4 time{0u};         // x: clock (0.1 ms, wraps), y: kind (game::WeatherType; 0 = nothing to draw)
    glm::vec4 cameraRight{0.0f}; // world; w: the splashes' opacity
    glm::vec4 cameraUp{0.0f};    // world; w: the camera's world z
    glm::vec4 rain{0.0f};        // x: half width of a drop, y: its tail, z: splash size (yd), w: splash time (s)
    glm::vec4 blur[3]{};         // rows of the rotation that turns the drops' fall towards their motion as seen by
                                 // the moving camera (MapWeather's c_motionBlurRotateMtx)
    glm::vec4 ground{0.0f};      // xy: the camera's world position modulo the ground's span, z: point spacing (yd),
                                 // w: how far from the camera the ground is known (yd)
    WeatherParticles particles;
    WeatherParticles mist;
    glm::vec4 heights[kWeatherGroundSide * kWeatherGroundSide / 4]{}; // world z; -1e4 where nothing is known
};
static_assert(sizeof(WeatherParticles) == 112, "WeatherParticles layout");
static_assert(sizeof(WeatherUniforms) == 4448, "WeatherUniforms layout (std140 of shaders/weather.glsl)");

// --- Ground effects (phase 14) --------------------------------------------------------------------------------------

// One detail doodad of the terrain (world/GroundEffects): grass, flowers, small bushes and pebbles. Layout shared
// with shaders/grass.vert.
struct GrassInstance {
    f32 position[3]; // its base, relative to the camera (world/GroundEffects keeps x, y from its chunk's corner)
    f32 rotation[2]; // cosine and sine of its turn about Z, times its scale
    i8 normal[4];    // snorm8: xyz the terrain's normal under it, w its light (127 lit, 96 in the chunk's shadow)
};
static_assert(sizeof(GrassInstance) == 24, "GrassInstance layout");

// One model of the grass models (Renderer::setGrassModels) drawn with instances [firstInstance, firstInstance +
// instanceCount) of FrameParams::grassInstances.
struct GrassDraw {
    u32 firstIndex = 0; // the model's triangles in the grass index buffer, over its vertices from vertexOffset
    u32 indexCount = 0;
    u32 vertexOffset = 0;
    u32 firstInstance = 0;
    u32 instanceCount = 0;
    TextureHandle texture;
    bool clamp = false; // its texture does not wrap
};

// --- Portraits of the unit frames (parity) --------------------------------------------------------------------------

// Square images the unit frames show (the client's SetPortraitTexture): each is a texture of its own for the
// UI (Renderer::portraitTexture), rendered only when asked (PortraitDraw), at most kMaxPortraitRenders a
// frame.
constexpr u32 kPortraitSlots = 16;
constexpr u32 kPortraitSize = 256;
constexpr u32 kMaxPortraitRenders = 2;

// One portrait rendered this frame: FrameParams::portraitActors[firstActor, firstActor + actorCount) — a
// unit's model and what it carries, whose instances, geosets, colors, texture animations and bones are in the
// frame's actor arrays like the world's — seen by its own camera over a black background, without fog, lit by
// one key light.
struct PortraitDraw {
    u32 slot = 0;
    u32 firstActor = 0;
    u32 actorCount = 0;
    // Camera-relative positions -> Vulkan clip space (y down, depth 0..1), no pre-rotation.
    glm::mat4 viewProj{1.0f};
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f}; // unit vector towards the light
    glm::vec3 directColor{1.0f};
    glm::vec3 ambientColor{0.4f};
};

// Phase 15: the interface's 3D model frames (the character sheet, the dressing room, the pet's page, the stable,
// inspect, the tabard designer: app/ModelViews): images with a transparent background rendered every frame they are
// shown, each a texture of its own for the UI (Renderer::modelViewTexture). A PortraitDraw each
// (FrameParams::modelViews), its actors in FrameParams::portraitActors with the portraits'.
constexpr u32 kModelViewSlots = 4;
constexpr u32 kModelViewWidth = 512;
constexpr u32 kModelViewHeight = 640;

struct FrameParams {
    f32 timeSeconds = 0.0f;
    // Camera-relative view * projection (Vulkan clip space: y down, depth 0..1). World positions reach
    // the GPU relative to the camera (TerrainDraw::origin), so this has no translation. The renderer
    // appends the swapchain pre-rotation.
    glm::mat4 viewProj{1.0f};
    SceneLighting lighting;
    std::span<const TerrainDraw> terrain;
    std::span<const HorizonDraw> horizon;
    std::span<const ModelDraw> models;
    std::span<const ModelInstanceData> modelInstances;
    std::span<const u16> modelGroups;
    // Animated actors (characters, creatures and what they carry). Their bone matrices were written straight into
    // Renderer::boneMatrices(); boneCount says how many.
    std::span<const ActorDraw> actors;
    std::span<const ActorInstanceData> actorInstances;
    std::span<const u16> actorGeosets;
    std::span<const glm::vec4> actorColors;
    u32 boneCount = 0;
    // Phase 9: texture animations of models and actors (see ModelDraw::firstUvTransform), and the particles and
    // ribbons, whose quads were written straight into Renderer::particleVertices(): the first particleQuads.
    std::span<const UvTransform> uvTransforms;      // of the models (ModelDraw::firstUvTransform)
    std::span<const UvTransform> actorUvTransforms; // of the actors (ActorDraw::firstUvTransform)
    // Phase 14 (doodads): the matrices of the animated doodads written into Renderer::doodadBoneMatrices().
    u32 doodadBoneCount = 0;
    std::span<const ParticleDraw> particles;
    u32 particleQuads = 0;
    // The touch UI over the scene (before the debug overlay).
    std::span<const UiVertex> uiVertices;
    std::span<const UiDrawCmd> uiCommands;
    // Portraits to render this frame (at most kMaxPortraitRenders), before the scene: the UI shows them at once.
    std::span<const PortraitDraw> portraits;
    std::span<const ActorDraw> portraitActors;
    // Phase 15: the model frames shown this frame (PortraitDraw::slot = a model view slot; actors in portraitActors).
    std::span<const PortraitDraw> modelViews;
    // Phase 11: rain, snow or sand around the camera (null: none), drawn after the other transparent surfaces.
    const WeatherUniforms* weather = nullptr;
    // Phase 11: the sky's moving parts (world/Sky).
    SkyFrame sky;
    // Phase 14 (grass): the terrain's detail doodads, alpha blended and tested after the opaque world, faded out by the
    // view depth from grassFadeStart to grassFadeEnd yards like the 1.12 client (its alpha ramp).
    std::span<const GrassDraw> grass;
    std::span<const GrassInstance> grassInstances;
    f32 grassFadeStart = 52.5f;
    f32 grassFadeEnd = 70.0f;
    // Phase 14 (bodyfx): quads of the particle buffer (within the first particleQuads) drawn right after the opaque
    // world and the sky, before the translucent water and models: the units' blob shadows and footprints.
    std::span<const ParticleDraw> groundDecals;
};

struct RenderStats {
    f32 gpuFrameMs = 0.0f;  // GPU time of the most recently completed frame
    f32 blockedMs = 0.0f;   // CPU time spent waiting on the GPU/display last frame (fence, acquire, present)
    u32 drawCalls = 0;
    u32 triangles = 0;
    u32 wmoDrawCalls = 0; // included in the totals above
    u32 wmoTriangles = 0;
    u32 m2DrawCalls = 0;
    u32 m2Triangles = 0;
    u32 actorDrawCalls = 0;
    u32 actorTriangles = 0;
    u32 uiDrawCalls = 0; // included in the totals above
    u32 uiQuads = 0;
    u32 particleDrawCalls = 0; // included in the totals above
    u32 particleQuads = 0;
    u32 weatherDrawCalls = 0; // phase 11, included in the totals above
    u32 weatherVertices = 0;
    u32 width = 0;          // logical (as seen by the player) render size
    u32 height = 0;
    u32 rotationDegrees = 0; // surface pre-rotation applied by the renderer
    u32 portraitRenders = 0; // portraits rendered this frame (their draws are in the totals above)
    f32 portraitGpuMs = 0.0f; // GPU time of the last frame that rendered portraits
    u32 modelViewRenders = 0;  // phase 15: model frames rendered this frame (their draws are in the totals above)
    f32 modelViewGpuMs = 0.0f; // GPU time of the last frame that rendered model frames
    // Phase 10: the world's resolution (Renderer::setRenderScale) in the last frame, and its size in pixels (the
    // display's native orientation). The UI is always drawn at the full width x height.
    f32 renderScale = 1.0f;
    u32 sceneWidth = 0;
    u32 sceneHeight = 0;
    u32 frameInterval = 1; // display frames of 60 fps per presented frame (2 = 30 fps)
    u32 grassDrawCalls = 0; // phase 14, included in the totals above
    u32 grassInstances = 0;
    u32 grassTriangles = 0;
};

struct TextureInfo {
    u32 width = 0;
    u32 height = 0;
    u32 mipCount = 0;
    u64 gpuBytes = 0;
    bool nativeAstc = false; // false: decoded to RGBA8 on the CPU (GPU without ASTC)
    f32 uploadMs = 0.0f;
};

// Rendering facade. No Vulkan types leak out of here, so game code never depends on the graphics API.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Device-level setup: done once, survives surface loss.
    Result<void> init(const PlatformInfo& platform);
    void shutdown();

    Result<void> attachSurface(const NativeSurface& surface);
    void detachSurface();
    // Size and/or orientation may have changed; the swapchain is rebuilt lazily if needed.
    void onSurfaceChanged(u32 width, u32 height);

    // Textures from pack blobs (pack::TextureHeader + mips). destroyTexture() is deferred until the GPU
    // can no longer be using the texture.
    // debugUiVisible: also register it for ImGui::Image (uses a descriptor from a small pool).
    Result<TextureHandle> createTexture(const u8* blob, usize size, TextureInfo* info = nullptr,
                                        bool debugUiVisible = true);
    void destroyTexture(TextureHandle handle);

    // Terrain of the current map. Phase 18 (world): nothing here waits for the GPU (a map opens while the loading
    // screen animates). clearTerrainMap retires the map's texture arrays, shared indices and horizon: freed once the
    // frames in flight are done with them; the terrain is not drawn until the next map's textures are set.
    void clearTerrainMap();
    // The index buffer shared by tiles without holes (sharedLod: first/count of each LOD level, pack::LodRange pairs)
    // and the distant horizon (WDL: i16 height pairs as u32 words and the tile mesh indices; empty: none), copied
    // through the upload ring into the next frame.
    Result<void> setTerrainGeometry(std::span<const u16> sharedIndices, std::span<const u32> sharedLod,
                                    std::span<const u32> horizonHeights, std::span<const u16> horizonIndices);
    // The map's terrain layer array and, with `liquids`, the liquid frames (kept from map to map otherwise): texels an
    // IO thread wrote into `block` at the given offsets, laid out as the headers say (mip offsets from there), already
    // RGBA8 where !nativeAstc(). The copies are recorded into the next frame: `block` must stay untouched until that
    // frame completes (retireStagingBlock()).
    Result<void> setTerrainTextures(const StagingBlock& block, u64 terrainOffset, const pack::TextureHeader& terrain,
                                    u64 liquidOffset, const pack::TextureHeader* liquids);
    // The GPU samples ASTC (false: blobs are decoded to RGBA8 on the CPU, desktop development only).
    bool nativeAstc() const;
    // Uploads a TerrainTile blob without stalling (the transfer is recorded into the next frame). Returns
    // an invalid handle, not an error, when this frame's upload budget is used up: retry next frame.
    Result<TerrainTileHandle> createTerrainTile(const u8* blob, usize size);
    // Same for a blob stored at the start of a staging block: the GPU copies it from there (no CPU
    // copy). The block must stay untouched until completedFrames() >= frameNumber() + 1, both read
    // right after this call.
    Result<TerrainTileHandle> createTerrainTile(const StagingBlock& block, usize size);
    void destroyTerrainTile(TerrainTileHandle handle);
    u32 terrainTileCount() const;

    // Streamed assets: `block` holds the baked blob at `offset` (written by an IO thread), `blob` is a CPU
    // copy of the same bytes. The GPU copies are recorded into the next frame; nothing stalls.
    Result<TextureHandle> createTexture(const StagingBlock& block, u64 offset, const u8* blob, usize size);
    Result<ModelHandle> createModel(const StagingBlock& block, u64 offset, const u8* blob, usize size);
    // Material textures arrive after the model; until then a white placeholder is used.
    void setModelTexture(ModelHandle model, u32 material, TextureHandle texture);
    void destroyModel(ModelHandle model);
    // Animated actor model (pack::ActorModelHeader blob), same streaming rules as createModel. Materials whose
    // texture comes from a file get it through setModelTexture; the rest take it from each ActorDraw.
    Result<ModelHandle> createActorModel(const StagingBlock& block, u64 offset, const u8* blob, usize size);
    // A model's visible parts as one index range per material: `indices` index the model's vertices, `runs` split
    // them by material (in draw order). An actor wearing a few of its model's geosets then draws once per material
    // instead of once per geoset. Uploaded without stalling; destroyed like models (deferred).
    Result<ActorMeshHandle> createActorMesh(ModelHandle model, std::span<const u16> indices,
                                            std::span<const ActorMeshRun> runs);
    void destroyActorMesh(ActorMeshHandle mesh);
    // A texture composed on the GPU from `layers` (see ComposeLayer), with mips, RGBA8. Recorded into the next
    // frame before anything draws, so it can be used right away. The layer textures must exist and may be
    // destroyed right after this call.
    Result<TextureHandle> composeTexture(u32 width, u32 height, std::span<const ComposeLayer> layers);
    // Bone palette of the frame being prepared: write-only mapped memory (never read it back), valid until
    // renderFrame(). Fill a prefix and pass its length in FrameParams::boneCount. Safe to fill from worker threads
    // (disjoint ranges).
    std::span<anim::BoneMatrix> boneMatrices();
    // Phase 14 (doodads): the animated doodads' bone palette of the frame being prepared, like boneMatrices(): fill a
    // prefix and pass its length in FrameParams::doodadBoneCount.
    std::span<anim::BoneMatrix> doodadBoneMatrices();
    // Particle and ribbon quads of the frame being prepared (4 vertices each): write-only mapped memory, valid until
    // renderFrame(), like boneMatrices(). Fill a prefix and give its length in FrameParams::particleQuads.
    std::span<ParticleVertex> particleVertices();
    u32 modelCount() const;
    u64 modelGpuBytes() const;

    // Touch UI textures from ui.vdpk: the interface sprite atlas and the font distance fields (both 2D arrays).
    // Waits for the GPU to go idle (done once, while loading).
    Result<void> setUiTextures(const u8* atlas, usize atlasSize, const u8* fonts, usize fontsSize);
    // An icon (pack texture blob of kUiIconSize square, ASTC 4x4 like every Interface\Icons texture) into cache
    // slot `slot`, without stalling: the copy from `block` (blob at `offset`, written by an IO thread) is recorded
    // into the next frame; the block must stay untouched until that frame completes. False if the blob is not an
    // icon.
    bool setUiIcon(u32 slot, const StagingBlock& block, u64 offset, const u8* blob, usize size);
    u32 uiQuadCapacity() const;

    // Phase 11: the weather's textures, one pack texture blob per WeatherTexture (in order; an empty one leaves a
    // white placeholder). Waits for the GPU to go idle (once, while loading).
    Result<void> setWeatherTextures(std::span<const std::span<const u8>> blobs);

    // Phase 14 (grass): the detail doodads' shared geometry, from the grass models blob (pack::GrassModelsHeader) at
    // `offset` in `block` (written by an IO thread; `blob` is a CPU copy): their vertices and indices are copied by
    // the GPU in the next frame, so the block must stay untouched until that frame completes. Once per pack: a second
    // call is refused (the buffers live until shutdown).
    Result<void> setGrassModels(const StagingBlock& block, u64 offset, const u8* blob, usize size);

    // A portrait slot's image for the UI (DrawList::portrait): what the last PortraitDraw of that slot
    // rendered. Invalid before the first surface; the images are lost (and the generation changes) when the
    // swapchain's format does, so their owner renders them again.
    TextureHandle portraitTexture(u32 slot) const;
    u32 portraitGeneration() const;
    // Phase 15: a model view slot's image (premultiplied alpha, transparent around the model): what the last
    // FrameParams::modelViews draw of that slot rendered. Like the portraits', lost when the generation changes.
    TextureHandle modelViewTexture(u32 slot) const;
    u32 modelViewGeneration() const;

    // Phase 11: a sky model (pack::SkyModelHeader blob) and the texture of each of its parts (invalid: white).
    // Written once into mapped memory; lives until shutdown (there are a handful, loaded once).
    Result<SkyModelHandle> createSkyModel(const u8* blob, usize size,
                                          std::span<const TextureHandle> partTextures);

    Result<StagingBlock> createStagingBlock(u64 size);
    // Waits for the GPU to go idle: for teardown, not for use during gameplay.
    void destroyStagingBlock(StagingBlock& block);
    // Phase 18 (world): the same during gameplay: freed once the frames submitted so far have completed.
    void retireStagingBlock(StagingBlock& block);
    // Frames submitted so far / frames the GPU is known to have finished (both count from 1).
    u64 frameNumber() const;
    u64 completedFrames() const;
    u64 terrainGpuBytes() const;
    // Phase 10: all the GPU memory the renderer holds (VMA's blocks in every heap). Not for every frame.
    u64 gpuMemoryBytes() const;
    // Phase 10: frees at once what destroy*() left for the frames in flight (waits for the GPU to go idle). For the
    // background, where no frame comes to release it.
    void releaseRetired();

    // Debug UI (Dear ImGui). Returns true if ImGui:: calls may be issued for this frame; whatever is
    // submitted is drawn on top of the scene by the next renderFrame(). Development overlay only.
    bool beginDebugUi(f32 deltaSeconds);
    // Primary pointer (mouse or first finger) in logical pixels, for the debug UI.
    void debugUiPointer(f32 x, f32 y, bool down, bool touch);
    void debugUiWheel(f32 steps);
    // ImTextureID for ImGui::Image, 0 if unavailable.
    u64 debugUiTextureId(TextureHandle handle) const;
    // True if (x, y) in logical pixels lies over a debug window that accepts input.
    bool debugUiHitTest(f32 x, f32 y) const;
    // True while ImGui uses the mouse (hover/drag over a window) or wants text input.
    bool debugUiWantsMouse() const;

    // Records, submits and presents one frame. Returns false if nothing was presented.
    bool renderFrame(const FrameParams& params);

    // Phase 10: the world's share of the screen's resolution, 0.5..1 (whole percents), from the next frame. Below 1
    // the world is drawn smaller and stretched to the screen under the UI, which keeps its full resolution;
    // changing it reallocates nothing.
    void setRenderScale(f32 scale);
    f32 renderScale() const;
    // Phase 18 (world): the video options' anisotropic filtering of the world's textures (1, 2, 4, 8, 16, capped by the
    // GPU) and multisampling of the world and the interface (1, 2, 4, 8, capped by the GPU; above 1 the alpha-tested
    // parts, foliage first, turn their alpha test into coverage). Applied at the start of the next frame after
    // waiting for the GPU (descriptor sets, and for multisampling the passes and their pipelines, rebuilt): a choice
    // in the options window, never something to change during play.
    void setAnisotropy(u32 level);
    void setMultisample(u32 samples);
    u32 maxAnisotropy() const;
    u32 maxMultisample() const;
    u32 multisample() const; // the samples in use
    // Development (VD_WORLD_A2C=0): multisampling with the plain alpha test, to compare. Before multisampling applies.
    void setAlphaToCoverage(bool enabled);
    // Phase 10: frames of 60 fps per presented frame: 1 (60 fps) or 2 (30 fps). Android paces it with Swappy;
    // elsewhere it is only recorded.
    void setFrameInterval(u32 frames);

    const RenderStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vd
