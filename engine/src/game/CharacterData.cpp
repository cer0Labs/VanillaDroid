#include "game/CharacterData.h"

#include "game/DbcIndex.h"

#include <algorithm>

namespace vd::game {
namespace {

// SkillRaceClassInfo.dbc (1.12): id, skill, race mask, class mask, flags, minimum level, tier, cost index.
namespace rci {
constexpr u32 kSkill = 1;
constexpr u32 kRaceMask = 2;
constexpr u32 kClassMask = 3;
constexpr u32 kFlags = 4;
constexpr u32 kMinLevel = 5;
constexpr u32 kCostIndex = 7;
constexpr u32 kFields = 8;
} // namespace rci
// SkillLineCategory.dbc: id, name (8 locales + flags), sort index.
constexpr u32 kCategorySortIndex = 10;
// ChrClasses.dbc: ..., file name, spell class set, relic flag (WoW.exe reads +0x40).
constexpr u32 kClassFile = 14;
constexpr u32 kClassRelic = 16;

constexpr u32 kMissingOrder = 0xFFFFu;

bool fits(const SkillRaceClassInfo& row, u32 race, u32 cls) {
    const bool raceFits =
        row.raceMask == 0 || (race > 0 && race <= 32 && (row.raceMask & (1u << (race - 1))) != 0);
    const bool classFits =
        row.classMask == 0 || (cls > 0 && cls <= 32 && (row.classMask & (1u << (cls - 1))) != 0);
    return raceFits && classFits;
}

} // namespace

bool CharacterData::init(const pack::PackReader& db) {
    m_skillInfo.clear();
    m_categoryOrder.clear();
    m_classes = {};
    bool ok = true;
    DbcTable table;
    if (table.load(db, "SkillRaceClassInfo", rci::kFields)) {
        const db::DbcView& v = table.view;
        m_skillInfo.reserve(v.recordCount());
        for (u32 r = 0; r < v.recordCount(); ++r) {
            SkillRaceClassInfo row;
            row.skill = v.getU32(r, rci::kSkill);
            row.raceMask = v.getU32(r, rci::kRaceMask);
            row.classMask = v.getU32(r, rci::kClassMask);
            row.flags = v.getU32(r, rci::kFlags);
            row.minLevel = v.getU32(r, rci::kMinLevel);
            row.costIndex = v.getU32(r, rci::kCostIndex);
            m_skillInfo.push_back(row);
        }
        std::stable_sort(
            m_skillInfo.begin(), m_skillInfo.end(),
            [](const SkillRaceClassInfo& a, const SkillRaceClassInfo& b) { return a.skill < b.skill; });
    } else {
        ok = false;
    }
    DbcTable categories;
    if (categories.load(db, "SkillLineCategory", kCategorySortIndex + 1)) {
        for (u32 r = 0; r < categories.view.recordCount(); ++r) {
            m_categoryOrder.emplace_back(categories.view.getU32(r, 0),
                                         categories.view.getU32(r, kCategorySortIndex));
        }
        std::sort(m_categoryOrder.begin(), m_categoryOrder.end());
    } else {
        ok = false;
    }
    DbcTable classes;
    if (classes.load(db, "ChrClasses", kClassRelic + 1)) {
        for (u32 r = 0; r < classes.view.recordCount(); ++r) {
            const u32 id = classes.view.getU32(r, 0);
            if (id < m_classes.size()) {
                m_classes[id].file = classes.view.getString(r, kClassFile);
                m_classes[id].relic = classes.view.getU32(r, kClassRelic) != 0;
            }
        }
    } else {
        ok = false;
    }
    return ok;
}

const SkillRaceClassInfo* CharacterData::skillInfo(u32 skill, u32 race, u32 cls) const {
    auto it = std::lower_bound(m_skillInfo.begin(), m_skillInfo.end(), skill,
                               [](const SkillRaceClassInfo& row, u32 id) { return row.skill < id; });
    for (; it != m_skillInfo.end() && it->skill == skill; ++it) {
        if (fits(*it, race, cls)) {
            return &*it;
        }
    }
    return nullptr;
}

u32 CharacterData::categoryOrder(u32 category) const {
    const auto it =
        std::lower_bound(m_categoryOrder.begin(), m_categoryOrder.end(), std::make_pair(category, 0u));
    return it != m_categoryOrder.end() && it->first == category ? it->second : kMissingOrder;
}

std::string_view CharacterData::classFile(u32 cls) const {
    return cls < m_classes.size() ? m_classes[cls].file : std::string_view{};
}

bool CharacterData::relicSlot(u32 cls) const {
    return cls < m_classes.size() && m_classes[cls].relic;
}

void CharacterData::addSkillInfo(const SkillRaceClassInfo& row) {
    const auto at = std::upper_bound(m_skillInfo.begin(), m_skillInfo.end(), row.skill,
                                     [](u32 id, const SkillRaceClassInfo& r) { return id < r.skill; });
    m_skillInfo.insert(at, row);
}

void CharacterData::setCategoryOrder(u32 category, u32 order) {
    const auto it =
        std::lower_bound(m_categoryOrder.begin(), m_categoryOrder.end(), std::make_pair(category, 0u));
    if (it != m_categoryOrder.end() && it->first == category) {
        it->second = order;
    } else {
        m_categoryOrder.insert(it, {category, order});
    }
}

void CharacterData::setClass(u32 cls, std::string_view file, bool relic) {
    if (cls < m_classes.size()) {
        m_classes[cls] = {file, relic};
    }
}

u32 weaponSkill(u32 itemClass, u32 subclass) {
    // vMaNGOS Item.cpp item_weapon_skills: axes, two-handed axes, bows, guns, maces, two-handed maces,
    // polearms, swords, two-handed swords, -, staves, -, -, fist weapons (Unarmed), -, daggers, thrown,
    // spears (vMaNGOS writes SKILL_ASSASSINATION there), crossbows, wands, fishing poles.
    static constexpr u32 kSkills[] = {44, 172, 45,  46, 54,  160, 229, 43,  55,  0,  136,
                                      0,  0,   162, 0,  173, 176, 253, 226, 228, 356};
    constexpr u32 kItemClassWeapon = 2;
    return itemClass == kItemClassWeapon && subclass < std::size(kSkills) ? kSkills[subclass] : 0;
}

} // namespace vd::game
