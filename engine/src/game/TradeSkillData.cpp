#include "game/TradeSkillData.h"

#include <algorithm>

namespace vd::game {
namespace {

// Field numbers of the 1.12.1 client tables (vMaNGOS DBCStructure.h / DBCfmt.h; Spell.dbc as game/GameData
// and game/SpellText number it: the client's layout, 173 fields; checked against db.vdpk and WoW.exe's
// reads).
namespace spell {
constexpr u32 kCastUi = 3, kAttributes = 6, kTargets = 13, kSpellFocus = 15, kSpellLevel = 29, kTotem = 40,
              kReagent = 42, kReagentCount = 50, kEffect = 61, kDieSides = 64, kBaseDice = 67,
              kBasePoints = 76, kImplicitTargetA = 82, kImplicitTargetB = 85, kItemType = 103, kMisc = 106,
              kTrigger = 109, kName = 120, kRank = 129, kFields = 173;
}
namespace ability { // SkillLineAbility.dbc (15 fields)
constexpr u32 kSkill = 1, kSpell = 2, kRaceMask = 3, kClassMask = 4, kMinRank = 7, kSuperceded = 8,
              kTrivialHigh = 10, kTrivialLow = 11, kTrainingPoints = 14, kFields = 15;
}
namespace skill { // SkillLine.dbc (22 fields)
constexpr u32 kName = 3, kFields = 22;
}
namespace subclass { // ItemSubClass.dbc (28 fields): class, subclass, ..., display name 10, verbose name 19
constexpr u32 kClass = 0, kSubclass = 1, kDisplayName = 10, kVerboseName = 19, kFields = 28;
}
namespace focus { // SpellFocusObject.dbc (10 fields)
constexpr u32 kName = 1, kFields = 10;
}
namespace family { // CreatureFamily.dbc (18 fields): id, scales, the two skill lines at 5 and 6
constexpr u32 kSkillLine = 5, kFields = 18;
}

u32 bit(u32 value) {
    return value >= 1 && value <= 32 ? 1u << (value - 1) : 0u;
}

} // namespace

u32 TradeSpellInfo::madeMin() const {
    // SpellCaster::CalculateSpellEffectValue: base + baseDice for 0 or 1 die sides, else base + the lower
    // end.
    const i32 low = dieSides <= 1 ? baseDice : std::min(baseDice, dieSides);
    return static_cast<u32>(std::max(1, basePoints + low));
}

u32 TradeSpellInfo::madeMax() const {
    const i32 high = dieSides <= 1 ? baseDice : std::max(baseDice, dieSides);
    return static_cast<u32>(std::max(1, basePoints + high));
}

bool TradeSkillData::init(const pack::PackReader& db) {
    bool ok = m_spells.load(db, "Spell", spell::kFields);
    ok &= m_skillLines.load(db, "SkillLine", skill::kFields);
    ok &= m_foci.load(db, "SpellFocusObject", focus::kFields);
    ok &= m_families.load(db, "CreatureFamily", family::kFields);
    DbcTable subclasses;
    if (subclasses.load(db, "ItemSubClass", subclass::kFields)) {
        m_subclasses = subclasses.view; // looked up by (class, subclass): no id field
    } else {
        ok = false;
    }
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
    return ok;
}

bool TradeSkillData::spell(u32 spellId, TradeSpellInfo& out) const {
    const u32 r = m_spells.find(spellId);
    if (r == DbcTable::kMissing) {
        return false;
    }
    const db::DbcView& v = m_spells.view;
    out = TradeSpellInfo{};
    out.id = spellId;
    out.name = v.getString(r, spell::kName);
    out.rank = v.getString(r, spell::kRank);
    out.castUi = v.getU32(r, spell::kCastUi);
    out.attributes = v.getU32(r, spell::kAttributes);
    out.targets = v.getU32(r, spell::kTargets);
    out.spellFocus = v.getU32(r, spell::kSpellFocus);
    out.spellLevel = v.getU32(r, spell::kSpellLevel);
    for (u32 i = 0; i < 2; ++i) {
        out.tools[i] = v.getU32(r, spell::kTotem + i);
    }
    for (u32 i = 0; i < 8; ++i) {
        const i32 reagent = v.getI32(r, spell::kReagent + i);
        out.reagents[i] = reagent > 0 ? static_cast<u32>(reagent) : 0;
        out.reagentCounts[i] = v.getU32(r, spell::kReagentCount + i);
    }
    for (u32 i = 0; i < 3; ++i) {
        out.effects[i] = v.getU32(r, spell::kEffect + i);
        out.implicitTargetA[i] = v.getU32(r, spell::kImplicitTargetA + i);
        out.implicitTargetB[i] = v.getU32(r, spell::kImplicitTargetB + i);
        out.misc[i] = v.getI32(r, spell::kMisc + i);
        out.triggerSpells[i] = v.getU32(r, spell::kTrigger + i);
    }
    out.createdItem = v.getU32(r, spell::kItemType);
    out.basePoints = v.getI32(r, spell::kBasePoints);
    out.dieSides = v.getI32(r, spell::kDieSides);
    out.baseDice = v.getI32(r, spell::kBaseDice);
    return true;
}

SkillAbility TradeSkillData::row(u32 record) const {
    const db::DbcView& v = m_abilities.view;
    SkillAbility a;
    a.skillLine = v.getU32(record, ability::kSkill);
    a.spell = v.getU32(record, ability::kSpell);
    a.raceMask = v.getU32(record, ability::kRaceMask);
    a.classMask = v.getU32(record, ability::kClassMask);
    a.minRank = v.getU32(record, ability::kMinRank);
    a.supercededBy = v.getU32(record, ability::kSuperceded);
    a.trivialHigh = v.getU32(record, ability::kTrivialHigh);
    a.trivialLow = v.getU32(record, ability::kTrivialLow);
    a.trainingPoints = v.getU32(record, ability::kTrainingPoints);
    return a;
}

bool TradeSkillData::ability(u32 spellId, u32 race, u32 cls, SkillAbility& out) const {
    auto it = std::lower_bound(m_abilityBySpell.begin(), m_abilityBySpell.end(), std::make_pair(spellId, 0u));
    bool any = false;
    for (; it != m_abilityBySpell.end() && it->first == spellId; ++it) {
        const SkillAbility a = row(it->second);
        if ((a.raceMask == 0 || (a.raceMask & bit(race))) && (a.classMask == 0 || (a.classMask & bit(cls)))) {
            out = a;
            return true;
        }
        if (!any) {
            out = a;
            any = true;
        }
    }
    return any;
}

bool TradeSkillData::abilityInLine(u32 skillLine, u32 spellId, SkillAbility& out) const {
    auto it = std::lower_bound(m_abilityBySpell.begin(), m_abilityBySpell.end(), std::make_pair(spellId, 0u));
    for (; it != m_abilityBySpell.end() && it->first == spellId; ++it) {
        if (m_abilities.view.getU32(it->second, ability::kSkill) == skillLine) {
            out = row(it->second);
            return true;
        }
    }
    return false;
}

std::string_view TradeSkillData::skillLineName(u32 skillLine) const {
    const u32 r = m_skillLines.find(skillLine);
    return r == DbcTable::kMissing ? std::string_view{} : m_skillLines.view.getString(r, skill::kName);
}

std::string_view TradeSkillData::subclassName(u32 itemClass, u32 sub) const {
    // A few dozen rows: a linear search (headers are named when the list is rebuilt, not every frame).
    const db::DbcView& v = m_subclasses;
    for (u32 r = 0; r < v.recordCount(); ++r) {
        if (v.getU32(r, subclass::kClass) == itemClass && v.getU32(r, subclass::kSubclass) == sub) {
            const std::string_view verbose = v.getString(r, subclass::kVerboseName);
            return verbose.empty() ? v.getString(r, subclass::kDisplayName) : verbose;
        }
    }
    return {};
}

std::string_view TradeSkillData::focusName(u32 focusId) const {
    const u32 r = m_foci.find(focusId);
    return r == DbcTable::kMissing ? std::string_view{} : m_foci.view.getString(r, focus::kName);
}

std::array<u32, 2> TradeSkillData::familySkillLines(u32 familyId) const {
    const u32 r = m_families.find(familyId);
    if (r == DbcTable::kMissing) {
        return {};
    }
    return {m_families.view.getU32(r, family::kSkillLine), m_families.view.getU32(r, family::kSkillLine + 1)};
}

} // namespace vd::game
