#pragma once

#include "core/Types.h"
#include "pack/UiModelFormat.h"

#include <array>
#include <span>
#include <vector>

namespace vd::ui {

class Assets;
class DrawList;
struct Rect;

// Retouches (ui models): the models the 1.12 interface shows in its Model frames ("ui/models" of ui.vdpk,
// pack/UiModelFormat.h), evaluated on the CPU and drawn as the UI's own quads: the cooldown's sweep and the flash at
// its end (Cooldown.xml / Cooldown.lua), the pet's autocast sparkles (PetActionBarFrame.xml, SpellBookFrame.xml) and
// the minimap's ping (Minimap.xml / Minimap.lua). A Model frame without a camera shows its model flat: the model's
// origin on the frame's bottom-left corner, X right, Y up, one model unit = 1280 interface units of the frame times the
// model's scale, clipped to the frame (WoW.exe 0x76D240 / 0x7AD7F0 / 0x76D1A0, pack/UiModelFormat.h). No 3D pass, no
// allocation once loaded: a cooldown is a few quads, an autocast button its live particles.
//
// The interface's buttons keep no state, so the cooldowns' flash (the end of a cooldown) and the autocast's start
// are tracked here per widget id. Game thread only (each DrawList owns one).
class UiModels {
public:
    using Id = pack::UiModelId;

    void bind(const Assets* assets) { m_assets = assets; }
    // Parses the blob once ui.vdpk is open; false while it is not (or the pack predates the models).
    bool ready();

    // Where a model goes: its origin (the Model frame's bottom-left corner, logical pixels, y down), the pixels of one
    // model unit (1280 * model scale * pixels per interface unit of the frame), the frame (its viewport: everything is
    // clipped to it). `round` > 0: also inside the circle of that radius centered in the frame (the touch cluster's
    // round buttons: the 1-D textures masked by it antialiased, the sprites cut by it).
    struct Placement {
        f32 x = 0.0f, y = 0.0f;
        f32 scale = 1.0f;
        f32 frameX = 0.0f, frameY = 0.0f, frameW = 0.0f, frameH = 0.0f;
        f32 round = 0.0f;
        f32 alpha = 1.0f;      // the frame's alpha
        f32 worldScale = 0.0f; // pixels of one unit of the frame's world (particles: 1280 * pixels per unit)
    };
    // The model at `timeMs` into its sequence `sequence` (index), global sequences at `globalMs`.
    void drawModel(DrawList& draw, Id model, u32 sequence, f32 timeMs, f32 globalMs, const Placement& at) const;

    // CooldownFrameTemplate on a button (CooldownFrame_SetTimer / CooldownFrame_OnUpdateModel): while `remaining`
    // (the share of the cooldown still to come, 0..1) is above 0 the model's Stand sequence at (1 - remaining) *
    // 1000 ms (the dark sweep uncovering the button clockwise from twelve o'clock); when a cooldown runs out (not when
    // it is cut short: Hide), its Death sequence (the blue star flash) plays once. `frame`: the Model frame (logical
    // pixels), `unit` pixels per interface unit of it, `modelScale` the template's 0.75. `leftSeconds` < 0 when the
    // caller only knows the share (the end is then estimated from how the share falls). No seconds are written: 1.12
    // writes none.
    void cooldown(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 remaining, f32 leftSeconds, u64 nowMs,
                  f32 modelScale = 0.75f);
    // The same model over the round icon of radius `radius` at (cx, cy) (the touch cluster): the model's square
    // fitted to the circle's bounding square, the sweep masked by the circle.
    void cooldownRound(DrawList& draw, u64 id, f32 cx, f32 cy, f32 radius, f32 remaining, f32 leftSeconds,
                       u64 nowMs);

    // UI-AutoCastButton.m2 while an action's autocast is on: four sparks running round the button, leaving their
    // particles behind. Its time starts when it shows (PetActionButton / SpellButton: SetSequenceTime(0, 0) at load,
    // advancing while shown). `frame` the Model frame, `unit` pixels per interface unit, `modelScale` its XML scale
    // (1.2 on the pet bar, 1.22 in the spell book).
    void autocast(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 modelScale, u64 nowMs);

