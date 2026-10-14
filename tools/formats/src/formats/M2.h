#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace vd::formats {

// M2 models, version 256 (1.12): geometry of the most detailed view, textures, materials, batches, and the
// skeleton (bones, animation tracks, sequences, attachments). Model space, Z up, facing +X.

// M2Material::blendMode.
enum class M2Blend : u16 { Opaque = 0, AlphaKey = 1, Alpha = 2, NoAlphaAdd = 3, Add = 4, Mod = 5, Mod2x = 6 };

// M2Material::flags.
constexpr u16 kM2MaterialUnlit = 0x1;
constexpr u16 kM2MaterialUnfogged = 0x2;
constexpr u16 kM2MaterialTwoSided = 0x4;
constexpr u16 kM2MaterialBillboard = 0x8;
constexpr u16 kM2MaterialNoDepthWrite = 0x10;

// M2Texture::type: 0 = the file named in the model; the others are chosen per instance at runtime.
constexpr u32 kM2TextureFile = 0;
constexpr u32 kM2TextureBody = 1;       // character skin + clothes (composited), baked NPC skin
constexpr u32 kM2TextureObjectSkin = 2; // cape, item model skins
constexpr u32 kM2TextureHair = 6;
constexpr u32 kM2TextureSkinExtra = 8;  // tauren fur
constexpr u32 kM2TextureMonster1 = 11;  // CreatureDisplayInfo texture variations 1..3
constexpr u32 kM2TextureMonster2 = 12;
constexpr u32 kM2TextureMonster3 = 13;

// M2CompBone::flags.
constexpr u32 kM2BoneSphericalBillboard = 0x8;
constexpr u32 kM2BoneCylindricalZBillboard = 0x40; // turns about Z only to face the camera

using Vec3 = std::array<f32, 3>;
using Quat = std::array<f32, 4>; // x, y, z, w

struct M2Vertex {
    Vec3 position;
    std::array<u8, 4> boneWeights;
    std::array<u8, 4> boneIndices; // straight into M2Model::bones
    Vec3 normal;
    std::array<f32, 2> uv;
    std::array<f32, 2> uv2;
};
static_assert(sizeof(M2Vertex) == 48, "M2 vertex layout");

// Vanilla track: keys of every sequence on one timeline (the sequences' [start, end] ms), plus per sequence an
// inclusive range of key indices to search. A track with a global sequence loops its keys over that global
// duration instead. Interpolation: 0 none (step), 1 linear.
template <typename T>
struct M2Track {
    u16 interpolation = 0;
    i16 globalSequence = -1;
    std::vector<std::array<u32, 2>> ranges; // [first, last] key per sequence; may hold a few extra entries
    std::vector<u32> times;
    std::vector<T> values;
    bool animated() const { return !times.empty(); }
};

struct M2Bone {
    i32 keyBoneId = -1;
    u32 flags = 0;
    i16 parent = -1;
    u16 submesh = 0;
    M2Track<Vec3> translation;
    M2Track<Quat> rotation;
    M2Track<Vec3> scale;
    Vec3 pivot{};
};

struct M2Sequence {
    u16 animationId = 0; // AnimationData.dbc
    u16 variation = 0;
    u32 start = 0; // on the track timeline, ms
    u32 end = 0;
    f32 moveSpeed = 0.0f; // yards per second the animation moves at (walk, run)
    u32 flags = 0;
    i16 frequency = 0; // relative weight among the variations of the animation
    u32 replayMin = 0;
    u32 replayMax = 0;
    u32 blendTime = 0; // ms
    Vec3 boundsMin{};
    Vec3 boundsMax{};
    f32 boundsRadius = 0.0f;
    i16 nextVariation = -1; // next sequence index with the same animation, -1 = last
    u16 aliasNext = 0;
};

// A point on a bone where other models attach (weapons, helmets, the rider of a mount...).
struct M2Attachment {
    u32 id = 0;
    u16 bone = 0;
    Vec3 position{}; // model space, bind pose
};

