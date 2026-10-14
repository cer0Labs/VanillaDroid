#pragma once

#include "core/Types.h"
#include "game/Entities.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class AbilityData;
class CharacterData;

// The skill window's list as WoW.exe builds it for SkillFrame.lua (phase 15): GetNumSkillLines,
// GetSkillLineInfo, ExpandSkillHeader / CollapseSkillHeader and Get/SetSelectedSkill. The builder (0x4D2CB0)
// takes the player's skill fields in slot order, keeps the lines SkillRaceClassInfo lists for its race and
// class (not hidden; not yet learned only when that row lets it show at the player's level), adds a header
// for each SkillLineCategory, sorts the headers by the category's sort index and the lines (0x4D3070) by
// header, header first, learned before not learned, then by name; lines under a collapsed header go to the
// end and are not counted. Every change of the skill fields builds the list again with every header expanded
// (the rebuild resets the collapse mask), as SKILL_LINES_CHANGED does in the client. GetSkillLineInfo
// (0x4D3610) reads each line's numbers from the fields. Main thread; fixed storage.

// One skill line of the player with what the tables say of it (readSkillSources fills them; tests write
// them).
struct SkillLineSource {
    u32 skill = 0; // SkillLine.dbc
    u32 step = 0;  // the high half of the skill's first field: its rank of profession (Apprentice 1...)
    u32 value = 0; // the second field: value, then maximum
    u32 max = 0;
    i32 tempBonus = 0; // the third field: temporary bonus (signed), then permanent bonus
    u32 permBonus = 0;
    u32 category = 0;      // SkillLine.dbc's SkillLineCategory
    u32 categoryOrder = 0; // its sort index
    std::string_view name;
    std::string_view description;
    std::string_view categoryName;
    u32 flags = 0; // SkillRaceClassInfo.dbc of the race and class
    u32 minLevel = 0;
    u32 costIndex = 0;
};

constexpr u32 kSkillSlots = 128;

// The player's skill lines in slot order, with the lines of SkillLine.dbc, SkillLineCategory.dbc and
// SkillRaceClassInfo.dbc; a line without its rows in those tables is left out (as WoW.exe does). Returns how
// many.
u32 readSkillSources(const Entity& player, const AbilityData& abilities, const CharacterData& data,
                     std::span<SkillLineSource> out);

// A row of the window (GetSkillLineInfo's thirteen values, minus the costs no 1.12 skill has).
struct SkillLineRow {
    bool header = false;
    bool expanded = true; // a header's: its lines show
    u32 skill = 0;        // 0 for a header
    u32 category = 0;
    std::string_view name; // a header: its category's name
    std::string_view description;
    u32 rank = 0;     // the value plus the permanent bonus (when it has a value)
    u32 maxRank = 0;  // the maximum plus the permanent bonus; 1 for proficiencies (kSkillFlagMaxRankOne)
    i32 modifier = 0; // the temporary bonus (green when positive, red when negative)
    bool abandonable = false; // a step of profession and SKILL_FLAG_UNLEARNABLE: the Unlearn button
    bool learned = true;      // false: listed before it is learned (maximum 0)
    u32 minLevel = 0;
    u32 costType = 0; // the cost index + 1 (the bar's colour of skills that cost points: none in 1.12)
};

class SkillList {
public:
    SkillList(); // its storage, once (kept on the heap: the screens holding it live on the stack)
    // Builds the list from the player's lines; a change of them since the last call expands every header (the
    // selection stays an index, as the client's: it may land on another line).
    void build(std::span<const SkillLineSource> lines, u32 level);
    // Whether the last build() saw other skill fields than the one before (SKILL_LINES_CHANGED).
    bool changed() const { return m_changed; }

    // The rows shown (GetNumSkillLines / GetSkillLineInfo, 0-based).
    std::span<const SkillLineRow> rows() const { return {m_rows.data(), m_shown}; }
    // CollapseSkillHeader / ExpandSkillHeader of a header row; -1: every header (the "All" button). Applies
    // at the next build().
    void collapse(i32 row);
    void expand(i32 row);
    bool allExpanded() const; // SkillFrameCollapseAllButton's state: no header collapsed
    // Get/SetSelectedSkill: a row index (-1 none), kept as it is when the list changes.
    i32 selected() const { return m_selected; }
    void select(i32 row) { m_selected = row; }
    void reset(); // another character: nothing selected, every header expanded

private:
    struct Entry {
        u32 source = 0;   // index into the lines given (headers: their category's first line)
        u32 category = 0; // index into m_categories
        bool header = false;
        bool learned = true;
    };
    struct Category {
        u32 id = 0;
        u32 order = 0;
        u32 firstLine = 0;
    };

    std::vector<SkillLineSource> m_lines; // kSkillSlots
    u32 m_lineCount = 0;
    std::vector<Entry> m_entries; // kSkillSlots * 2
    u32 m_entryCount = 0;
    std::array<Category, 32> m_categories{};
    u32 m_categoryCount = 0;
    std::vector<SkillLineRow> m_rows; // kSkillSlots * 2
    u32 m_shown = 0;
    u32 m_expandedMask = 0xFFFFFFFFu; // bit per header, in the headers' sorted order
    u64 m_signature = 0;
    bool m_changed = false;
    i32 m_selected = -1;
};

} // namespace vd::game
