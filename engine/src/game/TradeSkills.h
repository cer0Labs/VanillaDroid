#pragma once

#include "core/Types.h"
#include "game/TradeSkillData.h"
#include "net/GameMessages.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Abilities;
class EntityStore;
class Inventory;
class QueryCache;
class SpellBook;
struct Entity;

// Phase 13 (professions): the TradeSkillFrame and the CraftFrame as WoW.exe 1.12.1 runs them on its own
// (CGTradeSkillInfo / CGCraftInfo): nothing about them comes from the server. Casting a profession's spell
// opens its window instead of being sent (Spell_C_CastSpell: SPELL_EFFECT_TRADE_SKILL, see openBySpell); the
// windows list the known spells of the skill line (SPELL_ATTR_IS_TRADESKILL) or of the craft list (Spell.dbc
// castUI) with the colour the player's rank gives them, grouped and sorted as the client does, with the
// counts the bags allow; "Create" casts the recipe (CMSG_CAST_SPELL) and "Create All" / a count re-casts it
// after each of the player's SMSG_SPELL_GO (CGTradeSkillInfo::DoRecast) until the count runs out, a cast
// fails, the player cancels or the window closes. Owned by game::Gameplay; main thread; the lists are rebuilt
// only when what they show changed (the known spells, the bags, the skill rank, the pet, the item templates
// that arrive), into reserved storage.

// A recipe's colour (GetTradeSkillInfo / GetCraftInfo's type): the chance of a skill-up. Crafts without ranks
// (Beast Training) are None, or Used once the pet knows the ability.
enum class RecipeColor : u8 { None, Optimal, Medium, Easy, Trivial, Used };
// CGTradeSkillInfo's rule: orange below trivialLow (0: trivialHigh - 25), yellow below halfway to
// trivialHigh, green below trivialHigh, grey from there.
RecipeColor recipeColor(u32 rank, u32 trivialLow, u32 trivialHigh);

// The equipment slots of the inventory slot filter (TradeSkillInvSlotDropDown): bit i of a recipe's slotMask,
// named by the client's key kTradeSlotKeys[i] (HEADSLOT ... BAGSLOT, NONEQUIPSLOT).
constexpr u32 kTradeSlots = 24;
extern const std::array<const char*, kTradeSlots> kTradeSlotKeys;
// The slots an item of this inventory type goes in (WoW.exe's table; one ring / trinket slot, the bag slot; 0
// for items that are not worn: NONEQUIPSLOT).
u32 tradeSlotMask(u32 inventoryType);

// A recipe of the open TradeSkillFrame.
struct TradeRecipe {
    u32 spellId = 0;
    std::string_view name;
    RecipeColor color = RecipeColor::Optimal;
    u32 header = 0;   // into headers()
    u32 slotMask = 0; // bits of kTradeSlotKeys
    u32 itemLevel = 0;
    u32 available = 0; // how many the bags' reagents allow (numAvailable)
};
// A header: the created items' class and subclass ("One-Handed Swords", "Trade Goods", "Consumable").
struct TradeHeader {
    u32 itemClass = 0;
    u32 subclass = 0;
    std::string_view name;
    bool collapsed = false;
    u32 count = 0; // recipes in it that pass the slot filter
};
// A line of the list as GetTradeSkillInfo gives it: a header or a recipe (after filters and collapsed
// headers).
struct TradeSkillRow {
    bool header = false;
    u32 index = 0; // into headers() or recipes()
};

// A craft of the open CraftFrame (Enchanting, Beast Training).
struct CraftEntry {
    u32 spellId = 0; // 0: a header (the skill line, when the crafts span several)
    u32 skillLine = 0;
    std::string_view name; // the spell's, or the skill line's for a header
    std::string_view rank; // "Rank 2"
    RecipeColor color = RecipeColor::None;
    u32 available = 0;
    u32 spellLevel = 0;
    bool collapsed = false; // headers
};

// The pet's training points (UNIT_TRAINING_POINTS: total in the high 16 bits, spent in the low ones).
struct PetTrainingPoints {
    u32 total = 0;
    u32 spent = 0;
    i32 available() const { return static_cast<i32>(total) - static_cast<i32>(spent); }
};

