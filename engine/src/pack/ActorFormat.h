#pragma once

#include "core/Types.h"

#include <cmath>

namespace vd::pack {

// Baked animated models (shared by the baker and the runtime), in actors.vdpk under their original paths
// (".m2"): the race models (character/), creature models (CreatureModelData) and the item models attached to
// them (item/objectcomponents/). Model space, Z up, facing +X. Vertices are skinned on the GPU with up to four
// bones; the skeleton is sampled on the CPU from the tracks below.
//
// Layout: ActorModelHeader, then 16-byte aligned sections at the offsets it gives (relative to the blob).

// 2: keys grouped by sequence, local times; 3: effects; 4: portrait camera; 5: events (ActorEventsHeader).
constexpr u32 kActorFormatVersion = 5;

struct ActorVertex {
    f32 position[3]; // bind pose
    i8 normal[4];    // snorm8, w = 0
    f32 uv[2];
    u8 weights[4]; // sum 255
    u8 bones[4];   // into the model's bones
};
static_assert(sizeof(ActorVertex) == 32, "ActorVertex layout");

// Textures a material takes from the instance instead of a file (M2 texture types). The runtime keeps one
// texture per slot for each actor: its composited skin, hair, cape, creature skins...
enum class ActorTextureSlot : u8 {
    File = 0,     // ActorMaterial::texture
    Body = 1,     // type 1: character skin + clothes (composited) or a baked NPC skin
    Cape = 2,     // type 2: cape, or an item model's own skin
    Hair = 3,     // type 6
    SkinExtra = 4, // type 8: tauren fur
    Monster1 = 5, // types 11-13: CreatureDisplayInfo texture variations
    Monster2 = 6,
    Monster3 = 7,
    None = 8, // another replaceable type the runtime never fills: drawn untextured
};
constexpr u32 kActorTextureSlots = 8; // File..Monster3

constexpr u16 kActorNoColorTrack = 0xFFFF;

// A material's color can be animated (glows, ghosts, fading effects): RGB times the color track, alpha times
// the color's alpha track and the transparency track (scalar tracks keep their value in ActorVec3Key::value[0]).
struct ActorMaterial {
    u64 texture; // ActorTextureSlot::File: assetPathHash of the texture in textures.vdpk (0 = none)
    u8 blend;    // pack::ModelBlend
    u8 slot;     // ActorTextureSlot
    u16 flags;   // pack::kModelMaterial*
    u32 color;   // RGBA multiplier when nothing is animated (alpha = the batch's first opacity)
    u16 colorTrack;        // RGB, ActorTrack index or kActorNoColorTrack
    u16 alphaTrack;        // opacity 0..1
    u16 transparencyTrack; // opacity 0..1
    u16 uvAnimation;       // 1 + index into the model's UV animations (pack/ParticleFormat.h), 0 = static
};
static_assert(sizeof(ActorMaterial) == 24, "ActorMaterial layout");

// One draw: a submesh with a material. Parts are sorted by blend class (opaque, alpha key, then the blended ones
// in the model's order); opaque and alpha-key parts also by material, so neighbors can merge into one draw.
struct ActorPart {
    u32 firstIndex;
    u32 indexCount;
    u16 material;
    u16 geoset;      // M2 submesh id (character geosets: 0 body, 1-99 hair, 101 facial hair, 401 gloves...)
    u16 geosetIndex; // into the model's sorted geoset table
    u16 reserved;
};
static_assert(sizeof(ActorPart) == 16, "ActorPart layout");

constexpr u32 kActorNoTrack = ~0u;

// ActorBone::flags.
constexpr u32 kActorBoneBillboard = 1u << 0; // spherical billboard: always faces the camera
constexpr u32 kActorBoneBillboardZ = 1u << 1; // ours (quest marks): turns about Z only (M2 bone flag 0x40)

struct ActorBone {
    i32 keyBone; // key bone id (-1: none)
    u32 flags;   // kActorBone*
    i16 parent;  // -1 = root; parents come before their children
    u16 reserved;
    f32 pivot[3];
    u32 tracks[3]; // translation, rotation, scale: ActorTrack index or kActorNoTrack (identity)
};
static_assert(sizeof(ActorBone) == 36, "ActorBone layout");

// How a track's keys are stored.
enum class ActorTrackKind : u8 {
    Constant = 0, // one key: firstKey
    Global = 1,   // keys firstKey.. (keyCount) loop over the global sequence's duration
    Sequence = 2, // keys per sequence: ActorRange ranges[sequence * sequenceSlotCount + slot]
};

// One animated channel (translation, rotation or scale of a bone; a material's color or opacity). Values live in
// the vec3 keys (translation, scale, colors; opacity in value[0]) or the quaternion keys (rotation).
//
// Sampling a pose touches every animated track of one sequence, so the baker groups keys by sequence: for sequence
// s, the ranges of all its tracks are adjacent (s * sequenceSlotCount ..), and so are their keys. Times are local
// to the sequence (0 = its start); each range has keys at 0 and at the end, sampled on the original timeline, so
// the value is held outside and no search leaves the range.
struct ActorTrack {
    u8 kind;           // ActorTrackKind
    u8 interpolation;  // 0 step, 1 linear
    i16 globalSequence; // Global tracks
    u32 firstKey;      // Constant, Global
    u32 keyCount;      // Constant, Global
    u32 slot;          // Sequence tracks: column in the per-sequence range table
};
static_assert(sizeof(ActorTrack) == 16, "ActorTrack layout");

struct ActorRange {
    u32 first; // into the vec3 or quaternion keys
    u32 count; // at least 1
};
static_assert(sizeof(ActorRange) == 8, "ActorRange layout");

struct ActorVec3Key {
    u32 time;
    f32 value[3];
};
static_assert(sizeof(ActorVec3Key) == 16, "ActorVec3Key layout");

struct ActorQuatKey {
    u32 time;
    i16 value[4]; // x, y, z, w as snorm16
};
static_assert(sizeof(ActorQuatKey) == 12, "ActorQuatKey layout");

struct ActorSequence {
    u16 animation; // AnimationData.dbc
    u16 variation;
    u32 start; // timeline ms
    u32 end;
    f32 moveSpeed;
    u32 flags;
    i16 frequency;
    i16 nextVariation; // sequence index of the next variation of the same animation, -1 = none
    u32 blendTime;     // ms
    f32 boundsRadius;
};
static_assert(sizeof(ActorSequence) == 32, "ActorSequence layout");

struct ActorAttachment {
    u32 id; // attachment id (1 right hand, 2 left hand, 11 helm, 18 name...)
    u16 bone;
    u16 reserved;
    f32 position[3]; // model space, bind pose
};
static_assert(sizeof(ActorAttachment) == 20, "ActorAttachment layout");

// ActorPortrait::flags.
constexpr u32 kActorPortraitComputed = 1u << 0; // the model has no portrait camera: framed by the baker

// How the unit frames' portrait sees the model (the client's SetPortraitTexture), model space: the M2's
// portrait camera (type 0) as it stands at the first frame (base vectors plus the first key of its tracks),
// or, for the few models without one, a framing of the head (key bone / helm attachment) or of the top of the
// bounds.
struct ActorPortrait {
    f32 position[3]; // the camera
    f32 target[3];   // what it looks at
    f32 fov;         // the camera's diagonal field of view, radians (a square: tan(fov / 2) / sqrt(2) per axis)
    f32 nearClip;
    f32 farClip;
    f32 roll;  // radians about the view direction
    u32 flags; // kActorPortrait*
    u32 reserved;
};
static_assert(sizeof(ActorPortrait) == 48, "ActorPortrait layout");

// --- Phase 14 (v5): the model's events (M2Event) ------------------------------------------------------------------
// Named moments of the animations: "$FD1"... a foot down, "$DSO" / "$DSL" a sound, "$CSD" an emote's sound. An event
// fires at each of its times inside the sequence playing (local ms, as the tracks'); one without times
// (kActorEventTimed clear) is always on while the model shows.

// The id of an event as stored: its four characters as a little-endian u32 ('$' in the low byte).
constexpr u32 actorEventId(const char (&id)[5]) {
    return static_cast<u32>(static_cast<u8>(id[0])) | static_cast<u32>(static_cast<u8>(id[1])) << 8 |
           static_cast<u32>(static_cast<u8>(id[2])) << 16 | static_cast<u32>(static_cast<u8>(id[3])) << 24;
}

constexpr u32 kActorEventTimed = 1u << 0; // fires at times (else always on)

struct ActorEvent {
    u32 id;   // actorEventId
    u32 data; // the event's data word (what it plays)
    u16 bone; // its point moves with this bone
    u16 flags; // kActorEventTimed
    f32 position[3]; // model space, bind pose
    // Times: ActorEventsHeader::ranges[firstRange + sequence] (sequenceCount entries; count 0: none in that sequence).
    u32 firstRange;
};
static_assert(sizeof(ActorEvent) == 28, "ActorEvent layout");

struct ActorEventsHeader {
    u32 eventCount; // ActorEvent
    u32 eventsOffset;
    u32 rangeCount; // ActorRange into the times: eventCount * sequenceCount
    u32 rangesOffset;
    u32 timeCount; // u32 local ms, ascending within a range
    u32 timesOffset;
    u32 reserved[2];
};
static_assert(sizeof(ActorEventsHeader) == 32, "ActorEventsHeader layout");

struct ActorModelHeader {
    u32 version; // kActorFormatVersion
    u32 flags;
    f32 boundsMin[3];
    f32 boundsMax[3];
    f32 radius; // around the model origin
    u32 vertexCount;
    u32 verticesOffset;
    u32 indexCount; // u16
    u32 indicesOffset;
    u32 materialCount;
    u32 materialsOffset;
    u32 partCount;
    u32 partsOffset;
    u32 geosetCount; // sorted unique u16 geoset ids
    u32 geosetsOffset;
    u32 boneCount;
    u32 bonesOffset;
    u32 trackCount;
    u32 tracksOffset;
    u32 rangeCount;
    u32 rangesOffset;
    u32 vec3KeyCount;
    u32 vec3KeysOffset;
    u32 quatKeyCount;
    u32 quatKeysOffset;
    u32 sequenceCount;
    u32 sequencesOffset;
    u32 globalSequenceCount; // u32 durations, ms
    u32 globalSequencesOffset;
    u32 attachmentCount;
    u32 attachmentsOffset;
    u32 sequenceSlotCount; // Sequence tracks; rangeCount = sequenceCount * sequenceSlotCount
    u32 effectsOffset;     // pack::EffectsHeader (pack/ParticleFormat.h), 0 = none; its tracks are in the tables above
    u32 portraitOffset;    // ActorPortrait (version 4), 0 = none (a model without vertices)
    u32 eventsOffset;      // ActorEventsHeader (version 5), 0 = none
    // The selection circle's half width under the model, unscaled (actorSelectionHalfSize of its Stand box); 0 in
    // the bakes before it (the field was reserved): the runtime takes the bind pose's box instead.
    f32 selectionHalfSize;
};
static_assert(sizeof(ActorModelHeader) == 160, "ActorModelHeader layout");

// WoW.exe (CGUnit_C 0x60AEE0, read by the circle's draw at 0x608E09): the selection circle is a square of this half
// width (times OBJECT_FIELD_SCALE_X) around the feet, from the model's box: its Stand animation's (the first sequence
// of animation 0), or the M2 header's when it has none. The square root of half the box's horizontal diagonal (a
// big creature's circle grows slower than its body); 1.2 for a box with no width.
inline f32 actorSelectionHalfSize(const f32 boundsMin[3], const f32 boundsMax[3]) {
    const f32 dx = boundsMax[0] - boundsMin[0];
    const f32 dy = boundsMax[1] - boundsMin[1];
    if (dx == 0.0f && dy == 0.0f) {
        return 1.2f;
    }
    return std::sqrt(0.5f * std::sqrt(dx * dx + dy * dy));
}

} // namespace vd::pack
