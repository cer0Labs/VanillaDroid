#pragma once

#include "core/Types.h"

namespace vd::audio {

struct CreatureSounds;

// Phase 14: what the M2 events of a model's animations sound like, as WoW.exe 1.12.1 dispatches them: the unit
// events in its animation event handler (0x5FFBE0: footsteps, fidgets, wings, emote sounds), the doodads' and game
// objects' sound events in their own (0x6951E0: one loop per model that $DSL starts or replaces and $DSE stops;
// $DSO one-shots). Pure rules, tested without packs (tests/audio/test_model_sounds.cpp).

// An event id as stored in the M2 (its four characters as a little-endian u32, '$' in the low byte: the value
// pack::actorEventId gives).
constexpr u32 modelEventId(const char (&id)[5]) {
    return static_cast<u32>(static_cast<u8>(id[0])) | static_cast<u32>(static_cast<u8>(id[1])) << 8 |
           static_cast<u32>(static_cast<u8>(id[2])) << 16 | static_cast<u32>(static_cast<u8>(id[3])) << 24;
}

enum class ModelEventKind : u8 {
    None,
    Footstep,      // $FSD: FootstepTerrainLookup by the unit's footstep id and the ground
    DoodadLoop,    // $DSL: data = SoundEntries id, the model's loop (started, or replacing the one playing)
    DoodadOneShot, // $DSO: data = SoundEntries id
    DoodadStop,    // $DSE: the model's loop ends
    Sound,         // $SND: data = SoundEntries id (spell effect models)
    EmoteSound,    // $CSD: data = SoundEntries id (a character's emote: claps, laughs, kisses)
    EmoteState,    // $ESD: Emotes.dbc's sound of the unit's emote state (mining, chopping wood)
    Fidget,        // $FD1..$FD4: CreatureSoundData fidget 0..3 ($FD5..$FD9: nothing in 1.12)
    Stand,         // $FDX: CreatureSoundData's stand sound (not for the player's own character)
    CustomAttack,  // $AH0..$AH3: CreatureSoundData custom attack 0..3 (while it attacks)
    WingFlap,      // $WNG: CreatureSoundData's wing flap
    WingGlide,     // $WGG: its wing glide
};

struct ModelEvent {
    ModelEventKind kind = ModelEventKind::None;
    u8 index = 0; // Fidget, CustomAttack
};

ModelEvent classifyModelEventInfo(u32 id);
inline ModelEventKind classifyModelEvent(u32 id) {
    return classifyModelEventInfo(id).kind;
}

// The CreatureSoundData sound of a unit event (Fidget, Stand, CustomAttack, WingFlap, WingGlide); 0 for none.
u32 creatureEventSound(const CreatureSounds& sounds, const ModelEvent& event);

// --- Voices: the models' sounds share the mixer with everything else ----------------------------------------------
// At most this many loops of doodads, terrain emitters and creatures play at once (the nearest), and this many
// footsteps of other units at the same time (each lasts ~0.3 s).
constexpr u32 kMaxModelLoops = 12;
constexpr u32 kMaxOtherFootsteps = 6;
// A loop starts within its cutoff and stops this much further (no flapping at the edge).
constexpr f32 kLoopHysteresis = 1.1f;

// The time (ms into a cycle of `cycleMs`) that a looping animation clock `nowMs` is at; the window (from, to] of
// a frame crossed a timed event at `eventMs` when it lies in it, the cycle's wrap included.
inline u32 cycleTime(u32 nowMs, u32 cycleMs) {
    return cycleMs == 0 ? 0 : nowMs % cycleMs;
}
bool crossed(u32 fromMs, u32 toMs, u32 eventMs, u32 cycleMs);

} // namespace vd::audio