class TradeSkills {
public:
    // How a recipe is cast: Gameplay's own cast (a spell without a target).
    struct Caster {
        bool (*cast)(void* context, u32 spellId) = nullptr;
        void* context = nullptr;
    };

    TradeSkills(const EntityStore& entities, QueryCache& queries, const Inventory& inventory,
                const SpellBook& spells, const Abilities& abilities);

    void setData(const TradeSkillData* data) { m_data = data; }
    const TradeSkillData* data() const { return m_data; }
    void setCaster(Caster caster) { m_caster = caster; }
    void enterWorld(u64 player, bool sameCharacter);
    void leaveWorld();
    // Once per frame: rebuilds an open window's list when what it shows changed.
    void update(u32 nowMs);

    // Spell_C_CastSpell's first step: a profession's spell (SPELL_EFFECT_TRADE_SKILL) opens or closes its
    // window and is not cast. True when `spellId` is one (the caller sends nothing).
    bool openBySpell(u32 spellId);
    // The player's SMSG_SPELL_GO (DoRecast), a refused cast of the player's, the player's own cancel.
    void onSpellGo(const net::SpellGo& m, u64 player);
    void onCastFailed(u32 spellId);
    void onCastCancelled() { stopRecast(); }
    // Bumped whenever an open window's list, selection or repeat count changed: views rebuild then.
    u32 version() const { return m_version; }

    // --- TradeSkillFrame ---
    bool tradeSkillOpen() const { return m_skillLine != 0; }
    u32 skillLine() const { return m_skillLine; }
    std::string_view skillLineName() const;
    // GetTradeSkillLine's rank and maximum (the value and the maximum with the permanent bonus).
    u32 skillRank() const { return m_skillRank; }
    u32 skillMax() const { return m_skillMax; }
    // The skill line's known recipes, also those the filters hide (0: the window does not open).
    u32 recipeCount() const { return static_cast<u32>(m_known.size()); }
    // Templates of created items still asked for: the list waits for them (the client's 0xbde048).
    bool pending() const { return m_pendingItems > 0; }
    std::span<const TradeSkillRow> rows() const { return m_rows; }
    std::span<const TradeRecipe> recipes() const { return m_recipes; }
    std::span<const TradeHeader> headers() const { return m_headers; }
    // The selected recipe (TradeSkillFrame.selectedSkill), 0 none; its line in rows(), -1 when not shown.
    u32 selected() const { return m_selected; }
    i32 selectedRow() const;
    const TradeRecipe* selectedRecipe() const;
    // TradeSkillFrame_SetSelection: a recipe is selected; a header expands or collapses.
    void selectRow(u32 row);
    // TradeSkillCollapseAllButton: every header collapsed (no header expanded) / expand or collapse them all.
    bool allCollapsed() const;
    void setAllCollapsed(bool collapsed);
    // TradeSkillSubClassDropDown: -1 every subclass, else the header index shown alone.
    i32 subclassFilter() const { return m_subclassFilter; }
    void setSubclassFilter(i32 header);
    // TradeSkillInvSlotDropDown: the slots the recipes go in (bits of kTradeSlotKeys), -1 all or the bit
    // shown.
    u32 slotsPresent() const { return m_slotsPresent; }
    i32 slotFilter() const { return m_slotFilter; }
    void setSlotFilter(i32 slot);
    // GetTradeskillRepeatCount: what is left of a "Create All" of the selected recipe (1 otherwise).
    u32 repeatCount() const;
    // DoTradeSkill(selected, count): the count cut to what the reagents allow; more than one repeats it.
    bool create(u32 count);
    void closeTradeSkill(); // CloseTradeSkill (TradeSkillFrame's OnHide)

