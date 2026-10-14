#pragma once

#include "core/Types.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Phase 14 (spellfx): what the 1.12 client does with the parts of a SpellVisualKit that are not models: the
// character procs (SpellVisualKit.dbc fields 15-18, their parameters in 19-34: parameter p of proc i in field
// 19 + p * 4 + i) and the camera shake (field 14), with the tables they point at (SpellChainEffects,
// SpellEffectCameraShakes, CameraShakes), the precast transitions (SpellVisualPrecastTransitions) and the
// shakes of big creatures (CreatureModelData). Rules taken from WoW.exe 1.12.1 (read-only disassembly; the
// addresses are in the comments of SpellKitFx.cpp), the field layouts from the 1.12.1 files, checked against
// their rows in tests/game/test_spell_kit_fx.cpp. Read-only after init(): lookups never allocate.

// SpellChainEffects.dbc (8 fields): how a beam looks. WoW.exe builds it as a strip facing the camera through
// joints `segmentLength` apart, each jittered by up to `noise` times the beam's length, `width` to each side,
// the texture stretched once over the whole beam and scrolled by one length every |scroll| seconds (from the
// first point to the last when positive), added over the scene.
struct ChainEffect {
    u32 id = 0;
    f32 segmentLength = 2.78f; // m_avgSegLen, yards
    f32 width = 0.25f;         // m_width: half the strip's width, yards
    f32 noise = 0.01f;         // m_noiseScale
    f32 scroll = 0.0f;         // m_texCoordScale: seconds per texture length, sign = direction (0: still)
    u32 segmentMs = 1000;      // m_segDuration: how long one jump of a chain shows
    u32 delayMs = 200;         // m_segDelay: between one jump and the next
    u64 texture = 0;           // textures.vdpk hash of m_texture
};

// CameraShakes.dbc (8 fields). CGCamera adds it with the amplitude divided by 36 (yards); its offset at time
// t (seconds since it started, plus `phase`) is amplitude * sin(2 pi * frequency * t), times exp(-coefficient
// * t) when it decays, along the facing of the unit the camera follows, across it or up.
struct CameraShake {
    enum class Direction : u8 { Forward = 0, Side = 1, Up = 2 };
    u32 id = 0;
    bool decays = false; // m_shakeType 1
    Direction direction = Direction::Up;
    f32 amplitude = 0.0f; // yards (the DBC value / 36)
    f32 frequency = 0.0f; // Hz
    f32 duration = 0.0f;  // seconds
    f32 phase = 0.0f;     // seconds added to the time (they count against the duration)
    f32 coefficient = 0.0f;
};

// What a kit's procs and shake do (only kits with any of them are kept).
struct KitFx {
    u32 kit = 0;
    // Procs 0 and 12: a chain of beams from the caster through the targets the server names (a channel's,
    // SMSG_SPELL_UPDATE_CHAIN_TARGETS; a cast's, the units SMSG_SPELL_GO hit). `chainBeams` beams side by
    // side (1-3); held while the kit lasts (channels) or each jump for ChainEffect::segmentMs.
    u32 chainEffect = 0;
    u8 chainBeams = 0;
    bool chainHeld = false;
    // Proc 1: the unit's model takes this color (0xRRGGBB, multiplied) while the kit lasts.
    bool tints = false;
    u32 tint = 0xFFFFFF;
    // Proc 13: the unit's model flashes this color for `flashHoldSeconds`, then back over `flashFadeSeconds`.
    bool flashes = false;
    u32 flashColor = 0xFFFFFF;
    f32 flashHoldSeconds = 0.0f;
    f32 flashFadeSeconds = 0.0f;
    // Proc 10: a line from the caster's fishing pole to the object it channels at (Fishing).
    bool line = false;
    // SpellEffectCameraShakes.dbc id (field 14), 0 = none.
    u32 shake = 0;
};

class SpellKitFx {
public:
    // Proc types of SpellVisualKit (WoW.exe 1.12.1, the switch at 0x60D7C0).
    static constexpr u32 kProcChain = 0, kProcTint = 1, kProcLine = 10, kProcCastChain = 12, kProcFlash = 13;
    static constexpr u16 kNoAnimation = 0xFFFF;

    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    const KitFx* kit(u32 id) const;
    const ChainEffect* chainEffect(u32 id) const;
    const CameraShake* cameraShake(u32 id) const;
    // SpellEffectCameraShakes.dbc: its three CameraShakes ids (0: none).
    std::array<u32, 3> effectShakes(u32 id) const;
    // SpellVisualPrecastTransitions.dbc (by AnimationData names): the animation that loops once the precast's
    // `load` animation (LoadBow, LoadRifle, LoadThrown) has played; kNoAnimation for any other.
    u16 holdAfter(u16 load) const;
    // CreatureModelData.dbc fields 11 and 12: the CameraShakes of each footstep and of the death thud of the
    // creature model with this actors.vdpk hash (0: none).
    u32 footstepShake(u64 model) const;
    u32 deathShake(u64 model) const;

    // Tests: tables by hand.
    void addKit(const KitFx& kit);
    void addChainEffect(const ChainEffect& chain);
    void addCameraShake(const CameraShake& shake);
    void addEffectShakes(u32 id, const std::array<u32, 3>& shakes);
    void addTransition(u16 load, u16 hold);
    void addModelShakes(u64 model, u32 footstep, u32 death);

private:
    struct ModelShakes {
        u32 footstep = 0;
        u32 death = 0;
    };
    bool m_ready = false;
    std::unordered_map<u32, KitFx> m_kits;
    std::unordered_map<u32, ChainEffect> m_chains;
    std::unordered_map<u32, CameraShake> m_shakes;
    std::unordered_map<u32, std::array<u32, 3>> m_effectShakes;
    std::vector<std::pair<u16, u16>> m_transitions; // (load, hold)
    std::unordered_map<u64, ModelShakes> m_modelShakes;
};

// The CameraShakes record a 1.12.1 row decodes to (amplitude already in yards). Public for the tests.
CameraShake cameraShakeFromRow(u32 id, u32 type, u32 direction, f32 amplitude, f32 frequency, f32 duration,
                               f32 phase, f32 coefficient);
// A kit's procs and parameters (as the DBC lays them: params[p * 4 + i] of proc i) to what this phase draws.
KitFx kitFxFromProcs(u32 kitId, const std::array<i32, 4>& procs, const std::array<f32, 16>& params,
                     u32 shake);

} // namespace vd::game
