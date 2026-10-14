#pragma once

#include "core/Types.h"
#include "game/DbcIndex.h"
#include "game/SpellText.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

// SkillLineCategory.dbc: what a skill line is (the skills window groups by it, the spell book picks its
// tabs).
enum class SkillCategory : i32 {
    Attributes = 5,
    Weapon = 6,
    Class = 7,
    Armor = 8,
    Secondary = 9, // racials, riding, first aid, cooking, fishing
    Language = 10,
    Profession = 11,
    Hidden = 12,
};

struct SkillLineInfo {
    u32 id = 0;
    SkillCategory category = SkillCategory::Hidden;
    std::string_view name;
    std::string_view description;
    u32 spellIcon = 0; // SpellIcon.dbc (SpellText::iconHash)
    std::string_view baseName; // phase 21: the base client's ("Shadow Magic" under "Magia sombría"), for name rules
};

// A talent tree (TalentTab.dbc).
struct TalentTabInfo {
    u32 id = 0;
    std::string_view name;
    std::string_view baseName; // phase 21: the base client's ("Shadow" under "Sombra")
    u32 spellIcon = 0;
    u32 classMask = 0;
    u32 order = 0; // left to right
    std::string_view
        background; // Interface\TalentFrame\<background>-TopLeft.blp and its three other quarters
};

constexpr u32 kMaxTalentRanks = 5;

// A talent (Talent.dbc): its place in the tree and the spell of each rank.
struct TalentInfo {
    u32 id = 0;
    u32 tab = 0;
    u32 row = 0; // tier: 5 points spent in the tree per row
    u32 column = 0;
    std::array<u32, kMaxTalentRanks> ranks{}; // 0 past the last rank
    u32 dependsOn = 0;                        // a talent of the same tree...
    u32 dependsOnRank = 0;                    // ...at this rank (0-based) at least
    u32 dependsOnSpell = 0;

    u32 rankCount() const {
        u32 n = 0;
        while (n < kMaxTalentRanks && ranks[n] != 0) {
            ++n;
        }
        return n;
    }
};

// Reputation (Faction.dbc): a faction with a place in the reputation list.
struct FactionInfo {
    u32 id = 0;
    i32 listId = -1; // the index the reputation packets use
    std::array<u32, 4> raceMask{};
    std::array<u32, 4> classMask{};
    std::array<i32, 4> baseValue{};
    std::array<u32, 4> flags{};
    u32 parent = 0; // the group it is listed under (Alliance, Horde, Steamwheedle Cartel...)
    std::string_view name;
    std::string_view description;

    // The first of the four race/class entries that fits (FactionEntry::GetIndexFitTo), -1 none.
    i32 entryFor(u32 race, u32 cls) const;
    // The reputation every character of this race and class starts with.
    i32 baseReputation(u32 race, u32 cls) const;
};

// Reputation ranks (ReputationRank): Hated 0 .. Exalted 7.
constexpr u32 kReputationRanks = 8;
// The rank of a reputation value (base + standing), as ReputationMgr::ReputationToRank.
u32 reputationRank(i32 reputation);
// The first value of a rank (Hated: -42000).
i32 reputationRankStart(u32 rank);
// Points a rank spans (PointsInRank).
i32 reputationRankSize(u32 rank);

// The client tables of the character's abilities (phase 8): spell texts, skill lines and the spell -> skill
// line map (spell book tabs), talent trees and factions. Indexes over the mapped db pack, built once.
class AbilityData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return m_spellText.ready(); }

    const SpellText& spellText() const { return m_spellText; }

    // --- Skills ---
    bool skillLine(u32 id, SkillLineInfo& out) const;
    // The name of a skill category (SkillLineCategory.dbc: "Class Skills", "Weapon Skills"...), empty if
    // none.
    std::string_view skillCategoryName(SkillCategory category) const;
    // The skill line `spellId` belongs to for this race / class (SkillLineAbility.dbc), 0 if none.
    u32 skillOfSpell(u32 spellId, u32 race, u32 cls) const;

    // --- Talents ---
    // The class's talent trees in order (at most out.size()). Returns how many.
    u32 talentTabs(u32 cls, std::span<TalentTabInfo> out) const;
    // The talents of a tree, by row then column.
    std::span<const TalentInfo> talents(u32 tabId) const;
    const TalentInfo* talent(u32 talentId) const;
    // The talent with `spellId` among its ranks (`rank` 0-based), nullptr if none.
    const TalentInfo* talentOfSpell(u32 spellId, u32& rank) const;

    // --- Factions ---
    const FactionInfo* factionByList(u32 listId) const;
    const FactionInfo* faction(u32 factionId) const;

    // --- Shapeshift forms (SpellShapeshiftForm.dbc) ---
    // The action bar a form swaps in (m_bonusActionBar: 1 cat / battle stance / stealth, 2 defensive stance,
    // 3 bear / berserker stance), 0 none.
    u32 formBonusBar(u32 form) const;
    // A stance (SHAPESHIFT_FLAG_STANCE: warrior stances, stealth, Shadowform, Moonkin) rather than a body
    // change (cat, bear, travel): spells cast in it as without a form.
    bool formIsStance(u32 form) const;
    // Whether a spell can be cast in `form` (0: none), as SpellEntry::GetErrorAtShapeshiftedCast decides.
    bool castableInForm(const SpellDetails& spell, u32 form) const;

private:
    SpellText m_spellText;
    DbcTable m_skillLines;
    DbcTable m_baseSkillLines; // phase 21: under a language overlay, the base client's (names in enUS)
    DbcTable m_skillCategories;
    DbcTable m_abilities;
    // SkillLineAbility sorted by spell: (spell, record).
    std::vector<std::pair<u32, u32>> m_abilityBySpell;
    std::vector<TalentTabInfo> m_tabs;                // sorted by class mask, then order
    std::vector<TalentInfo> m_talents;                // sorted by tab, row, column
    std::vector<std::pair<u32, u32>> m_talentBySpell; // (rank spell, index into m_talents)
    std::vector<FactionInfo> m_factions;              // sorted by id
    DbcTable m_forms;
    std::array<i32, 64> m_factionByList{}; // list id -> index into m_factions (-1 none)
};

} // namespace vd::game
