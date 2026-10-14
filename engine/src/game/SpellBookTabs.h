#pragma once

#include "core/Types.h"
#include "game/AbilityData.h"

#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

// The spell book's pages (phase 8), as the client groups the known spells: "General" (racials, Attack, the
// weapon skills' spells, the professions' own spells: phase 13, as WoW.exe files the skill lines whose
// SkillRaceClassInfo has flag 0x80) and one tab per class skill line that has a talent tree (Arcane, Fire, Frost).
// Spells of armor / weapon / language lines that are passive (proficiencies), hidden spells and, phase 13, the
// recipes (SPELL_ATTR_IS_TRADESKILL) and crafts (castUI: Enchanting, Beast Training) are not listed: their windows
// list them (game/TradeSkills). Within a tab: by name, then rank; by default only the highest rank of each spell.
enum class SpellTabKind : u8 { General, Class };

struct SpellBookTab {
    SpellTabKind kind = SpellTabKind::General;
    u32 skillLine = 0;     // Class tabs
    std::string_view name; // DBC name (Class tabs); empty for General / Professions (the UI names them)
    u32 spellIcon = 0;     // SpellIcon.dbc of the skill line (AbilityData::spellText().iconHash)
    u32 first = 0;         // into entries()
    u32 count = 0;
};

struct SpellBookEntry {
    u32 spellId = 0;
    std::string_view name;
    std::string_view rank; // "Rank 3" (DBC), may be empty
    u32 rankNumber = 0;    // 3 for "Rank 3", 0 without a rank
    bool passive = false;
    bool highest = true; // the highest known rank of its spell
};

class SpellBookTabs {
public:
    // Rebuilt only when the known spells, the character or the rank toggle changed (allocates then only).
    void update(const AbilityData& data, std::span<const u32> known, u8 race, u8 cls, bool allRanks);
    void invalidate() { m_signature = 0; }

    std::span<const SpellBookTab> tabs() const { return m_tabs; }
    std::span<const SpellBookEntry> entries(u32 tab) const;
    // Where a spell is listed (tab, index in it); false when it is not (hidden, unknown).
    bool find(u32 spellId, u32& tab, u32& index) const;
    // The highest known rank of the spell `spellId` is a rank of (by name), itself if none higher.
    u32 highestRank(u32 spellId) const;

private:
    // A listed spell with its tab key: 0 General, 1 + tree index.
    struct Keyed {
        u32 key = 0;
        u32 skillLine = 0;
        SpellBookEntry entry;
    };

    u64 m_signature = 0;
    std::vector<SpellBookTab> m_tabs;
    std::vector<SpellBookEntry> m_entries; // grouped by tab
    std::vector<Keyed> m_keyed;            // scratch of update()
};

} // namespace vd::game
