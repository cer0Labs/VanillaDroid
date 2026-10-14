#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"

#include <string>
#include <string_view>

namespace vd::pack {

// worldsound.vdpk (phase 14, `vd_baker bake --only worldsound`): where the world itself sounds, and which room of a
// building a point is in. A small pack of its own (sound.vdpk, with the sound files, stays as it was):
//
//   "modelsounds/<map>" DoodadSoundsHeader   the sound events ($DSL / $DSO / $SND) of the doodads the map places
//   "wmoareas/<map>"    WmoAreasHeader       the map's buildings with WMOAreaTable rows: placements, groups and the
//                                            floors of every group (the interior music, ambience and names)
//   "emitters/<map>"    SoundEmittersHeader  the terrain's sound emitters (ADT MCSE)
//
// <map> is the Map.dbc directory in lowercase ("azeroth"). The tables are LZ4 compressed, read on an IO thread when
// the map opens.
constexpr const char* kWorldSoundPack = "worldsound.vdpk";
constexpr u32 kWorldSoundVersion = 2; // 2: the doodads' sounds placed per map

inline std::string wmoAreasPath(std::string_view mapDirectory) {
    std::string path = "wmoareas/";
    for (const char c : mapDirectory) {
        path += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return path;
}

inline std::string soundEmittersPath(std::string_view mapDirectory) {
    std::string path = "emitters/";
    for (const char c : mapDirectory) {
        path += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return path;
}

// --- "modelsounds/<map>": where the doodads sound (AssetType::ModelSounds) ---------------------------------------
// Every sound event of every M2 doodad the map places (ADT MDDF, and the doodad sets the runtime shows of every WMO:
// the default one and the placement's), at its world point (the event's bind-pose point through the placement). A
// loop plays while the listener is near (the events of a looping SoundEntries row, and $DSL ones without times); a
// timed one-shot plays at its time in every cycle of the model's Stand animation, which the doodads loop (a bellows
// breathing in at 1.1 s and out at 0 s of its 2.2 s cycle). Baked per map so that the game thread finds the ones
// near the listener in a small sorted table instead of walking the world's instances: sorted by kDoodadSoundCell
// cells (cell = cy * kDoodadSoundCells + cx from the map's corner), a query reads the cells' rows by binary search.
constexpr u32 kModelSoundLoop = 0;
constexpr u32 kModelSoundTimed = 1;
constexpr f32 kDoodadSoundCell = 32.0f;
constexpr u32 kDoodadSoundCells = 1067; // 64 tiles * 533.33 yards / 32

// The cell of a world point (clamped to the map).
inline u32 doodadSoundCell(f64 x, f64 y) {
    const f64 half = 32.0 * (1600.0 / 3.0);
    const auto clampCell = [](f64 v) {
        return static_cast<u32>(v < 0.0 ? 0.0 : v >= kDoodadSoundCells - 1 ? kDoodadSoundCells - 1 : v);
    };
    return clampCell((half - x) / kDoodadSoundCell) * kDoodadSoundCells + clampCell((half - y) / kDoodadSoundCell);
}

struct DoodadSoundsHeader {
    u32 version; // kWorldSoundVersion
    u32 count;   // DoodadSound records, sorted by cell
    u32 reserved[2];
};
static_assert(sizeof(DoodadSoundsHeader) == 16, "DoodadSoundsHeader layout");

struct DoodadSound {
    f32 position[3]; // world
    u32 sound;       // SoundEntries id
    u32 cell;        // doodadSoundCell(position)
    u16 kind;        // kModelSound*
    u16 reserved;
    u32 timeMs;  // kModelSoundTimed: within the cycle
    u32 cycleMs; // the model's Stand animation
};
static_assert(sizeof(DoodadSound) == 32, "DoodadSound layout");

// The model-space events of one M2, as the bake finds them (not stored on their own).
struct ModelSoundEvent {
    u32 sound;
    u16 kind;
    u16 reserved;
    f32 position[3];
    u32 timeMs;
};
static_assert(sizeof(ModelSoundEvent) == 24, "ModelSoundEvent layout");

inline std::string modelSoundsPath(std::string_view mapDirectory) {
    std::string path = "modelsounds/";
    for (const char c : mapDirectory) {
        path += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return path;
}

// --- "wmoareas/<map>": the buildings' rooms (AssetType::WmoAreas) ------------------------------------------------
// WMOAreaTable.dbc names a building's rooms by (WMOID: MOHD, NameSetID: the placement's MODF, WMOGroupID: MOGP): their
// music, ambience, intro and name. A point is in the group whose box holds it and whose floor is nearest under it,
// the rule of the vmaps vMaNGOS builds from the client (GroupModel::IsInsideObject: a ray down inside the group's
// box; the nearest hit wins) unless the terrain is between that floor and the point (TerrainInfo::GetAreaInfo).
// The floors are baked as a grid over each group's box: per kWmoFloorCell-yard cell the lowest and highest height
// of the group's collision surfaces there (vertical walls left out), in the group's floorStep units above its box
// (a quarter of a yard, more for groups taller than 63 yards). Only the buildings whose rows say something (music,
// ambience, intro, area or name: 443 of the 917 WMOs with rows) are baked.
constexpr f32 kWmoFloorCell = 2.0f;
constexpr f32 kWmoFloorStep = 0.25f; // the finest step
constexpr u8 kWmoNoFloor = 0xFF;

struct WmoAreasHeader {
    u32 version; // kWorldSoundVersion
    u32 placementCount;
    u32 modelCount;
    u32 groupCount;
    u32 cellCount; // u8 pairs
    u32 placementsOffset;
    u32 modelsOffset;
    u32 groupsOffset;
    u32 cellsOffset;
    u32 reserved[3];
};
static_assert(sizeof(WmoAreasHeader) == 48, "WmoAreasHeader layout");

// A building of the map (ADT or WDT MODF) whose WMO has rows.
struct WmoAreaPlacement {
    f32 transform[12]; // model -> world, 3x4 row-major (as TerrainWmoInstance)
    f32 boundsMin[3];  // world box of the groups
    f32 boundsMax[3];
    u32 uniqueId;
    u16 nameSet;
    u16 model; // into the models
};
static_assert(sizeof(WmoAreaPlacement) == 80, "WmoAreaPlacement layout");

struct WmoAreaModel {
    u64 model; // assetPathHash of the root .wmo
    i32 wmoId; // MOHD
    u32 firstGroup;
    u32 groupCount;
    u32 reserved;
};
static_assert(sizeof(WmoAreaModel) == 24, "WmoAreaModel layout");

struct WmoAreaGroup {
    f32 boundsMin[3]; // model space (MOGP)
    f32 boundsMax[3];
    i32 groupId;   // MOGP's WMOAreaTable id
    u32 flags;     // MOGP flags (0x8 exterior, 0x2000 interior...)
    u32 firstCell; // cellsX * cellsY u8 pairs [lowest, highest], rows along model Y
    u16 cellsX;
    u16 cellsY;
    f32 floorStep; // yards per unit of the cells' heights
    u32 reserved;
};
static_assert(sizeof(WmoAreaGroup) == 48, "WmoAreaGroup layout");

// --- "emitters/<map>": the terrain's sound emitters (AssetType::SoundEmitters) -----------------------------------
// ADT MCSE (52 bytes per emitter in 1.12): a sound at a point, heard between two times of the day, as groups of
// instances with silences between them. The 1.12 data holds 36 of them in 7 tiles (most name SoundEntries rows the
// client does not have: test placements).
struct SoundEmittersHeader {
    u32 version; // kWorldSoundVersion
    u32 count;
    u32 reserved[2];
};
static_assert(sizeof(SoundEmittersHeader) == 16, "SoundEmittersHeader layout");

struct SoundEmitter {
    f32 position[3]; // world
    u32 sound;       // SoundEntries id
    f32 minDistance; // 0: the SoundEntries row's
    f32 maxDistance;
    f32 cutoffDistance;
    u16 startMinute; // of the day (0..1440): heard from
    u16 endMinute;   // until
    u16 mode;
    u8 loopCountMin; // times a sound plays in a row
    u8 loopCountMax;
    u16 silenceMinMs; // between two groups
    u16 silenceMaxMs;
    u16 instancesMin; // sounds in a group
    u16 instancesMax;
    u16 gapMinMs; // between two sounds of a group
    u16 gapMaxMs;
    u32 id; // MCSE soundPointID
};
static_assert(sizeof(SoundEmitter) == 52, "SoundEmitter layout");

} // namespace vd::pack
