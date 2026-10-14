#pragma once

#include "core/Types.h"

namespace vd::pack {

// Effects of a baked M2 (shared by the baker and the runtime): particle emitters, ribbons and texture
// coordinate animations, in the same blob as the model they belong to. Both formats carry them:
//   models.vdpk (ModelHeader, static doodads): every animated value is a track of the model's own track table
//     (ModelHeader::tracks..., one sequence: the model's first, which doodads loop), and each emitter's bone
//     pose at the start of that sequence is baked into ParticleEmitter::transform.
//   actors.vdpk (ActorModelHeader): the tracks live in the actor's track table (per sequence, like the
//     bones'), and emitters move with their bone (ParticleEmitter::bone of the sampled skeleton).
// Tracks use the actor track encoding (pack/ActorFormat.h: ActorTrack, ActorRange, ActorVec3Key; scalars in
// x). A value that never changes has no track (kEffectNoTrack) and is stored in place.
//
// Particles follow the 1.12 client: an emitter spawns `emissionRate` particles per second, each living
// `lifespan` seconds; color, opacity and size go through three points (birth, midPoint, death) of its life.

constexpr u32 kEffectNoTrack = ~0u;

// Animated emitter parameters (ParticleEmitter::tracks / values).
enum EmitterParam : u32 {
    kEmitterSpeed = 0,      // yards per second
    kEmitterSpeedVariation, // fraction of the speed: speed * (1 + variation * rand(-1, 1))
    kEmitterVerticalRange, // radians: polar angle of the initial velocity (plane) or of the position (sphere)
    kEmitterHorizontalRange, // radians: azimuth
    kEmitterGravity,         // yards / s^2 downwards (negative: upwards)
    kEmitterLifespan,        // seconds
    kEmitterRate,            // particles per second
    kEmitterAreaLength,      // plane: size along the emitter's X; sphere: smallest radius
    kEmitterAreaWidth,       // plane: size along Y; sphere: largest radius
    kEmitterZSource,         // > 0: initial velocity away from (0, 0, zSource)
    kEmitterEnabled,         // emits while > 0.5
    kEmitterParamCount,
};

// ParticleEmitter::flags: the M2 bits (formats::kM2Particle*), as the client reads them.
constexpr u32 kParticleLit = 0x1;
constexpr u32 kParticleVelocityOrient = 0x4; // the quad stretches along the velocity
constexpr u32 kParticleUnfogged = 0x8;
constexpr u32 kParticleModelSpace = 0x10;   // particles live in the emitter's space and move with it
constexpr u32 kParticleInheritScale = 0x20; // size times the emitter's scale
constexpr u32 kParticleBurst = 0x40;        // the emitter's own motion adds to the particles' velocity
constexpr u32 kParticleImplode = 0x80;      // sphere: particles passing the center die
constexpr u32 kParticleGoUp = 0x100;        // sphere: velocity straight up
constexpr u32 kParticleRandomSpinSign = 0x200;
constexpr u32 kParticleClampTail = 0x400;    // tail no longer than the particle's age
constexpr u32 kParticleXYQuad = 0x1000;      // quad lies in the emitter's XY plane
constexpr u32 kParticleGround = 0x2000;      // spawned on the ground (the emitter's z = 0)
constexpr u32 kParticleFollow = 0x4000;      // particles follow the emitter's movement
constexpr u32 kParticleRandomCell = 0x10000; // each particle shows one random texture cell

enum class EmitterShape : u8 { Plane = 1, Sphere = 2, Spline = 3 };

// ParticleEmitter::style: what each particle draws.
enum class ParticleStyle : u8 { Head = 0, Tail = 1, Both = 2 };

struct ParticleEmitter {
    u64 texture; // assetPathHash in textures.vdpk (cells: rows x columns)
    u32 flags;   // kParticle*
    u16 bone;    // actors: the skeleton bone it moves with; models: unused (see transform)
    u8 blend;    // pack::ModelBlend
    u8 shape;    // EmitterShape
    u8 style;    // ParticleStyle
    u8 rows;     // texture cells
    u8 columns;
    u8 reserved;
    f32 position[3];   // emitter origin, model space (bind pose)
    f32 transform[12]; // models: emitter -> model space (bone pose and position); actors: identity, unused
    u32 tracks[kEmitterParamCount]; // track index, or kEffectNoTrack: the value is values[param]
    f32 values[kEmitterParamCount];
    f32 midPoint;     // of the life, 0..1
    u32 colors[3];    // RGBA8 (red in the low byte): birth, midPoint, death; alpha = opacity
    f32 scales[3];    // quad half size, yards
    u16 headCells[6]; // texture cell over the life: first half start, end, repeat; second half start, end,
                      // repeat
    i16 tailCells[4]; // first half start, end; second half start, end
    f32 tailLength;   // seconds of motion a tail covers
    f32 twinkleSpeed; // twinkle: some particles hidden or rescaled, switching `twinkleSpeed` times per second
    f32 twinklePercent; // fraction of particles shown at a time
    f32 twinkleMin;     // size multiplier range
    f32 twinkleMax;
    f32 burstMultiplier;
    f32 drag; // velocity *= 1 - drag * dt
    f32 spin; // radians per second the quad turns
    f32 wind[3];
    f32 windTime;
    f32 followSpeed1; // kParticleFollow: how much of the emitter's motion is passed on, by its speed
    f32 followScale1;
    f32 followSpeed2;
    f32 followScale2;
    u32 firstSplinePoint; // EffectsHeader spline points (f32[3]), shape Spline
    u32 splinePointCount;
    f32 reach; // yards around the emitter origin its particles can cover (culling), computed by the baker
    u32 mesh;  // phase 14 (spellfx): ParticleMesh index + 1 when its particles are a model (0: quads)
};
static_assert(sizeof(ParticleEmitter) == 296, "ParticleEmitter layout");

// A ribbon trails behind a moving bone: a strip of edges, each `above` over and `below` under the bone, added
// `edgesPerSecond` times per second and dropped after `edgeLifetime` seconds.
enum RibbonParam : u32 {
    kRibbonAbove = 0,
    kRibbonBelow,
    kRibbonAlpha,
    kRibbonVisibility, // drawn while > 0.5
    kRibbonParamCount,
};

struct RibbonEmitter {
    u64 texture; // assetPathHash in textures.vdpk
    u16 bone;
    u8 blend; // pack::ModelBlend
    u8 reserved;
    u32 color; // RGBA8 (constant in 1.12 data)
    f32 position[3];
    f32 transform[12]; // models: emitter -> model space; actors: unused
    u32 tracks[kRibbonParamCount];
    f32 values[kRibbonParamCount];
    f32 edgesPerSecond;
    f32 edgeLifetime;
    f32 gravity;
    u32 reserved2[2];
};
static_assert(sizeof(RibbonEmitter) == 128, "RibbonEmitter layout");

// Texture coordinate animation (water, lava, energy fields): uv' = T(translation) * C * R(rotation) *
// S(scale) * C^-1
// * uv, C = translate(0.5, 0.5). Translation and scale are vec3 tracks, rotation a quaternion track.
struct UvAnimation {
    u32 tracks[3]; // translation, rotation, scale: track index or kEffectNoTrack (identity)
};
static_assert(sizeof(UvAnimation) == 12, "UvAnimation layout");

// Phase 14 (spellfx): what the particles of an emitter with a geometry model are (M2 geometry_model: Cone of
// Cold's shards, Cyclone's rocks and dust, skulls): that model's triangles (its own space, bind pose, the
// texture and blend of its first batch), drawn at each particle's place and size, in its color, turned by a
// random orientation that tumbles at a random rate between tumbleMin and tumbleMax (radians per second per
// axis; random signs with kParticleMeshRandomSigns), as WoW.exe's CParticle2_Model (0x7B2540, 0x7B4510). Old
// blobs have no meshes (EffectsHeader::meshCount 0) and their emitters' `mesh` 0: no format version change.
constexpr u32 kParticleMeshRandomSigns = 0x8000; // the emitter flag WoW.exe tests at 0x7B25E3

struct ParticleMeshVertex {
    f32 position[3];
    f32 uv[2];
};
static_assert(sizeof(ParticleMeshVertex) == 20, "ParticleMeshVertex layout");

struct ParticleMesh {
    u64 texture;     // assetPathHash in textures.vdpk
    u32 firstVertex; // into EffectsHeader's mesh vertices: 3 per triangle
    u32 triangleCount;
    f32 tumbleMin[3];
    f32 tumbleMax[3];
    u8 blend; // pack::ModelBlend of the model's first batch
    u8 reserved[7];
};
static_assert(sizeof(ParticleMesh) == 48, "ParticleMesh layout");

// Where a blob keeps its effects (offsets relative to the blob, 16-byte aligned sections).
struct EffectsHeader {
    u32 emitterCount;
    u32 emittersOffset;
    u32 ribbonCount;
    u32 ribbonsOffset;
    u32 uvAnimationCount;
    u32 uvAnimationsOffset;
    u32 splinePointCount; // f32[3]
    u32 splinePointsOffset;
    f32 reach; // yards around the model origin any of its particles can reach (culling)
    u32 meshCount; // phase 14 (spellfx): ParticleMesh records (0 in older blobs)
    u32 meshesOffset;
    u32 meshVerticesOffset; // ParticleMeshVertex, as many as the meshes use
};
static_assert(sizeof(EffectsHeader) == 48, "EffectsHeader layout");

} // namespace vd::pack
