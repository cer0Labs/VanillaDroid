#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class SpellText;

// Phase 20 (docs/MOBILE_CONTROLS.md §5.4): the ranks of the player's spells. A touch button keeps a spell and casts
// the highest rank the player knows of it (1.12 swaps ranks on its bars with SMSG_SUPERCEDED_SPELL; the touch layout
// keeps the line instead). A line is the spells of one name, as the spell book groups them (Spell.dbc's rank text
// "Rank 3" orders them). Rebuilt when the known spells change (allocates then only); lookups do not allocate.
class SpellLines {
public:
    // `version`: SpellBook::knownVersion() (nothing is done while it is the same).
    void update(std::span<const u32> known, const SpellText& text, u32 version);
    // The highest known rank of the line `spellId` belongs to; `spellId` itself when the player knows none of it.
    u32 highest(u32 spellId) const;
    // The player knows `spellId` or another rank of it.
    bool knows(u32 spellId) const;
    // The rank number of a rank text ("Rank 3": 3; none: 0).
    static u32 rankNumber(std::string_view rank);
    u32 version() const { return m_version; }

private:
    struct Entry {
        u64 name = 0; // the name's hash
        u32 rank = 0;
        u32 spellId = 0;
    };
    const SpellText* m_text = nullptr;
    std::vector<Entry> m_entries; // the known spells by name, then rank
    u32 m_version = 0;
};

} // namespace vd::game
