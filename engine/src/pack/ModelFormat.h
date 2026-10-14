#pragma once

#include "core/Types.h"

namespace vd::pack {

// Baked static models (shared by the baker and the runtime), in models.vdpk under their original paths:
//   world/.../x.m2    Model  M2 doodad (most detailed view, bind pose): one group
//   world/wmo/.../x.wmo Model WMO: one group per WMO group, plus portals and doodad sets
// Model space, Z up (as in the client files); instances carry the transform to world axes.

// 3: WMO liquids, 4: 32-bit indices for large WMOs, 5: M2 effects, 6: interior lighting (ModelLightingHeader),
// 7: skeletons of the doodads that move or turn to the camera (ModelSkeletonHeader), the M2's own radius, tracks over
// the Stand sequence, 8: the WMO groups' BSP trees (ModelBspHeader)
constexpr u32 kModelFormatVersion = 8;

enum class ModelKind : u32 { M2 = 1, Wmo = 2 };

struct ModelVertex {
    f32 position[3];
    i8 normal[4]; // snorm8, w = 0
    f32 uv[2];
    // RGBA. WMO (v6): RGB = the light baked into the vertex colors, halved (with 1.12's overbright), A = the share of the
    // outdoor light instead (0 interior batches, 255 exterior ones and groups without MOCV, the MOCV alpha in transition
    // batches). M2: white.
    u8 color[4];
};
static_assert(sizeof(ModelVertex) == 28, "ModelVertex layout");

// Draw order classes: opaque and alpha-key parts render in the opaque pass (alpha-key with discard),
// the others blend after the opaque world.
enum class ModelBlend : u8 { Opaque = 0, AlphaKey = 1, Alpha = 2, Add = 3, Mod = 4, Mod2x = 5 };

constexpr u16 kModelMaterialUnlit = 1u << 0;
constexpr u16 kModelMaterialUnfogged = 1u << 1;
constexpr u16 kModelMaterialTwoSided = 1u << 2;
constexpr u16 kModelMaterialClamp = 1u << 3;      // clamp texture coordinates (else repeat)
constexpr u16 kModelMaterialVertexColor = 1u << 4; // lit by the vertex colors (WMO interiors)
constexpr u16 kModelMaterialNightGlow = 1u << 5;   // WMO windows/lamps: emissive at night
// WMO liquid surface (MLIQ): no texture, drawn with the map's animated liquid frames. `color` holds the
// liquid kind (0 water, 1 ocean, 2 magma, 3 slime); the vertices' color.r repeats it, color.g is the depth.
constexpr u16 kModelMaterialLiquid = 1u << 6;
// Phase 11 (v6): MOMT 0x08, the inner face of a window: lit by the outdoor light (dark at night) even inside.
constexpr u16 kModelMaterialExteriorLight = 1u << 7;
// M2 (v8): `color` is the batch's own static color and opacity (RGBA), which the texture is multiplied by (without
// this flag an M2 material is white).
constexpr u16 kModelMaterialTinted = 1u << 8;

struct ModelMaterial {
    u64 texture; // assetPathHash of the texture in textures.vdpk, 0 = none (runtime placeholder)
    u8 blend;    // ModelBlend
    u8 uvAnimation; // M2: 1 + index into the model's UV animations (pack/ParticleFormat.h), 0 = static
    u16 flags;
    u32 color; // RGBA: WMO emissive color for night glow, M2 kModelMaterialTinted multiplier, else 0
};
static_assert(sizeof(ModelMaterial) == 16, "ModelMaterial layout");

// One draw: indices of one group with one material. Parts of a group are sorted by blend class.
struct ModelPart {
    u32 firstIndex;
    u32 indexCount;
    u32 vertexOffset; // added to the indices (0 in WMOs)
    u16 material;
    u16 group;
};
// WMO layout: the groups' vertices in Morton order of their centers (neighbors end up adjacent) and the
// parts sorted by blend class, material, lighting and group position. The same material in neighboring
// groups is therefore contiguous in the index buffer, and the renderer merges the parts of the groups in
// view into as few draws as possible. WMOs of more than 65536 vertices (the cities) use 32-bit indices so
// that a material is never split across vertex ranges.
static_assert(sizeof(ModelPart) == 16, "ModelPart layout");

constexpr u32 kModelGroupInterior = 1u << 0; // MOGP 0x2000: lit by its vertex colors
constexpr u32 kModelGroupExterior = 1u << 1; // MOGP 0x8
constexpr u32 kModelGroupExteriorLit = 1u << 2; // MOGP 0x40: outdoor light (city streets flagged interior)
constexpr u32 kModelGroupUnreachable = 1u << 3; // MOGP 0x80: no portal leads to it
constexpr u32 kModelGroupAlwaysDraw = 1u << 4;  // MOGP 0x8000: drawn from outside without portals
// The 1.12 client never draws a group that is unreachable and neither exterior nor always drawn (B1: Stormwind's
// three rough cathedral drafts floating over the city). Older packs have neither bit: nothing changes for them.
constexpr bool modelGroupNeverDrawn(u32 flags) {
    return (flags & kModelGroupUnreachable) != 0 && (flags & (kModelGroupExterior | kModelGroupAlwaysDraw)) == 0;
}

struct ModelGroup {
    f32 boundsMin[3];
    f32 boundsMax[3];
    u32 flags;
    u32 firstPortalRef;
    u32 portalRefCount;
};
static_assert(sizeof(ModelGroup) == 36, "ModelGroup layout");

struct ModelPortal {
    f32 plane[4]; // normal xyz, distance (model space)
    u32 firstVertex; // into the portal vertices (f32[3])
    u32 vertexCount;
};
static_assert(sizeof(ModelPortal) == 24, "ModelPortal layout");

struct ModelPortalRef {
    u16 portal;
    u16 group; // on the other side
    i16 side;
    u16 reserved;
};
static_assert(sizeof(ModelPortalRef) == 8, "ModelPortalRef layout");

struct ModelDoodadSet {
    char name[20];
    u32 first;
    u32 count;
};
static_assert(sizeof(ModelDoodadSet) == 28, "ModelDoodadSet layout");

// A doodad of a WMO doodad set, in the WMO's model space.
struct ModelDoodad {
    u64 model; // assetPathHash of the M2 in models.vdpk
    f32 position[3];
    f32 rotation[4]; // quaternion x, y, z, w
    f32 scale;
    u32 color; // RGBA tint
    u32 reserved;
};
static_assert(sizeof(ModelDoodad) == 48, "ModelDoodad layout");

struct ModelHeader {
    u32 version; // kModelFormatVersion
    u32 kind;    // ModelKind
    f32 boundsMin[3];
    f32 boundsMax[3];
    f32 radius; // bounding sphere around the model origin
    u32 ambientColor; // RGBA, WMO interiors
    u32 vertexCount;
    u32 verticesOffset;
    u32 indexCount; // u16, or u32 with kModelIndices32
    u32 indicesOffset;
    u32 materialCount;
    u32 materialsOffset;
    u32 partCount;
    u32 partsOffset;
    u32 groupCount;
    u32 groupsOffset;
    u32 portalCount;
    u32 portalsOffset;
    u32 portalVertexCount;
    u32 portalVerticesOffset;
    u32 portalRefCount;
    u32 portalRefsOffset;
    u32 doodadSetCount;
    u32 doodadSetsOffset;
    u32 doodadCount;
    u32 doodadsOffset;
    u32 flags; // kModelIndices32
    u32 lightingOffset; // phase 11 (v6): ModelLightingHeader, 0 = none
    // M2 effects (v5): pack::EffectsHeader at effectsOffset (0 = none), and the track table their animated values
    // use (pack/ActorFormat.h encoding): the sequences doodads loop (v5: the first; v7: Stand's variations, else the
    // first sequence; the table is also there for a skeleton without effects). Effects sample the first.
    u32 effectsOffset;
    u32 trackCount;
    u32 tracksOffset;
    u32 rangeCount; // sequenceCount * sequenceSlotCount
    u32 rangesOffset;
    u32 vec3KeyCount;
    u32 vec3KeysOffset;
    u32 quatKeyCount;
    u32 quatKeysOffset;
    u32 sequenceCount; // 0, or the loop's variations
    u32 sequenceSlotCount;
    u32 sequenceDuration; // ms of the first sequence (the loop)
    u32 globalSequenceCount; // u32 durations, ms
    u32 globalSequencesOffset;
    // Phase 11, WMO: the assetPathHash of its MOSB skybox (pack::skyModelPath), low word first; 0 = none.
    u32 reserved2[2];
    // Phase 14 (v7), M2: the skeleton of a doodad that moves or turns to the camera (ModelSkeletonHeader), 0 = drawn
    // as baked. Its bounds (above) then hold every pose of the loop.
    u32 skeletonOffset;
    // Phase 14 (v7), M2: the bounding radius the model file gives (its vertex box), which 1.12's doodad distance rule
    // reads (world/DoodadFade.h); 0 for WMOs.
    f32 fileRadius;
    // Phase 18 (world, v8), WMO: the groups' BSP trees (ModelBspHeader), 0 = none.
    u32 bspOffset;
    u32 reserved3;
};
static_assert(sizeof(ModelHeader) == 208, "ModelHeader layout");

constexpr u32 kModelIndices32 = 1u << 0;

// --- Phase 11 (v6): the light of interiors, and the models' own lights ----------------------------------------------
// At ModelHeader::lightingOffset. A WMO: its lights (MOLT) and fogs (MFOG); per group its fogs, its lights (MOLR) and,
// for true interiors, a grid of the light baked into its vertex colors (what lights an actor standing there); per
// doodad the group that lists it (MODR). An M2: its own lights (a brazier's fire), animated over its first sequence.

constexpr u8 kModelLightOmni = 0; // WMO omni light, M2 point light
constexpr u8 kModelLightSpot = 1;
constexpr u8 kModelLightDirectional = 2;
constexpr u8 kModelLightAmbient = 3;

struct ModelLight {
    f32 position[3]; // model space (M2: bind pose)
    u32 color;       // RGBA, gamma space (M2: the diffuse color)
    f32 intensity;   // M2: the diffuse intensity when it has no keys
    f32 attenStart;  // yards: full light up to here, none from attenEnd
    f32 attenEnd;    // <= 0: not attenuated
    u8 type;         // kModelLight*
    u8 bone;         // phase 14 (v7), M2: the bone it rides (an animated doodad's light follows it), 0xFF = none
    u16 keyCount;    // M2: diffuse intensity keys (ModelLightKey from firstKey), looping over loopMs
    u32 firstKey;
    u32 loopMs;
};
static_assert(sizeof(ModelLight) == 40, "ModelLight layout");

struct ModelLightKey {
    u32 time; // ms into the loop
    f32 intensity;
};
static_assert(sizeof(ModelLightKey) == 8, "ModelLightKey layout");

constexpr u32 kModelFogInfinite = 1u << 0; // MFOG 0x1: 1.12 leaves it out of the camera's fog (WoW.exe 0x69dfe6)

// MFOG: inside innerRadius of `position` the fog replaces the outdoor one, fading back to it at outerRadius.
struct ModelFog {
    f32 position[3]; // model space
    f32 innerRadius;
    f32 outerRadius;
    f32 end;        // yards
    f32 startScale; // start = end * startScale
    u32 color;      // RGBA
    f32 underwaterEnd;
    f32 underwaterStartScale;
    u32 underwaterColor;
    u32 flags; // kModelFogInfinite
};
static_assert(sizeof(ModelFog) == 48, "ModelFog layout");

constexpr u16 kModelNoGroup = 0xFFFF;

// Per WMO group, parallel to the groups.
struct ModelGroupLighting {
    u32 firstLightRef; // u16 indices into the lights (MOLR)
    u32 firstCell;     // light grid: gridSize x * y * z cells (u32 RGBA, x fastest) from firstCell
    f32 gridMin[3];    // model space corner of the grid
    f32 cellSize;      // yards; 0 = no grid (only true interiors carry one)
    u32 average;       // RGBA: the mean vertex color of the group (MOCV), 0 without
    u16 lightRefCount;
    u8 fogs[4];        // into the fogs (MOGP fog ids)
    u8 gridSize[3];
    u8 reserved;
    u16 reserved2;
};
static_assert(sizeof(ModelGroupLighting) == 40, "ModelGroupLighting layout");

struct ModelLightingHeader {
    u32 lightCount; // ModelLight
    u32 lightsOffset;
    u32 lightKeyCount; // ModelLightKey
    u32 lightKeysOffset;
    u32 lightRefCount; // u16
    u32 lightRefsOffset;
    u32 fogCount; // ModelFog
    u32 fogsOffset;
    u32 groupCount; // ModelGroupLighting, one per ModelGroup
    u32 groupsOffset;
    u32 cellCount; // u32 RGBA
    u32 cellsOffset;
    u32 doodadCount; // u16 per doodad: the group listing it, kModelNoGroup if none
    u32 doodadGroupsOffset;
    u32 reserved[2];
};
static_assert(sizeof(ModelLightingHeader) == 64, "ModelLightingHeader layout");

// --- Phase 14 (v7): doodads that move or turn to the camera -----------------------------------------------------------
// At ModelHeader::skeletonOffset, for the M2s whose loop visibly moves a vertex (windmills, water wheels, banners,
// flickering halos), whose vertices hang from billboard bones (torch and lamp halos, flames), or whose particle
// emitters ride a moving bone (vd_baker scan-doodad-anim). Their bones use the pack::ActorBone layout, with tracks
// into the model's own track table (ModelHeader::tracks..., one sequence per variation of the loop). The runtime samples
// one pose per model and phase and skins the vertices on the GPU; the static path keeps drawing the vertices as baked.

// ModelSkeletonHeader::flags.
constexpr u32 kModelSkeletonAnimated = 1u << 0; // some vertex moves over the loop (else only billboards and effects)
// Some bone runs on the loop sequence, not only on global sequences: 1.12 starts each doodad's loop when the doodad
// (or its model) appears, so instances that appeared at other times move out of step.
constexpr u32 kModelSkeletonPhased = 1u << 1;

// The bones' flags (pack::ActorBone::flags of a doodad skeleton): M2 0x8 and 0x40.
constexpr u32 kModelBoneBillboard = 1u << 0;  // spherical: faces the camera (= pack::kActorBoneBillboard)
constexpr u32 kModelBoneBillboardZ = 1u << 1; // cylindrical: turns about the model's Z axis only

// Per vertex, parallel to the model's vertices. Weights sum 255; all zero marks a billboard vertex, rigid on bones[0]
// (a billboard bone): the vertex shader turns it about the bone's pivot to the camera.
struct ModelSkinVertex {
    u8 bones[4];
    u8 weights[4];
};
static_assert(sizeof(ModelSkinVertex) == 8, "ModelSkinVertex layout");

// The variations of the loop (Stand's): the track table's sequences, in order. When one ends 1.12 picks the next by
// these weights (most doodads have one; Westfall's windmills turn their head in one and spin their sails in another).
struct ModelLoopVariation {
    u32 durationMs;
    u16 frequency; // weight among the variations
    u16 reserved;
};
static_assert(sizeof(ModelLoopVariation) == 8, "ModelLoopVariation layout");

struct ModelSkeletonHeader {
    u32 boneCount; // pack::ActorBone (parents before their children)
    u32 bonesOffset;
    u32 skinOffset; // ModelSkinVertex[ModelHeader::vertexCount], 0 for a model without vertices (effects only)
    u32 flags;      // kModelSkeleton*
    u32 loopMs;     // the first variation's duration (ModelHeader::sequenceDuration)
    u32 variationCount; // ModelLoopVariation, = ModelHeader::sequenceCount
    u32 variationsOffset;
    u32 reserved;
};
static_assert(sizeof(ModelSkeletonHeader) == 32, "ModelSkeletonHeader layout");

// --- Phase 18 (world, v8): the WMO groups' BSP trees -------------------------------------------------------------------
// At ModelHeader::bspOffset. 1.12 finds the group the camera is in by a segment straight down from the camera
// (WoW.exe 0x6821f0 -> 0x6be250): the nearest face of a group's BSP (0x6b92b0) or, nearer still, the nearest portal
// crossed (0x6a3f80, the group on the camera's side of it). Per group (parallel to the groups): its MOBN nodes as
// they are, its MOBR refs without the faces that query skips (MOPY 0x04, "detail") or never meets (the vertical ones)
// renumbered into the group's own triangles (MOVI order, only those a leaf refers to), and those triangles' vertices
// (MOVT, model space).

constexpr u16 kModelBspAxisMask = 0x3; // ModelBspNode::flags: the plane's axis (0 x, 1 y, 2 z)
constexpr u16 kModelBspLeaf = 0x4;
constexpr u16 kModelBspNoChild = 0xFFFF;

struct ModelBspNode { // = MOBN (the client's CAaBspNode)
    u16 flags;
    u16 negChild;  // the side where the axis coordinate is below planeDist (kModelBspNoChild: none)
    u16 posChild;  // above it
    u16 faceCount; // a leaf's refs
    u32 firstFace; // into the group's refs
    f32 planeDist;
};
static_assert(sizeof(ModelBspNode) == 16, "ModelBspNode layout");

struct ModelBspGroup {
    u32 firstNode; // into the nodes; the group's root is its first
    u32 nodeCount;
    u32 firstRef; // u16 indices into the group's triangles
    u32 refCount;
    u32 firstTriangle; // u16[3] indices into the group's vertices
    u32 triangleCount;
    u32 firstVertex; // f32[3]
    u32 vertexCount;
};
static_assert(sizeof(ModelBspGroup) == 32, "ModelBspGroup layout");

struct ModelBspHeader {
    u32 groupCount; // ModelBspGroup, = ModelHeader::groupCount
    u32 groupsOffset;
    u32 nodeCount; // ModelBspNode
    u32 nodesOffset;
    u32 refCount; // u16
    u32 refsOffset;
    u32 triangleCount; // u16[3]
    u32 trianglesOffset;
    u32 vertexCount; // f32[3]
    u32 verticesOffset;
    // Per doodad (= ModelHeader::doodadCount) a range (u32 first, u32 count) of the groups whose MODR lists it (u16):
    // 1.12 draws a doodad when one of them is drawn (Ironforge's lava pots: thirteen groups).
    u32 doodadCount;
    u32 doodadRangesOffset;
    u32 doodadGroupCount;
    u32 doodadGroupsOffset;
};
static_assert(sizeof(ModelBspHeader) == 56, "ModelBspHeader layout");

} // namespace vd::pack
