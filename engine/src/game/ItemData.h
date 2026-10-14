#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// An item set (ItemSet.dbc): its pieces and the bonuses they give.
struct ItemSetInfo {
    u32 id = 0;
    std::string_view name; // enUS
    std::array<u32, 17> items{};
    u32 itemCount = 0;
    std::array<u32, 8> spells{};     // bonus spells
    std::array<u32, 8> thresholds{}; // pieces each bonus needs
    u32 requiredSkill = 0;
    u32 requiredSkillRank = 0;
};

// Client tables the items need (phase 8): repair costs, bank slot prices, sets, enchantments, random suffixes, skill,
// faction, class, race and item subclass names, and the spell texts of "Use:" / "Equip:" lines with their $ tokens
// resolved. The DBCs stay in the mapped db pack; this only indexes them (1.12.1 layouts, field numbers from vMaNGOS'
// DBCStructure.h / DBCfmt.h and SpellEntry.h, checked against the real data in tests/game/test_inventory.cpp). Read
// only after init: any thread.
class ItemData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    // What repairing `lost` durability points costs, as Player::DurabilityRepair: DurabilityCosts.dbc (by item level,
    // a column per weapon / armor subclass) x DurabilityQuality.dbc ((quality + 1) * 2). At least 1 copper once
    // something is lost; 0 without loss or tables. Without the vendor's reputation discount.
    u32 repairCost(u32 itemClass, u32 subclass, u32 quality, u32 itemLevel, u32 lost) const;
    // Price of the n-th bank bag slot (1..6, BankBagSlotPrices.dbc); 0 when there is no such slot.
    u32 bankSlotPrice(u32 slot) const;
    std::optional<ItemSetInfo> itemSet(u32 id) const;
    // SpellItemEnchantment.dbc: the text an enchantment shows on the tooltip ("+5 Agility"); empty if unknown.
    std::string_view enchantmentText(u32 id) const;
    // ItemRandomProperties.dbc: the name suffix ("of the Bear") and its three enchantments.
    std::string_view randomSuffix(u32 id) const;
    std::array<u32, 3> randomEnchantments(u32 id) const;
    // SkillLine.dbc / Faction.dbc names (enUS), empty if unknown.
    std::string_view skillName(u32 id) const;
    std::string_view factionName(u32 id) const;
    // ChrClasses.dbc / ChrRaces.dbc names (enUS: "Warrior", "Night Elf"), empty if unknown.
    std::string_view className(u32 id) const;
    std::string_view raceName(u32 id) const;
    // ChrRaces.dbc's base language is Orcish: one of the Horde's races (PvP rank titles differ by side).
    bool hordeRace(u32 id) const;
    // ItemSubClass.dbc: the name the tooltip shows for an item's class and subclass ("Sword", "Cloth", "Arrow",
    // "Soul Bag", "Quiver"); empty if unknown.
    std::string_view subclassName(u32 itemClass, u32 subclass) const;

    // A spell's description (Spell.dbc) with the client's tokens resolved: $s1..3 (values, "70 to 90" with dice),
    // $m1..3, $o1..3 (total over time), $t1..3 (period), $a1..3 (radius), $x1..3 (chain targets), $d (duration),
    // $h (proc chance), $n (charges), $u (stack), $z ("your home"), references to other spells ($12345s1, $12345d),
    // $/10;s1 and $*2;s1 factors and $l / $g singular:plural and male:female choices. Unknown tokens are left as
    // they are. The words it adds ("70 to 90", "6 sec") are the client's texts (core/Text: INT_SPELL_*), in the
    // client's language like the description itself. False when the spell or its description is unknown.
    bool spellDescription(u32 spellId, std::string& out) const;
    std::string_view spellName(u32 spellId) const;

private:
    bool spellRecord(u32 spellId, u32& record) const;
    void appendToken(std::string& out, u32 spellId, char token, u32 index, f32 factor) const;

    bool m_ready = false;
    db::DbcView m_costs;
    db::DbcView m_qualities;
    db::DbcView m_bankSlots;
    db::DbcView m_sets;
    db::DbcView m_enchantments;
    db::DbcView m_randomProperties;
    db::DbcView m_skills;
    db::DbcView m_factions;
    db::DbcView m_spells;
    db::DbcView m_durations;
    db::DbcView m_radii;
    db::DbcView m_classes;
    db::DbcView m_races;
    db::DbcView m_subclasses;
    std::unordered_map<u32, u32> m_costRecords;
    std::unordered_map<u32, u32> m_qualityRecords;
    std::unordered_map<u32, u32> m_bankSlotRecords;
    std::unordered_map<u32, u32> m_setRecords;
    std::unordered_map<u32, u32> m_enchantmentRecords;
    std::unordered_map<u32, u32> m_randomPropertyRecords;
    std::unordered_map<u32, u32> m_skillRecords;
    std::unordered_map<u32, u32> m_factionRecords;
    std::unordered_map<u32, u32> m_spellRecords;
    std::unordered_map<u32, u32> m_durationRecords;
    std::unordered_map<u32, u32> m_radiusRecords;
    std::unordered_map<u32, u32> m_classRecords;
    std::unordered_map<u32, u32> m_raceRecords;
    std::unordered_map<u32, u32> m_subclassRecords; // (class << 8 | subclass) -> record
};

} // namespace vd::game