// Phase 14: a named moment of the model's animations (M2Event): "$FD1" a foot down, "$DSO" / "$DSL" a sound,
// "$CSD" an emote's sound... `id` is the four characters as stored, read as a little-endian u32 ('$' in the low
// byte: pack::actorEventId). It fires at each of its times within a sequence (the same timeline as the tracks);
// an event without times is always on while the model shows (a sound loop).
struct M2Event {
    u32 id = 0;
    u32 data = 0;    // what it plays (a SoundEntries id for the sound events...)
    u16 bone = 0;    // the point moves with this bone
    Vec3 position{}; // model space, bind pose
    i16 globalSequence = -1;
    std::vector<std::array<u32, 2>> ranges; // [first, last] time per sequence, as M2Track
    std::vector<u32> times;
};

struct M2Texture {
    u32 type = 0;  // kM2Texture*
    u32 flags = 0; // 1 = wrap S, 2 = wrap T
    std::string path;
};

struct M2Material {
    u16 flags = 0;
    M2Blend blend = M2Blend::Opaque;
};

// One draw of the model: a range of triangles with a texture and a material.
struct M2Batch {
    u32 firstIndex = 0; // into M2Model::triangles
    u32 indexCount = 0;
    i32 texture = -1;   // into M2Model::textures, -1 if none
    u32 material = 0;   // into M2Model::materials
    u16 geosetId = 0;   // submesh id (character geosets; all shown for doodads)
    i8 priorityPlane = 0;
    u8 flags = 0;
    f32 alpha = 1.0f;   // static opacity: first key of its color alpha and transparency tracks
    i16 color = -1;        // into M2Model::colors (RGB + alpha multiplier), -1 = none
    i16 transparency = -1; // into M2Model::transparency (texture weight), -1 = none
    i16 textureTransform = -1; // into M2Model::textureTransforms, -1 = none
};

// Animated batch color: RGB (0..1) and opacity (fixed16 / 32767).
struct M2Color {
    M2Track<Vec3> color;
    M2Track<i16> alpha;
};

// Texture coordinate animation of a batch (scrolling water and lava, swirling portals): applied to the UVs as
// translation, then rotation and scale about the texture's center.
struct M2TextureTransform {
    M2Track<Vec3> translation;
    M2Track<Quat> rotation;
    M2Track<Vec3> scale;
};

// M2ParticleOld::flags (the bits vanilla data uses; see vd_baker scan-particles).
constexpr u32 kM2ParticleLit = 0x1;            // lit like the model (else full bright)
constexpr u32 kM2ParticleSort = 0x2;           // depth sorted
constexpr u32 kM2ParticleVelocityOrient = 0x4; // the quad stretches along the velocity
constexpr u32 kM2ParticleUnfogged = 0x8;
constexpr u32 kM2ParticleModelSpace = 0x10;    // particles stay in the emitter's space (follow it)
constexpr u32 kM2ParticleInheritScale = 0x20;  // size times the bone's scale
constexpr u32 kM2ParticleBurst = 0x40;         // the emitter's own motion adds to the particles' velocity
constexpr u32 kM2ParticleImplode = 0x80;       // sphere: particles that pass the center die
constexpr u32 kM2ParticleGoUp = 0x100;         // sphere: velocity straight up
constexpr u32 kM2ParticleRandomSpinSign = 0x200;
constexpr u32 kM2ParticleClampTail = 0x400;    // tail length limited to the particle's age
constexpr u32 kM2ParticleXYQuad = 0x1000;      // quad in the emitter's XY plane, not facing the camera
constexpr u32 kM2ParticleGround = 0x2000;      // projected to the ground
constexpr u32 kM2ParticleFollow = 0x4000;      // particles follow the emitter's movement
constexpr u32 kM2ParticleRandomCell = 0x10000; // a random texture cell per particle

// M2ParticleOld::emitterType.
constexpr u16 kM2EmitterPlane = 1;
constexpr u16 kM2EmitterSphere = 2;
constexpr u16 kM2EmitterSpline = 3;