    // --- CraftFrame ---
    bool craftOpen() const { return m_craftList != 0; }
    u32 craftList() const { return m_craftList; }     // kCraftBeastTraining, kCraftEnchanting...
    u32 craftOpener() const { return m_craftOpener; } // the spell that opened it: its name is the title
    std::span<const CraftEntry> crafts() const { return m_craftRows; } // headers and visible crafts, in order
    u32 selectedCraft() const { return m_craftSelected; }
    i32 selectedCraftRow() const;
    void selectCraftRow(u32 row); // CraftFrame_SetSelection (a header collapses / expands)
    bool craftHeaders() const { return m_craftHeaders > 0; }
    bool craftsAllCollapsed() const;
    void setCraftsAllCollapsed(bool collapsed);
    // GetCraftDisplaySkillLine (Enchanting only): the first craft's skill line, rank and maximum; 0
    // otherwise.
    u32 craftSkillLine() const { return m_craftSkillLine; }
    u32 craftSkillRank() const { return m_craftSkillRank; }
    u32 craftSkillMax() const { return m_craftSkillMax; }
    // GetCraftInfo's trainingPointCost and requiredLevel: what teaching the pet this craft's ability costs
    // and the pet level it needs (the ability's row in the pet family's skill lines); false without a pet or
    // for crafts that teach the pet nothing.
    bool craftPetCost(u32 spellId, u32& trainingPoints, u32& requiredLevel) const;
    // GetPetTrainingPoints.
    PetTrainingPoints petTrainingPoints() const;
    void closeCraft(); // CloseCraft

    // The pet's family skill lines (0 without a pet or before its template came).
    std::array<u32, 2> petSkillLines() const;

private:
    struct PlayerSkill {
        u32 value = 0, max = 0, permBonus = 0;
        bool known = false;
    };
    const Entity* player() const;
    PlayerSkill skill(u32 skillLine) const;
    void touch() { ++m_version; }
    bool cast(u32 spellId);
    void stopRecast();

    void openTradeSkill(u32 skillLine);
    void rebuildTrade(); // the known recipes, their headers and sort (then flatten)
    void refreshTrade(); // colours and counts only (then flatten)
    void flattenTrade(); // rows() from the filters and the collapsed headers
    u32 available(const TradeSpellInfo& spell) const;

    void openCraft(u32 list, u32 opener);
    void rebuildCraft();
    void flattenCraft();
    RecipeColor craftColor(const TradeSpellInfo& spell, const SkillAbility* ability, u32 rank) const;
    bool petKnows(u32 spellId) const;
    bool petKnowsHigherRank(u32 spellId) const;
    u64 petSignature() const;

    const EntityStore& m_entities;
    QueryCache& m_queries;
    const Inventory& m_inventory;
    const SpellBook& m_spells;
    const Abilities& m_abilities;
    const TradeSkillData* m_data = nullptr;
    Caster m_caster;
    u64 m_player = 0;
    u32 m_version = 1;

    // What the open window was built from.
    u32 m_knownVersion = 0;
    u32 m_inventoryVersion = 0;
    u32 m_queryVersion = 0;
    u32 m_rankSeen = 0xFFFFFFFFu;
    u64 m_petSeen = 0;

    // TradeSkillFrame.
    u32 m_skillLine = 0;
    u32 m_lastSkillLine = 0; // the filters stay while the same profession reopens (0xbde064)
    u32 m_skillRank = 0, m_skillMax = 0;
    u32 m_pendingItems = 0;
    std::vector<TradeRecipe> m_recipes; // sorted: by header, then colour, item level (high first), name
    std::vector<TradeHeader> m_headers; // sorted: by item class, then name
    std::vector<TradeSkillRow> m_rows;
    std::vector<u32> m_known;         // the known recipes of the line (TRADE_SKILL_SHOW needs one)
    std::vector<TradeHeader> m_saved; // the headers' states across a rebuild
    u32 m_selected = 0;
    i32 m_subclassFilter = -1;
    i32 m_slotFilter = -1;
    u32 m_slotsPresent = 0;
    u32 m_recastSpell = 0; // DoTradeSkill's repeat (0xbde05c / 0xbde060)
    u32 m_recastCount = 0;

    // CraftFrame.
    u32 m_craftList = 0;
    u32 m_craftOpener = 0;
    u32 m_craftSelected = 0;
    u32 m_craftHeaders = 0;
    u32 m_craftSkillLine = 0, m_craftSkillRank = 0, m_craftSkillMax = 0;
    u32 m_craftRankSeen = 0;
    std::vector<CraftEntry> m_crafts;    // every craft and header, sorted
    std::vector<CraftEntry> m_craftRows; // the visible ones
    std::vector<u32> m_collapsedLines;   // headers collapsed (skill lines)
};

} // namespace vd::game
