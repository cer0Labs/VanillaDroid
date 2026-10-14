#pragma once

#include "core/Types.h"
#include "game/DbcIndex.h"

#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace vd::game {

// Phase 13 (professions): the client tables of the TradeSkillFrame and the CraftFrame, as WoW.exe 1.12.1
// reads them (CGTradeSkillInfo, CGCraftInfo): the recipes' spells (Spell.dbc: reagents, tools, spell focus,
// created item, craft list), their skill lines and difficulty ranks (SkillLineAbility.dbc), the created
// items' subclass names for the headers (ItemSubClass.dbc), the spell foci (SpellFocusObject.dbc), the skill
// line names (SkillLine.dbc) and the pets' families (CreatureFamily.dbc) for Beast Training's costs. Indexes
// over the mapped db pack, built once.

// SPELL_EFFECT_TRADE_SKILL: a profession's own spell ("Blacksmithing", "First Aid", "Smelting", "Enchanting",
// "Beast Training"): casting it opens its window. EffectMiscValue[0] 0 opens the TradeSkillFrame of the
// spell's skill line, otherwise the CraftFrame of that craft list (Spell_C_CastSpell; nothing is sent to the
// server).
constexpr u32 kEffectTradeSkill = 47;
constexpr u32 kSpellAttrTradeSpell = 0x20; // SPELL_ATTR_IS_TRADESKILL: listed by its skill line's window
constexpr u32 kTargetFlagItem = 0x10;      // TARGET_FLAG_ITEM: an enchantment needs an item to go on

// The craft lists (Spell.dbc castUI = EffectMiscValue[0] of the spell that opens them).
constexpr u32 kCraftBeastTraining = 1;
constexpr u32 kCraftDisguise = 2;
constexpr u32 kCraftEnchanting = 3;
constexpr u32 kCraftLists = 4;

// What a spell is for the profession windows (Spell.dbc, 1.12.1 layout).
struct TradeSpellInfo {
    u32 id = 0;
    std::string_view name;
    std::string_view rank;
    u32 castUi = 0; // the craft list it belongs to (kCraft*), 0 none
    u32 attributes = 0;
    u32 targets = 0;    // TARGET_FLAG_* it asks for (kTargetFlagItem: cast on an item)
    u32 spellFocus = 0; // SpellFocusObject.dbc: an anvil, a forge, a cooking fire... nearby
    std::array<u32, 2> tools{};
    std::array<u32, 8> reagents{};
    std::array<u32, 8> reagentCounts{};
    std::array<u32, 3> effects{};
    std::array<u32, 3> implicitTargetA{};
    std::array<u32, 3> implicitTargetB{};
    std::array<i32, 3> misc{};
    std::array<u32, 3> triggerSpells{};             // the spell a LEARN_SPELL effect teaches (Beast Training)
    u32 createdItem = 0;                            // EffectItemType[0]
    i32 basePoints = 0, dieSides = 0, baseDice = 0; // effect 0: how many items one cast makes
    u32 spellLevel = 0;

    bool tradeSpell() const { return (attributes & kSpellAttrTradeSpell) != 0; }
    bool opensWindow() const { return effects[0] == kEffectTradeSkill; }
    bool needsItemTarget() const { return (targets & kTargetFlagItem) != 0; }
    // How many items a cast makes (GetTradeSkillNumMade: effect 0's value range, at least 1).
    u32 madeMin() const;
    u32 madeMax() const;
};

// A row of SkillLineAbility.dbc: a spell in a skill line, with its difficulty ranks.
struct SkillAbility {
    u32 skillLine = 0;
    u32 spell = 0;
    u32 raceMask = 0;
    u32 classMask = 0;
    u32 minRank = 0;        // the skill rank needed to learn it
    u32 supercededBy = 0;   // its next rank
    u32 trivialHigh = 0;    // grey from this rank
    u32 trivialLow = 0;     // yellow from this rank (0: trivialHigh - 25)
    u32 trainingPoints = 0; // Beast Training: what teaching it to a pet costs
};

class TradeSkillData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_spells.empty(); }

    // Any spell of Spell.dbc; false when the client does not have it.
    bool spell(u32 spellId, TradeSpellInfo& out) const;
    // The skill line row of `spellId` for this race and class (the client's lookup): the first row whose
    // masks take them, else the first row of the spell (a GM's .learn across classes). False when the spell
    // is in no line.
    bool ability(u32 spellId, u32 race, u32 cls, SkillAbility& out) const;
    // The row of `spellId` in `skillLine` (a pet family's line).
    bool abilityInLine(u32 skillLine, u32 spellId, SkillAbility& out) const;

    std::string_view skillLineName(u32 skillLine) const;
    // A header of the TradeSkillFrame: the subclass's long name ("One-Handed Swords"), or its short one
    // ("Trade Goods") when it has none. Empty when the table has no such subclass.
    std::string_view subclassName(u32 itemClass, u32 subclass) const;
    std::string_view focusName(u32 focusId) const; // "Anvil", "Forge", "Cooking Fire"...
    // The two skill lines of a creature family ("Pet - Wolf" and the hunters' pets' general one), 0 when
    // none.
    std::array<u32, 2> familySkillLines(u32 family) const;

private:
    SkillAbility row(u32 record) const;

    DbcTable m_spells;
    DbcTable m_skillLines;
    DbcTable m_abilities;
    DbcTable m_foci;
    DbcTable m_families;
    db::DbcView m_subclasses;
    std::vector<std::pair<u32, u32>> m_abilityBySpell; // (spell, SkillLineAbility record), sorted
};

} // namespace vd::game
