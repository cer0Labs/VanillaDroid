#include "game/AbilityData.h"

#include <algorithm>

namespace vd::game {
namespace {

// Field numbers of the 1.12.1 client tables (vMaNGOS DBCStructure.h / DBCfmt.h, checked against db.vdpk).
namespace skill { // SkillLine.dbc (22 fields)
constexpr u32 kCategory = 1, kName = 3, kDescription = 12, kIcon = 21, kFields = 22;
}
namespace category { // SkillLineCategory.dbc (11 fields: id, name[8] + flags, sort index)
constexpr u32 kName = 1, kFields = 11;
} // namespace category
namespace ability { // SkillLineAbility.dbc (15 fields)
constexpr u32 kSkill = 1, kSpell = 2, kRaceMask = 3, kClassMask = 4, kFields = 15;
}
namespace tab { // TalentTab.dbc (15 fields)
constexpr u32 kName = 1, kIcon = 10, kClassMask = 12, kOrder = 13, kBackground = 14, kFields = 15;
}
namespace talent { // Talent.dbc (21 fields)
constexpr u32 kTab = 1, kRow = 2, kColumn = 3, kRanks = 4, kDependsOn = 13, kDependsOnRank = 16,
              kDependsOnSpell = 20, kFields = 21;
}
namespace form { // SpellShapeshiftForm.dbc (14 fields)
constexpr u32 kBonusBar = 1, kFlags = 11, kFields = 14;
constexpr u32 kFlagStance = 0x1; // SHAPESHIFT_FLAG_STANCE
} // namespace form
namespace faction { // Faction.dbc (37 fields)
constexpr u32 kListId = 1, kRaceMask = 2, kClassMask = 6, kBase = 10, kFlags = 14, kParent = 18, kName = 19,
              kDescription = 28, kFields = 37;
}

// ReputationMgr::PointsInRank, Hated first.
constexpr i32 kPointsInRank[kReputationRanks] = {36000, 3000, 3000, 3000, 6000, 12000, 21000, 1000};
constexpr i32 kReputationBottom = -42000;

u32 bit(u32 value) {
    return value >= 1 && value <= 32 ? 1u << (value - 1) : 0u;
}

} // namespace

i32 FactionInfo::entryFor(u32 race, u32 cls) const {
    for (u32 i = 0; i < 4; ++i) {
        if ((raceMask[i] == 0 || (raceMask[i] & bit(race))) &&
            (classMask[i] == 0 || (classMask[i] & bit(cls)))) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 FactionInfo::baseReputation(u32 race, u32 cls) const {
    const i32 i = entryFor(race, cls);
    return i >= 0 ? baseValue[static_cast<u32>(i)] : 0;
}

u32 reputationRank(i32 reputation) {
    i32 start = kReputationBottom;
    for (u32 rank = 0; rank < kReputationRanks; ++rank) {
        start += kPointsInRank[rank];
        if (reputation < start) {
            return rank;
        }
    }
    return kReputationRanks - 1;
}

i32 reputationRankStart(u32 rank) {
    i32 start = kReputationBottom;
    for (u32 i = 0; i < rank && i < kReputationRanks; ++i) {
        start += kPointsInRank[i];
    }
    return start;
}

i32 reputationRankSize(u32 rank) {
    return kPointsInRank[std::min(rank, kReputationRanks - 1)];
}

bool AbilityData::init(const pack::PackReader& db) {
    bool ok = m_spellText.init(db);

    // Skills and the spell -> skill line map.
    ok &= m_skillLines.load(db, "SkillLine", skill::kFields);
    // Phase 21: the base client's names too, which the spell book's tabs match (SpellBookTabs).
    if (!db.hasOverlay() || !m_baseSkillLines.load(db, "SkillLine", skill::kFields, true)) {
        m_baseSkillLines = m_skillLines;
    }
    ok &= m_skillCategories.load(db, "SkillLineCategory", category::kFields);
    m_abilityBySpell.clear();
    if (m_abilities.load(db, "SkillLineAbility", ability::kFields)) {
        const db::DbcView& v = m_abilities.view;
        m_abilityBySpell.reserve(v.recordCount());
        for (u32 r = 0; r < v.recordCount(); ++r) {
            m_abilityBySpell.emplace_back(v.getU32(r, ability::kSpell), r);
        }
        std::sort(m_abilityBySpell.begin(), m_abilityBySpell.end());
    } else {
        ok = false;
    }

    // Talent trees and talents (a few hundred: copied out of the tables).
    m_tabs.clear();
    m_talents.clear();
    m_talentBySpell.clear();
    DbcTable tabs, talents, baseTabs;
    if (tabs.load(db, "TalentTab", tab::kFields) && talents.load(db, "Talent", talent::kFields)) {
        if (!db.hasOverlay() || !baseTabs.load(db, "TalentTab", tab::kFields, true) ||
            baseTabs.view.recordCount() != tabs.view.recordCount()) {
            baseTabs = tabs;
        }
        for (u32 r = 0; r < tabs.view.recordCount(); ++r) {
            TalentTabInfo t;
            t.id = tabs.view.getU32(r, 0);
            t.name = tabs.view.getString(r, tab::kName);
            t.baseName = baseTabs.view.getString(r, tab::kName);
            t.spellIcon = tabs.view.getU32(r, tab::kIcon);
            t.classMask = tabs.view.getU32(r, tab::kClassMask);
            t.order = tabs.view.getU32(r, tab::kOrder);
            t.background = tabs.view.getString(r, tab::kBackground);
            m_tabs.push_back(t);
        }
        std::sort(m_tabs.begin(), m_tabs.end(), [](const TalentTabInfo& a, const TalentTabInfo& b) {
            return a.classMask != b.classMask ? a.classMask < b.classMask : a.order < b.order;
        });
        m_talents.reserve(talents.view.recordCount());
        for (u32 r = 0; r < talents.view.recordCount(); ++r) {
            TalentInfo t;
            t.id = talents.view.getU32(r, 0);
            t.tab = talents.view.getU32(r, talent::kTab);
            t.row = talents.view.getU32(r, talent::kRow);
            t.column = talents.view.getU32(r, talent::kColumn);
            for (u32 i = 0; i < kMaxTalentRanks; ++i) {
                t.ranks[i] = talents.view.getU32(r, talent::kRanks + i);
            }
            t.dependsOn = talents.view.getU32(r, talent::kDependsOn);
            t.dependsOnRank = talents.view.getU32(r, talent::kDependsOnRank);
            t.dependsOnSpell = talents.view.getU32(r, talent::kDependsOnSpell);
            m_talents.push_back(t);
        }
        std::sort(m_talents.begin(), m_talents.end(), [](const TalentInfo& a, const TalentInfo& b) {
            if (a.tab != b.tab) {
                return a.tab < b.tab;
            }
            return a.row != b.row ? a.row < b.row : a.column < b.column;
        });
        for (u32 i = 0; i < m_talents.size(); ++i) {
            for (const u32 spell : m_talents[i].ranks) {
                if (spell != 0) {
                    m_talentBySpell.emplace_back(spell, i);
                }
            }
        }
        std::sort(m_talentBySpell.begin(), m_talentBySpell.end());
    } else {
        ok = false;
    }

    // Shapeshift forms (the bonus action bars and which forms act as stances).
    ok &= m_forms.load(db, "SpellShapeshiftForm", form::kFields);

    // Factions with a reputation list entry.
    m_factions.clear();
    m_factionByList.fill(-1);
    DbcTable factions;
    if (factions.load(db, "Faction", faction::kFields)) {
        const db::DbcView& v = factions.view;
        for (u32 r = 0; r < v.recordCount(); ++r) {
            FactionInfo f;
            f.id = v.getU32(r, 0);
            f.listId = v.getI32(r, faction::kListId);
            for (u32 i = 0; i < 4; ++i) {
                f.raceMask[i] = v.getU32(r, faction::kRaceMask + i);
                f.classMask[i] = v.getU32(r, faction::kClassMask + i);
                f.baseValue[i] = v.getI32(r, faction::kBase + i);
                f.flags[i] = v.getU32(r, faction::kFlags + i);
            }
            f.parent = v.getU32(r, faction::kParent);
            f.name = v.getString(r, faction::kName);
            f.description = v.getString(r, faction::kDescription);
            m_factions.push_back(f);
        }
        std::sort(m_factions.begin(), m_factions.end(),
                  [](const FactionInfo& a, const FactionInfo& b) { return a.id < b.id; });
        for (u32 i = 0; i < m_factions.size(); ++i) {
            const i32 list = m_factions[i].listId;
            if (list >= 0 && static_cast<u32>(list) < m_factionByList.size()) {
                m_factionByList[static_cast<u32>(list)] = static_cast<i32>(i);
            }
        }
    } else {
        ok = false;
    }
    return ok;
}

bool AbilityData::skillLine(u32 id, SkillLineInfo& out) const {
    const u32 r = m_skillLines.find(id);
    if (r == DbcTable::kMissing) {
        return false;
    }
    const db::DbcView& v = m_skillLines.view;
    out.id = id;
    out.category = static_cast<SkillCategory>(v.getI32(r, skill::kCategory));
    out.name = v.getString(r, skill::kName);
    out.description = v.getString(r, skill::kDescription);
    out.spellIcon = v.getU32(r, skill::kIcon);
    const u32 b = m_baseSkillLines.find(id);
    out.baseName = b != DbcTable::kMissing ? m_baseSkillLines.view.getString(b, skill::kName) : out.name;
    return true;
}

std::string_view AbilityData::skillCategoryName(SkillCategory c) const {
    const u32 r = m_skillCategories.find(static_cast<u32>(c));
    return r == DbcTable::kMissing ? std::string_view{}
                                   : m_skillCategories.view.getString(r, category::kName);
}

u32 AbilityData::skillOfSpell(u32 spellId, u32 race, u32 cls) const {
    auto it = std::lower_bound(m_abilityBySpell.begin(), m_abilityBySpell.end(), std::make_pair(spellId, 0u));
    u32 any = 0;
    const db::DbcView& v = m_abilities.view;
    for (; it != m_abilityBySpell.end() && it->first == spellId; ++it) {
        const u32 raceMask = v.getU32(it->second, ability::kRaceMask);
        const u32 classMask = v.getU32(it->second, ability::kClassMask);
        const u32 skillId = v.getU32(it->second, ability::kSkill);
        if ((raceMask == 0 || (raceMask & bit(race))) && (classMask == 0 || (classMask & bit(cls)))) {
            return skillId;
        }
        any = any ? any : skillId;
    }
    return any; // a spell taught across classes (a GM's .learn): its first line
}

u32 AbilityData::talentTabs(u32 cls, std::span<TalentTabInfo> out) const {
    u32 count = 0;
    for (const TalentTabInfo& t : m_tabs) {
        if ((t.classMask & bit(cls)) != 0 && count < out.size()) {
            out[count++] = t;
        }
    }
    return count;
}

std::span<const TalentInfo> AbilityData::talents(u32 tabId) const {
    const auto first = std::lower_bound(m_talents.begin(), m_talents.end(), tabId,
                                        [](const TalentInfo& t, u32 id) { return t.tab < id; });
    auto last = first;
    while (last != m_talents.end() && last->tab == tabId) {
        ++last;
    }
    return {m_talents.data() + (first - m_talents.begin()), static_cast<usize>(last - first)};
}

const TalentInfo* AbilityData::talent(u32 talentId) const {
    for (const TalentInfo& t : m_talents) {
        if (t.id == talentId) {
            return &t;
        }
    }
    return nullptr;
}

const TalentInfo* AbilityData::talentOfSpell(u32 spellId, u32& rank) const {
    const auto it =
        std::lower_bound(m_talentBySpell.begin(), m_talentBySpell.end(), std::make_pair(spellId, 0u));
    if (it == m_talentBySpell.end() || it->first != spellId) {
        return nullptr;
    }
    const TalentInfo& t = m_talents[it->second];
    for (u32 i = 0; i < kMaxTalentRanks; ++i) {
        if (t.ranks[i] == spellId) {
            rank = i;
        }
    }
    return &t;
}

const FactionInfo* AbilityData::factionByList(u32 listId) const {
    if (listId >= m_factionByList.size() || m_factionByList[listId] < 0) {
        return nullptr;
    }
    return &m_factions[static_cast<u32>(m_factionByList[listId])];
}

u32 AbilityData::formBonusBar(u32 shapeshift) const {
    const u32 r = shapeshift ? m_forms.find(shapeshift) : DbcTable::kMissing;
    return r == DbcTable::kMissing ? 0 : m_forms.view.getU32(r, form::kBonusBar);
}

bool AbilityData::formIsStance(u32 shapeshift) const {
    const u32 r = shapeshift ? m_forms.find(shapeshift) : DbcTable::kMissing;
    return r == DbcTable::kMissing || (m_forms.view.getU32(r, form::kFlags) & form::kFlagStance) != 0;
}

bool AbilityData::castableInForm(const SpellDetails& spell, u32 shapeshift) const {
    constexpr u32 kAttrNotShapeshift = 0x00010000;           // SPELL_ATTR_NOT_SHAPESHIFT
    constexpr u32 kAttrEx2AllowNotShapeshifted = 0x00080000; // SPELL_ATTR_EX2_ALLOW_WHILE_NOT_SHAPESHIFTED
    const u32 mask = bit(shapeshift);
    if (mask & spell.stancesNot) {
        return false;
    }
    if (mask & spell.stances) {
        return true;
    }
    if (shapeshift != 0 && !formIsStance(shapeshift)) { // a body change: only spells of no form
        return (spell.attributes & kAttrNotShapeshift) == 0 && spell.stances == 0;
    }
    return spell.stances == 0 || (spell.attributesEx2 & kAttrEx2AllowNotShapeshifted) != 0;
}

const FactionInfo* AbilityData::faction(u32 factionId) const {
    const auto it = std::lower_bound(m_factions.begin(), m_factions.end(), factionId,
                                     [](const FactionInfo& f, u32 id) { return f.id < id; });
    return it != m_factions.end() && it->id == factionId ? &*it : nullptr;
}

} // namespace vd::game
