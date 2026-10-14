#pragma once

#include "core/Types.h"
#include "game/Abilities.h"
#include "game/AbilityData.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

// The reputation window's list as WoW.exe builds it for ReputationFrame.lua (phase 15): GetNumFactions,
// GetFactionInfo, Expand/CollapseFactionHeader, Get/SetSelectedFaction, IsFactionInactive and
// GetWatchedFactionInfo. Rules of the client (0x4D5430 .. 0x4D6C00):
// - Every faction of the reputation list with FACTION_FLAG_VISIBLE is listed. One whose flags carry 0x08 (in
//   Faction.dbc's reputation flags of the headers: Alliance, Horde, Steamwheedle Cartel, Alliance Forces,
//   Horde Forces) is a header; any other goes under the header of its parent faction, created for it when
//   missing (a faction without a parent goes under FACTION_OTHER). FACTION_INACTIVE's header always exists;
//   inactive factions (flag 0x20) go under it.
// - Headers are ordered by name, then FACTION_OTHER, then FACTION_INACTIVE; a header without factions is not
// shown.
//   Under a header, its factions by name (names compared without case). Factions under a collapsed header are
//   not shown; a new header expands every header again.
// - A faction's value is its standing plus the base reputation of the last of Faction.dbc's four race / class
// rows
//   that fits the player (rows with both masks 0 are skipped): vMaNGOS takes the first one, so the client may
//   show a few factions (Cenarion Circle for tauren and night elves) a rank apart from what the server
//   reckons, as 1.12 did.
// - At war: flag 0x02. The box can change when the value is Unfriendly or better (>= -3000) and the faction
// is not
//   peace forced (0x10). Watched: the faction's list id is PLAYER_FIELD_WATCHED_FACTION_INDEX.
// Main thread; fixed storage.

// Pseudo factions of the client's headers without a Faction.dbc row.
constexpr i32 kFactionOther = 0;
constexpr i32 kFactionInactiveHeader = -1;

struct FactionRow {
    i32 faction = 0; // Faction.dbc id; kFactionOther / kFactionInactiveHeader for those headers
    i32 listId = -1; // the reputation list id (-1: a header without one)
    bool header = false;
    bool collapsed = false; // a header's
    std::string_view
        name; // a pseudo header's is empty: FACTION_OTHER / FACTION_INACTIVE (the caller's texts)
    std::string_view description;
    u32 standing = 4; // 1 Hated .. 8 Exalted (FACTION_STANDING_LABEL<standing>, FACTION_BAR_COLORS[standing])
    i32 barMin = 0, barMax = 0, barValue = 0;
    bool atWar = false;
    bool canToggleAtWar = false;
    bool watched = false;
    bool inactive = false;
};

// The client's base reputation of a faction for a race and class (see above).
i32 clientBaseReputation(const FactionInfo& faction, u32 race, u32 cls);
// The bounds of a reputation rank 0..7 (WoW.exe's table 0x80928C: -42000, -6000, -3000, 0, 3000, 9000, 21000,
// 42000, 43000) and the rank of a value (0x4D63A0).
i32 reputationRankMin(u32 rank);
i32 reputationRankMax(u32 rank);
u32 clientReputationRank(i32 value);

class FactionList {
public:
    FactionList(); // its storage, once (on the heap: the screens holding it live on the stack)
    // Where the factions' rows come from (AbilityData in the game; tables of their own in tests).
    struct Source {
        const FactionInfo* (*byList)(const void* context, u32 listId) = nullptr;
        const FactionInfo* (*byId)(const void* context, u32 factionId) = nullptr;
        const void* context = nullptr;
    };
    static Source sourceOf(const AbilityData& data);

    // Builds the list from the reputation flags and standings (`watchedListId`:
    // PLAYER_FIELD_WATCHED_FACTION_INDEX).
    void build(const ReputationList& list, const Source& source, u32 race, u32 cls, i32 watchedListId);

    std::span<const FactionRow> rows() const {
        return {m_rows.data(), m_shown};
    } // GetNumFactions / GetFactionInfo
    // Collapse / ExpandFactionHeader of a header row (-1: every header). Applies at the next build().
    void collapse(i32 row);
    void expand(i32 row);
    // Get/SetSelectedFaction: the selection follows its faction (-1 none / not shown).
    i32 selected() const;
    void select(i32 row);
    i32 selectedFaction() const { return m_selectedFaction; }
    // GetWatchedFactionInfo: the watched faction's row (its numbers), nullptr when none.
    const FactionRow* watched() const { return m_hasWatched ? &m_watched : nullptr; }
    void reset(); // another character

private:
    struct Entry {
        i32 listId = -1;
        i32 faction = 0;
        i32 group = 0; // the header it goes under (its own id for a header)
        u32 headerIndex = 0;
        bool header = false;
    };
    struct Header {
        i32 faction = 0;
        u32 count = 0;
        std::string_view name;
        bool hasRow = false;
    };
    FactionRow rowOf(const Entry& e, const ReputationList& list, const Source& source, u32 race, u32 cls,
                     i32 watchedListId) const;

    std::vector<Entry> m_entries; // kFactionListSize * 2 + 2
    u32 m_entryCount = 0;
    std::array<Header, 32> m_headers{};
    u32 m_headerCount = 0;
    u32 m_lastHeaderCount = 0;
    std::vector<FactionRow> m_rows; // as many
    u32 m_shown = 0;
    u32 m_expandedMask = 0xFFFFFFFFu;   // bit per header in the headers' sorted order
    i32 m_selectedFaction = 0x7FFFFFFF; // none
    FactionRow m_watched;
    bool m_hasWatched = false;
};

} // namespace vd::game
