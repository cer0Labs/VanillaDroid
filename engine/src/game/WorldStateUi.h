#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

class Social;
class WorldStates;

// One WorldStateUI.dbc row (1.12.1: 39 fields, 0x9C bytes; WoW.exe reads them at the offsets in brackets):
// what the interface shows of a world state, where.
struct WorldStateUiRow {
    u32 id = 0;
    i32 map = -1; // [0x04] Map.dbc, -1 any
    u32 area = 0; // [0x08] AreaTable zone, 0 any
    std::string_view
        icon; // [0x0C] "Interface\\TargetingFrame\\UI-PVP-Alliance" (a score column's: + "0" / "1")
    std::string_view text;    // [0x10] enUS: "%1581w/%1601w" (a score column's name: "Flag Captures")
    std::string_view tooltip; // [0x34] "Alliance flag captures"
    u32 stateVariable = 0;    // [0x5C] the world state whose value is the row's state (0: always 1)
    u32 type = 0;             // [0x60] kWorldStateUi*
    std::string_view
        dynamicIcon; // [0x64] "Interface\\WorldStateFrame\\HordeFlag" (state 2), its "Flash" file too
    std::string_view dynamicTooltip; // [0x68] "Horde flag has been picked up"
    std::string_view extendedUi; // [0x8C] "CAPTUREPOINT": WorldStateFrame.lua's ExtendedUI (a capture bar)
    std::array<u32, 3>
        extendedStates{}; // [0x90] world states the extended UI reads (CaptureBar_Update: the first)
};
// The row types (the [0x60] field): WoW.exe's list (0x4C56E0) takes 0 always and 1 only while the player is
// in a LocalDefense channel (0xB6E5DC, 0x49BD50); 2 are the battleground's score columns (0x4AA9BA).
constexpr u32 kWorldStateUiAlwaysUp = 0;
constexpr u32 kWorldStateUiZone = 1;
constexpr u32 kWorldStateUiScoreColumn = 2;
// WorldStateFrame.lua MAX_NUM_STAT_COLUMNS (Alterac Valley has all seven).
constexpr u32 kMaxStatColumns = 7;

// The WorldStateUI table and the ChatChannels rows of the zone defense channels (flags DEFENSE 0x10000 and
// ZONE_DEP 0x2: LocalDefense), indexed once. Strings are views into the mapped db pack.
class WorldStateUiData {
public:
    bool init(const pack::PackReader& db);
    // Tests and samples: the rows (their strings must outlive this), the defense channels' ChatChannels ids
    // and their names' patterns ("LocalDefense - %s").
    void set(std::vector<WorldStateUiRow> rows, std::vector<u32> defenseChannels,
             std::vector<std::string_view> defensePatterns);
    bool ready() const { return !m_rows.empty(); }

    std::span<const WorldStateUiRow> rows() const { return m_rows; }
    const WorldStateUiRow* row(u32 id) const;
    // A channel the 0x49BD50 check counts (LocalDefense - <zone>): its ChatChannels id (an automatic zone
    // channel), or its name (one joined by hand, or by the server: WoW.exe finds its row by the name too).
    bool defenseChannel(u32 chatChannel, std::string_view name) const;
    // The battleground's score columns (GetNumBattlefieldStats / GetBattlefieldStatInfo): as
    // SMSG_BATTLEFIELD_STATUS "active" collects them (0x4AA99D), the first run of type-2 rows of `map` (or of
    // any map) in table order, at most `out.size()`. Returns how many.
    u32 statColumns(u32 map, std::span<const WorldStateUiRow*> out) const;

private:
    std::vector<WorldStateUiRow> m_rows;
    std::vector<u32> m_defenseChannels;
    std::vector<std::string_view> m_defensePrefixes; // the patterns' text before "%s"
};

// GetWorldStateUIInfo(i)'s results (0x4C5A70): the state (the row's state variable's value, 1 without one: 0
// hidden, 1 shown, 2 the dynamic icon flashing), the text with its world states written in, and the rest of
// the row.
struct WorldStateUiInfo {
    u32 id = 0;
    u32 state = 0;
    std::string_view text; // in the caller's buffer
    std::string_view icon, dynamicIcon, tooltip, dynamicTooltip, extendedUi;
    std::array<i32, 3> extendedStates{}; // their world states' values
};

// "%1581w" -> the value of world state 1581, as WoW.exe 0x508560 writes the WorldStateUI texts: a '%',
// digits, then 'w' or 'W' is the state's value (%d, 0 when not set); a '%' followed by anything else stays a
// '%' (the digits after it are dropped). Writes into `out` (truncated, NUL-terminated) and returns the text.
std::string_view formatWorldStateText(std::string_view text, const WorldStates& states, std::span<char> out);

// Phase 17 (scores): GetNumWorldStateUI / GetWorldStateUIInfo over game::WorldStates, as WoW.exe 1.12.1
// builds them (read-only disassembly 0x4C5650..0x4C5BFA):
//  - The list (0x4C56E0) is rebuilt on every SMSG_INIT_WORLD_STATES and whenever the player joins or leaves a
//    LocalDefense channel: every WorldStateUI row, in table order, whose map is the INIT's (or -1), whose
//    area is the INIT's zone (or 0), of type 0, or of type 1 while in a LocalDefense channel (0x49BD50: a
//    channel whose ChatChannels flags have 0x10000 and 0x2). A LocalDefense change fires UPDATE_WORLD_STATES
//    itself (0x49BDD6). Nothing before the first INIT of a world.
//  - Each entry's state, text and the rest are read when asked (GetWorldStateUIInfo).
// Main thread; the list keeps its capacity.
//
// Events (Gameplay::events()): WorldStates (id 0) when the LocalDefense check changes the list.
class WorldStateUi {
public:
    using Events = EventRing<GameEvent, 64>;

    WorldStateUi(const WorldStates& states, const Social& social, Events& events);
    void setData(const WorldStateUiData* data) { m_data = data; }
    const WorldStateUiData* data() const { return m_data; }

    // Once a frame: the list follows the INIT's map and zone and the LocalDefense channel.
    void update(u32 nowMs);
    void leaveWorld();

    // GetNumWorldStateUI and GetWorldStateUIInfo(index + 1); its text is written into `text`.
    u32 count() const { return static_cast<u32>(m_list.size()); }
    WorldStateUiInfo info(u32 index, std::span<char> text) const;
    // Whether the player is in a LocalDefense channel (0xB6E5DC).
    bool inLocalDefense() const { return m_localDefense; }
    // Lists built so far (a view rebuilds when it moves).
    u32 changes() const { return m_changes; }

    // Development (VD_WORLDSTATE_DEMO, the gallery): the type-1 rows without a LocalDefense channel.
    void setForceZoneRows(bool force) { m_forceZone = force; }

private:
    bool localDefenseNow() const;
    void rebuild();

    const WorldStates& m_states;
    const Social& m_social;
    Events& m_events;
    const WorldStateUiData* m_data = nullptr;
    std::vector<const WorldStateUiRow*> m_list; // 0xB71E7C
    bool m_built = false;
    u32 m_map = 0, m_zone = 0;
    const WorldStateUiData* m_listData = nullptr; // the data the list was built from
    bool m_localDefense = false;
    bool m_forceZone = false;
    u32 m_changes = 0;
};

} // namespace vd::game
