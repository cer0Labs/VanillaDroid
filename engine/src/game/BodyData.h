#pragma once

#include "core/Types.h"
#include "game/DbcIndex.h"
#include "pack/BodyFxFormat.h"

#include <array>
#include <unordered_map>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

class AreaData;

// Phase 14 (bodyfx): what the 1.12 client does with its units' bodies, from its tables (db.vdpk) and the
// rules of WoW.exe (addresses in the comments): blood spurts, footstep sprays and footprints, breath, the
// mount's poof, and the sequence boxes the blob shadows cover (bodyfx.vdpk). Indexed once; every lookup is
// cheap and allocates nothing.

// CreatureModelData.dbc flags (field 1) WoW.exe reads for these effects.
constexpr u32 kBodyNoFootstepSprays = 0x1; // 0x5FC0DD: no spray at its steps (whelps, ghosts, wisps)
constexpr u32 kBodyNoBreath = 0x2;         // 0x600003: no breath at its $BTH events (basilisks, ghosts)

// The client's violence levels (CVar violenceLevel, 0..2): 2 is its default but in one region (WoW.exe
// 0x86C3F8, by locale: 1 there, green blood). The enUS client plays at 2.
constexpr u32 kDefaultViolence = 2;

// One CreatureModelData.dbc row as the body's effects use it (5875, vMaNGOS CreatureModelDataEntry names).
struct BodyModel {
    u32 flags = 0;              // kBody*
    u32 bloodLevel = 0;         // UnitBloodLevels.dbc id (field 5), 0 = none (the table has -1)
    u64 footprint = 0;          // textures.vdpk hash of its FootprintTextures.dbc texture (field 6), 0 = none
    f32 footprintLength = 0.0f; // yards (field 7, in inches: / 36; WoW.exe 0x5FC310)
    f32 footprintWidth = 0.0f;  // yards (field 8)
    f32 collisionWidth = 0.0f;  // yards (field 14)
    f32 collisionHeight = 0.0f; // yards (field 15): the swimming depth, the wading and splash depths
};

// An effect model with its SpellVisualEffectName.dbc scale (field 4; 0 counts as 1).
struct BodyEffect {
    u64 model = 0; // actors.vdpk asset hash, 0 = none
    f32 scale = 1.0f;
};

// The effects WoW.exe looks up by name in SpellVisualEffectName.dbc ("HARDCODED ...", the table at 0x8617B8),
// on the unit's attachment of 0x80C968.
enum class HardcodedEffect : u8 {
    WaterRunSpray,     // a step in shallow water (attachment: none, at the foot)
    WaterWalkSpray,    //
    BreathUnderwater,  // bubbles (attachment 17, Breath)
    BreathCold,        // condensation (17)
    MountPoof,         // mounting (19, Base)
    InebriatedBubbles, // the drunk player's (17)
    MeetingStoneJoin,  // phase 16 (alerts): queued at a meeting stone (19; MeetingStoneInfo.cpp 0x4CA338)
    Count
};
constexpr u32 kAttachChestBloodFront = 15; // where the blood spurts (WoW.exe 0x6250DE / 0x6250C1)
constexpr u32 kAttachChestBloodBack = 16;
constexpr u32 kAttachBreath = 17;
constexpr u32 kAttachBase = 19;

class BodyData {
public:
    static constexpr u32 kTerrainSounds = 16; // TerrainType.dbc SoundID range (1..9 used)

    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    // The CreatureModelData row of a model (its actors.vdpk hash), nullptr when no unit uses it.
    const BodyModel* model(u64 modelHash) const;

    // The blood spurt a hit shows (WoW.exe 0x625010): UnitBloodLevels gives the UnitBlood row for the
    // violence level (none at 0; green at 1 for red blood), whose fields 1-4 are SpellVisualEffectName ids:
    // in front (the attacker before the victim, or unknown) or from behind, small or large
    // (HITINFO_BLOOD_SPURT). Nothing for a model without blood (elementals, totems, machines).
    BodyEffect bloodSpurt(u32 bloodLevel, bool front, bool large, u32 violence = kDefaultViolence) const;

    // TerrainType.dbc by its SoundID (the ground table of sound.vdpk, audio/GroundSounds): the spray of a
    // step (fields 2 run / 3 walk, WoW.exe 0x5FC24B; only snow has one in 1.12) and whether the ground keeps
    // footprints (field 5 flag 1, 0x699EB6: snow and sand).
    BodyEffect footstepSpray(u32 terrainSound, bool walking) const;
    bool keepsFootprints(u32 terrainSound) const;

    BodyEffect hardcoded(HardcodedEffect effect) const { return m_hardcoded[static_cast<u32>(effect)]; }

    // Breath condenses in this area (WoW.exe 0x67E9C0: AreaTable flag 0x1, AREA_FLAG_SNOW, of the area itself
    // when it has 0x2, else of its zone): Dun Morogh, Winterspring, Naxxramas, Razorfen Downs.
    static bool coldArea(const AreaData& areas, u32 areaId);

    // bodyfx.vdpk: the sequence boxes of the models (pack/BodyFxFormat.h). False when it is missing or of
    // another version (no blob shadows then).
    bool openBounds(const pack::PackReader& bodyfx);
    bool hasBounds() const { return m_boundsModels != nullptr; }
    // The box of `animation` in `model`, else its Stand's, else its first; nullptr when the model has none.
    const pack::BodyBoundsBox* bounds(u64 model, u16 animation) const;

    // Tests and tools: the tables from elsewhere.
    void setModel(u64 modelHash, const BodyModel& model) { m_models[modelHash] = model; }

private:
    BodyEffect effect(u32 id) const; // SpellVisualEffectName row -> model

    std::unordered_map<u64, BodyModel> m_models;
    std::unordered_map<u32, BodyEffect> m_effects; // SpellVisualEffectName id -> model
    // UnitBloodLevels: per level, the UnitBlood id at violence 0..2; UnitBlood: per id, its four spurts.
    std::unordered_map<u32, std::array<u32, 3>> m_bloodLevels;
    std::unordered_map<u32, std::array<u32, 4>> m_bloods;
    std::array<BodyEffect, kTerrainSounds> m_runSpray{};
    std::array<BodyEffect, kTerrainSounds> m_walkSpray{};
    std::array<bool, kTerrainSounds> m_footprints{};
    std::array<BodyEffect, static_cast<u32>(HardcodedEffect::Count)> m_hardcoded{};
    const pack::BodyBoundsModel* m_boundsModels = nullptr; // into the mapped bodyfx pack
    const pack::BodyBoundsBox* m_boundsBoxes = nullptr;
    u32 m_boundsModelCount = 0;
    u32 m_boundsBoxCount = 0;
    bool m_ready = false;
};

} // namespace vd::game
