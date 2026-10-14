#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/Adt.h"

#include <span>
#include <string>
#include <vector>

namespace vd::formats {

// Phase 14: the terrain's sound emitters of an ADT (MCSE, one array per MCNK; 52 bytes per emitter in 1.12, laid out
// from the client's data: `vd_baker scan-emitters`). Read without parsing the rest of the tile.
struct AdtSoundEmitter {
    u32 id = 0;        // soundPointID
    u32 sound = 0;     // SoundEntries id
    f32 position[3]{}; // world X, Y, Z
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f;
    f32 cutoffDistance = 0.0f;
    u16 startMinute = 0; // of the day: the emitter sounds from startMinute to endMinute
    u16 endMinute = 0;
    u16 mode = 0;
    u8 loopCountMin = 0; // a sound plays this many times in a row (the test "clock" emitters: the hour)
    u8 loopCountMax = 0;
    u16 silenceMinMs = 0; // between two groups of sounds
    u16 silenceMaxMs = 0;
    u16 instancesMin = 0; // sounds in a group
    u16 instancesMax = 0;
    u16 gapMinMs = 0; // between two sounds of a group
    u16 gapMaxMs = 0;
    u32 chunkX = 0; // the MCNK that lists it
    u32 chunkY = 0;
};

constexpr u32 kAdtSoundEmitterSize = 52;

Result<std::vector<AdtSoundEmitter>> parseAdtSoundEmitters(std::span<const u8> data);

// The objects an ADT places, without parsing the rest of the tile: its doodads (MDDF with their MMDX names) and its
// buildings (MODF with their MWMO names, nameSet included: WMOAreaTable's NameSetID).
struct AdtPlacements {
    std::vector<std::string> models; // by MDDF nameId
    std::vector<AdtDoodadPlacement> doodads;
    std::vector<std::string> wmos; // by MODF nameId
    std::vector<AdtWmoPlacement> placements;
};
Result<AdtPlacements> parseAdtPlacements(std::span<const u8> data);

} // namespace vd::formats