// A particle emitter (M2ParticleOld of 1.12, 504 bytes): emission parameters animated per sequence, and the
// particle's look over its life as three points (birth, midPoint, death).
struct M2ParticleEmitter {
    i32 id = -1;
    u32 flags = 0;
    Vec3 position{}; // model space, bind pose; moves with `bone`
    u16 bone = 0;
    u16 texture = 0; // into M2Model::textures
    std::string geometryModel;  // emits models instead of quads (unsupported)
    std::string recursionModel; // the emitters of another model (unsupported)
    u16 blend = 0;       // EGxBlend, as M2Material::blend
    u16 emitterType = 0; // kM2Emitter*
    u8 particleType = 0;
    u8 headOrTail = 0; // 0 head (camera-facing quad), 1 tail (stretched along the velocity), 2 both
    i16 tileRotation = 0;
    u16 rows = 1; // texture cells
    u16 columns = 1;
    M2Track<f32> speed;
    M2Track<f32> speedVariation;  // fraction of speed
    M2Track<f32> verticalRange;   // radians: polar angle of the initial velocity
    M2Track<f32> horizontalRange; // radians: azimuth
    M2Track<f32> gravity;         // yards / s^2 downwards
    M2Track<f32> lifespan;        // seconds
    M2Track<f32> emissionRate;    // particles per second
    M2Track<f32> areaLength;      // plane: size along X; sphere: minimum radius
    M2Track<f32> areaWidth;       // plane: size along Y; sphere: maximum radius
    M2Track<f32> zSource;         // > 0: velocity away from (0, 0, zSource)
    f32 midPoint = 0.5f;          // of the life, 0..1
    std::array<u32, 3> colors{};  // BGRA at birth, midPoint, death (alpha = opacity)
    std::array<f32, 3> scales{};  // quad half size, yards
    std::array<u16, 3> headLifeCells{};  // texture cell animation of the first half of the life
    std::array<u16, 3> headDecayCells{}; // idem, second half
    std::array<i16, 2> tailLifeCells{};
    std::array<i16, 2> tailDecayCells{};
    f32 tailLength = 0.0f;
    f32 twinkleSpeed = 0.0f;
    f32 twinklePercent = 1.0f;
    f32 twinkleMin = 1.0f;
    f32 twinkleMax = 1.0f;
    f32 burstMultiplier = 0.0f;
    f32 drag = 0.0f;
    f32 spin = 0.0f;
    Vec3 tumbleMin{};
    Vec3 tumbleMax{};
    Vec3 wind{};
    f32 windTime = 0.0f;
    f32 followSpeed1 = 0.0f;
    f32 followScale1 = 0.0f;
    f32 followSpeed2 = 0.0f;
    f32 followScale2 = 0.0f;
    std::vector<Vec3> splinePoints;
    M2Track<u8> enabled; // emits while nonzero (no keys: always)
};

// A ribbon: a strip that trails behind a bone (weapon swings, comet tails).
struct M2RibbonEmitter {
    i32 id = -1;
    u32 bone = 0;
    Vec3 position{};
    std::vector<u16> textures;  // into M2Model::textures
    std::vector<u16> materials; // into M2Model::materials (blend mode)
    M2Track<Vec3> color;
    M2Track<i16> alpha; // fixed16
    M2Track<f32> above; // half heights over and under the bone
    M2Track<f32> below;
    f32 edgesPerSecond = 0.0f;
    f32 edgeLifetime = 0.0f; // seconds
    f32 gravity = 0.0f;
    u16 rows = 1;
    u16 columns = 1;
    M2Track<u16> textureSlot;
    M2Track<u8> visibility;
};

// M2Camera::type (the camera lookup table is indexed by it).
constexpr i32 kM2CameraPortrait = 0;      // the unit frames' portrait (SetPortraitTexture)
constexpr i32 kM2CameraCharacterInfo = 1; // the paper doll
constexpr i32 kM2CameraFlyby = -1;