    // MiniMapPing (Minimap.xml: 50 x 50 units, model scale 0.4) centered on (cx, cy), `unit` pixels per interface unit
    // of the minimap, `ageMs` since the ping: Birth, then Stand looping; `alpha` the frame's.
    void minimapPing(DrawList& draw, f32 cx, f32 cy, f32 unit, f32 ageMs, f32 alpha = 1.0f);

    // The particles drawn per emitter at most (its rate times its life; 0: all, as the client). The autocast's emitters
    // keep 300 alive each, overlapping in a few pixels: kParticleLimit of them look the same (VD_UI_GALLERY=autocast
    // shows both) for a third of the quads and the time (~25 us per button on the desktop instead of ~75).
    static constexpr u32 kParticleLimit = 100;
    void setParticleLimit(u32 perEmitter) { m_particleLimit = perEmitter; }

    // The index of the sequence of `animationId` (AnimationData.dbc) in a model, or ~0u.
    u32 sequence(Id model, u16 animationId) const;
    // Milliseconds a sequence lasts.
    f32 sequenceMs(Id model, u32 sequence) const;

private:
    // A piece of a 1-D texture between two texture coordinates: its color goes linearly from c0 to c1 (straight
    // RGBA, 0..1); u0 / u1 may be -inf / +inf (the clamped ends).
    struct Segment {
        f32 u0, u1;
        f32 c0[4], c1[4];
    };
    // A widget's model state: the cooldown's end and flash, the autocast's start.
    struct Instance {
        u64 id = 0;
        u64 seenMs = 0;
        u64 endMs = 0;       // cooldown: when it should run out (0: unknown)
        u64 flashMs = 0;     // cooldown: when its flash started (0: none)
        u64 firstMs = 0;     // cooldown (share only): the first sample of this cooldown
        f32 firstShare = 0.0f;
        f32 lastShare = 0.0f; // 0: no cooldown on the last call
        u64 sinceMs = 0;     // autocast: shown since
    };

    void sample(u32 track, u32 modelSequence, u32 time, u32 globalMs, const pack::UiModelInfo& model, f32* out,
                u32 components, const f32* fallback) const;
    Instance& instance(u64 id, u64 nowMs);
    void drawBatch(DrawList& draw, const pack::UiModelInfo& model, u32 batchIndex, u32 sequence, u32 time,
                   u32 globalMs, const Placement& at, const f32 (*bones)[12]) const;
    void drawEmitter(DrawList& draw, const pack::UiModelInfo& model, u32 emitterIndex, f32 modelMs,
                     const Placement& at) const;
    void boneMatrix(const pack::UiModelInfo& model, u32 bone, u32 sequence, u32 time, u32 globalMs, f32* out) const;
    void cooldownAt(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 modelScale, f32 round, f32 remaining,
                    f32 leftSeconds, u64 nowMs);

    const Assets* m_assets = nullptr;
    bool m_loaded = false;
    bool m_failed = false;
    pack::UiModelsHeader m_header{};
    std::span<const pack::UiModelInfo> m_models;
    std::span<const pack::UiModelSequence> m_sequences;
    std::span<const pack::UiModelBone> m_bones;
    std::span<const pack::UiModelVertex> m_vertices;
    std::span<const pack::UiModelBatch> m_batches;
    std::span<const pack::UiModelEmitter> m_emitters;
    std::span<const pack::UiModelTrack> m_tracks;
    std::span<const pack::UiModelRange> m_ranges;
    std::span<const pack::UiModelKey> m_keys;
    std::span<const u32> m_texels;
    std::span<const u32> m_globalSequences;
    std::vector<Segment> m_segments;    // the 1-D textures, cut at load
    std::vector<u32> m_batchSegments;   // per batch: first segment << 16 | count
    std::array<Instance, 160> m_instances{};
    u32 m_particleLimit = kParticleLimit;
};

} // namespace vd::ui
