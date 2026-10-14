#pragma once

#include "core/Types.h"
#include "pack/PackReader.h"

#include <array>
#include <string_view>
#include <vector>

namespace vd::game {

// The client tables the character window of phase 15 reads that no other area indexes (db.vdpk, built once at
// start-up): SkillRaceClassInfo.dbc (which skills a race and class have, and with which flags:
// GetSkillLineInfo's abandon button, its maximum rank, the skill window's rows), SkillLineCategory.dbc's sort
// order (the skill window's headers) and two columns of ChrClasses.dbc (the class's file name, which picks
// PaperDollStatTooltip's class-specific texts, and UnitHasRelicSlot). Main thread; nothing allocates once
// built.

// SkillRaceClassInfo.dbc flags (vMaNGOS DBCEnums.h SkillRaceClassInfoFlags), as WoW.exe reads them.
constexpr u32 kSkillFlagHidden = 0x002;      // never listed (the skill window's builder 0x4D2CB0 skips it)
constexpr u32 kSkillFlagLearnable = 0x004;   // listed before it is learned once the level allows it
constexpr u32 kSkillFlagUnlearnable = 0x020; // SKILL_FLAG_UNLEARNABLE: the professions' abandon button
constexpr u32 kSkillFlagMaxRankOne = 0x400;  // GetSkillLineInfo shows it as a proficiency (rank 1 of 1)

struct SkillRaceClassInfo {
    u32 skill = 0;
    u32 raceMask = 0;  // 0: every race
    u32 classMask = 0; // 0: every class
    u32 flags = 0;
    u32 minLevel = 0;
    u32 costIndex = 0;
};

class CharacterData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_skillInfo.empty(); }

    // The row of `skill` that fits the race and class (WoW.exe 0x6DDF90: the first of the skill's rows whose
    // race and class masks take them), nullptr if none.
    const SkillRaceClassInfo* skillInfo(u32 skill, u32 race, u32 cls) const;
    // SkillLineCategory.dbc's sort index (the order of the skill window's headers); large when unknown.
    u32 categoryOrder(u32 category) const;
    // ChrClasses.dbc: the class's file name ("WARRIOR"), empty if unknown, and whether its ranged slot takes
    // a relic (UnitHasRelicSlot, WoW.exe 0x519E50: paladins, shamans and druids in 1.12).
    std::string_view classFile(u32 cls) const;
    bool relicSlot(u32 cls) const;

    // Tests: rows of a table without the db pack.
    void addSkillInfo(const SkillRaceClassInfo& row);
    void setCategoryOrder(u32 category, u32 order);
    void setClass(u32 cls, std::string_view file, bool relicSlot);

private:
    struct ClassInfo {
        std::string_view file;
        bool relic = false;
    };
    std::vector<SkillRaceClassInfo> m_skillInfo;      // sorted by skill, rows of a skill in table order
    std::vector<std::pair<u32, u32>> m_categoryOrder; // (category, sort index), sorted
    std::array<ClassInfo, 16> m_classes{};
};

// The weapon skill of an item (ItemPrototype::GetSkill for ITEM_CLASS_WEAPON; WoW.exe's table c4d738 by
// subclass), 0 when it has none. A missing main hand is fought with as a fist weapon: Unarmed.
constexpr u32 kSkillUnarmed = 162;
u32 weaponSkill(u32 itemClass, u32 subclass);

} // namespace vd::game