// A camera of the model (M2Camera before 2.0, 124 bytes): where it is and what it looks at, each a base
// vector plus an animated track (spline keys: their values only, tangents dropped), the field of view (the
// diagonal one, in radians) and the clip planes.
struct M2Camera {
    i32 type = kM2CameraFlyby;
    f32 fov = 0.0f;
    f32 farClip = 0.0f;
    f32 nearClip = 0.0f;
    Vec3 positionBase{};
    Vec3 targetBase{};
    M2Track<Vec3> position; // added to positionBase
    M2Track<Vec3> target;   // added to targetBase
    M2Track<f32> roll;      // radians
    // Phase 14 (cinematics): the keys' in and out tangents, parallel to the values (interpolation 2 bezier,
    // 3 hermite: the flyby cameras curve between their keys).
    std::vector<std::array<Vec3, 2>> positionTangents;
    std::vector<std::array<Vec3, 2>> targetTangents;
    std::vector<std::array<f32, 2>> rollTangents;
};

// Phase 11: a light of the model (M2Light of 1.12, 212 bytes): a point (or directional) light on a bone with its
// colors and intensities animated. The braziers' and campfires' fire light, reaching about a yard: what it lights
// is the model itself.
constexpr u16 kM2LightDirectional = 0;
constexpr u16 kM2LightPoint = 1;

struct M2Light {
    u16 type = kM2LightPoint;
    i16 bone = -1;
    Vec3 position{}; // model space, bind pose
    M2Track<Vec3> ambientColor;
    M2Track<f32> ambientIntensity;
    M2Track<Vec3> diffuseColor;
    M2Track<f32> diffuseIntensity;
    M2Track<f32> attenuationStart; // yards
    M2Track<f32> attenuationEnd;
    M2Track<u8> visibility;
};

struct M2Model {
    std::string name;
    u32 globalFlags = 0;
    std::vector<M2Vertex> vertices;
    std::vector<u32> triangles; // view 0 triangle list, indices into `vertices`
    std::vector<M2Texture> textures;
    std::vector<M2Material> materials;
    std::vector<M2Batch> batches;
    std::array<f32, 3> boundsMin{};
    std::array<f32, 3> boundsMax{};
    f32 boundsRadius = 0.0f;
    u32 sequenceCount = 0;
    // Collision mesh ("bounding triangles"): what the player bumps into. Most doodads without one (grass,
    // flowers) can be walked through. Winding as stored in the file (vMaNGOS' extractor flips it).
    std::vector<Vec3> collisionPositions;
    std::vector<u16> collisionIndices;

    std::vector<u32> globalSequences; // durations, ms
    std::vector<M2Sequence> sequences;
    std::vector<M2Bone> bones;         // parents come before their children
    std::vector<M2Attachment> attachments;
    std::vector<M2Event> events; // phase 14 (empty when unreadable)
    std::vector<M2Color> colors;
    std::vector<M2Track<i16>> transparency; // fixed16 opacity

    // Effects.
    std::vector<M2TextureTransform> textureTransforms;
    std::vector<M2ParticleEmitter> particles;
    std::vector<M2RibbonEmitter> ribbons;
    u32 textureFlipbookCount = 0; // header array of 1.12 (never used by the data: scan-particles)
    // Set when the effect arrays are unreadable: the model loads without them (scan-particles reports it).
    std::string effectsError;

    // Cameras, and the lookup by camera type (cameraLookup[type] = index into cameras, -1 = none).
    std::vector<M2Camera> cameras;
    std::vector<i16> cameraLookup;
    // The camera of `type` (through the lookup, else the first of that type), nullptr if the model has none.
    const M2Camera* camera(i32 type) const;

    std::vector<M2Light> lights; // phase 11 (empty when unreadable)
};

// Phase 11: value of a scalar track at `time` (as the Vec3 version).
f32 sampleM2Track(const M2Track<f32>& track, u32 sequence, u32 time, f32 fallback);

Result<M2Model> parseM2(std::span<const u8> data);

// Value of a vanilla track at `time` on its timeline, searching the keys of `range` ([first, last]) and
// clamping outside them. Used by the baker's checks and tests; the runtime samples baked tracks.
Vec3 sampleM2Track(const M2Track<Vec3>& track, u32 sequence, u32 time, const Vec3& fallback);
Quat sampleM2Track(const M2Track<Quat>& track, u32 sequence, u32 time, const Quat& fallback); // normalized lerp

} // namespace vd::formats
