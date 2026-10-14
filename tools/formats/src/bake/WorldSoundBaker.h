#pragma once

#include "core/Types.h"
#include "formats/AdtSound.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/WorldSoundFormat.h"

#include <array>
#include <functional>
#include <vector>

namespace vd::bake {

// Phase 14: the tables of worldsound.vdpk (pack/WorldSoundFormat.h).

// --- Doodad sound events -----------------------------------------------------------------------------------------

// What the bake knows of a SoundEntries row: whether the client has it and whether it loops (flag 0x200).
struct SoundRowInfo {
    bool exists = false;
    bool loop = false;
};
using SoundRowLookup = std::function<SoundRowInfo(u32 soundEntry)>;

struct ModelSounds {
    u64 model = 0; // assetPathHash of the .m2
    u32 cycleMs = 0;
    std::vector<pack::ModelSoundEvent> events;
};

// The sound events of a doodad M2 ($DSL, $DSO and $SND naming a row the client has), in its Stand animation (the
// first sequence of animation 0, which a doodad loops): a looping row, or a $DSL without times there, is a loop while
// the listener is near; any other event a one-shot at each of its times in that sequence. Empty `events` when
// the model has none.
ModelSounds modelSounds(const formats::M2Model& model, u64 hash, const SoundRowLookup& rows);
// The events of `model` placed by `transform` (model -> world, 3x4 by rows) as "modelsounds/<map>" records.
void placeDoodadSounds(const ModelSounds& model, const std::array<f32, 12>& transform,
                       std::vector<pack::DoodadSound>& out);
// "modelsounds/<map>": the records sorted by cell.
std::vector<u8> bakeDoodadSounds(std::vector<pack::DoodadSound> records);

// --- WMO areas ---------------------------------------------------------------------------------------------------

struct WmoAreaGroupData {
    pack::WmoAreaGroup group{}; // firstCell set by bakeWmoAreas
    std::vector<u8> cells;      // cellsX * cellsY pairs
};
struct WmoAreaModelData {
    u64 model = 0;
    i32 wmoId = 0;
    std::vector<WmoAreaGroupData> groups;
};
// A WMO's groups with their ids, boxes and floors (pack::kWmoFloorCell grid of the collision triangles vMaNGOS' vmap
// extractor keeps, walls left out).
WmoAreaModelData wmoAreaModel(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups, u64 hash);

struct WmoAreaPlacementData {
    u32 uniqueId = 0;
    u16 nameSet = 0;
    u32 model = 0; // index into the models given to bakeWmoAreas
    std::array<f32, 12> transform{};
};
// "wmoareas/<map>": the placements (world boxes computed from their groups), the models and their groups and cells.
std::vector<u8> bakeWmoAreas(const std::vector<WmoAreaPlacementData>& placements,
                             const std::vector<WmoAreaModelData>& models);

// The floor heights of one group at a model-space point (tests and probes): false outside its grid or where it has
// no floor; lowest and highest in model space.
bool wmoFloorAt(const WmoAreaGroupData& group, f32 x, f32 y, f32& lowest, f32& highest);

// --- Terrain emitters --------------------------------------------------------------------------------------------

// "emitters/<map>".
std::vector<u8> bakeSoundEmitters(const std::vector<formats::AdtSoundEmitter>& emitters);

} // namespace vd::bake
