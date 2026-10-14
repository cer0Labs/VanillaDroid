#include "app/TradeSkillGallery.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/TradeSkills.h"
#include "game/Travel.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace vd {
namespace {

using game::RecipeColor;

constexpr u64 icon(const char* name) {
    // Interface\Icons\<name>.blp in textures.vdpk.
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

constexpr u32 kHeaderColor = ui::rgba(255, 209, 0);

struct SampleReagent {
    const char* icon;
    const char* name;
    u32 have, need;
};
struct SampleRecipe {
    u32 header;
    const char* name;
    RecipeColor color;
    u32 available;
    const char* icon;
    const char* made;
    const char* requirements;
    std::array<SampleReagent, 3> reagents;
};

// Blacksmithing at 75 / 150 (a human warrior with a stack of copper and some stone).
constexpr const char* kHeaders[] = {"Consumable", "One-Handed Axes", "One-Handed Maces", "Mail",
                                    "Trade Goods"};
const SampleRecipe kRecipes[] = {
    {0,
     "Coarse Sharpening Stone",
     RecipeColor::Medium,
     4,
     "INV_Stone_SharpeningStone_02",
     "",
     "Anvil",
     {{{"INV_Stone_04", "Coarse Stone", 8, 2}}}},
    {0,
     "Rough Sharpening Stone",
     RecipeColor::Trivial,
     10,
     "INV_Stone_SharpeningStone_01",
     "",
     "Anvil",
     {{{"INV_Stone_04", "Rough Stone", 10, 1}}}},
    {0,
     "Rough Weightstone",
     RecipeColor::Trivial,
     5,
     "INV_Stone_WeightStone_01",
     "",
     "Anvil",
     {{{"INV_Stone_04", "Rough Stone", 10, 1}, {"INV_Fabric_Linen_01", "Linen Cloth", 5, 1}}}},
    {1,
     "Copper Axe",
     RecipeColor::Easy,
     2,
     "INV_Axe_04",
     "",
     "Anvil, |cffff2020Blacksmith Hammer|r",
     {{{"INV_Ingot_02", "Copper Bar", 14, 6}, {"INV_Misc_Flower_02", "Weak Flux", 3, 2}}}},
    {2,
     "Copper Mace",
     RecipeColor::Easy,
     2,
     "INV_Mace_01",
     "",
     "Anvil",
     {{{"INV_Ingot_02", "Copper Bar", 14, 6},
       {"INV_Misc_Flower_02", "Weak Flux", 3, 2},
       {"INV_Fabric_Linen_01", "Linen Cloth", 5, 2}}}},
    {3,
     "Runed Copper Bracers",
     RecipeColor::Optimal,
     0,
     "INV_Bracer_03",
     "",
     "Anvil",
     {{{"INV_Ingot_02", "Copper Bar", 14, 10},
       {"INV_Stone_GrindingStone_01", "Rough Grinding Stone", 1, 3}}}},
    {3,
     "Copper Chain Belt",
     RecipeColor::Medium,
     1,
     "INV_Belt_03",
     "",
     "Anvil",
     {{{"INV_Ingot_02", "Copper Bar", 14, 11}}}},
    {3,
     "Copper Chain Pants",
     RecipeColor::Trivial,
     1,
     "INV_Pants_03",
     "",
     "Anvil",
     {{{"INV_Ingot_02", "Copper Bar", 14, 8}}}},
    {4,
     "Coarse Grinding Stone",
     RecipeColor::Medium,
     4,
     "INV_Stone_GrindingStone_02",
     "",
     "",
     {{{"INV_Stone_04", "Coarse Stone", 8, 2}}}},
    {4,
     "Rough Grinding Stone",
     RecipeColor::Trivial,
     5,
     "INV_Stone_GrindingStone_01",
     "",
     "",
     {{{"INV_Stone_04", "Rough Stone", 10, 2}}}},
};

// Enchanting at 85 / 150 and Beast Training (a level 16 hunter's pet with 12 training points left).
struct SampleCraft {
    const char* name;
    const char* rank;
    RecipeColor color;
    u32 available;
    const char* icon;
    const char* tool; // Enchanting: the rod it needs (|c red when not carried)
    u32 petLevel;     // Beast Training: the pet level the ability needs (0: none)
    u32 points;       // and its training points
    const char* description;
    std::array<SampleReagent, 3> reagents;
};
const SampleCraft kEnchanting[] = {
    {"Enchant Chest - Minor Mana",
     "",
     RecipeColor::Medium,
     6,
     "Spell_Holy_GreaterHeal",
     "",
     0,
     0,
     "Adds a lasting +5 mana to a chest piece.",
     {{{"INV_Enchant_DustStrange", "Strange Dust", 6, 1}}}},
    {"Enchant Cloak - Minor Resistance",
     "",
     RecipeColor::Medium,
     1,
     "Spell_Holy_GreaterHeal",
     "|cffff2020Runed Copper Rod|r",
     0,
     0,
     "Gives a cloak a lasting +1 resistance against every school of magic.",
     {{{"INV_Enchant_DustStrange", "Strange Dust", 6, 1},
       {"INV_Enchant_EssenceMagicSmall", "Lesser Magic Essence", 1, 1}}}},
    {"Enchant Bracer - Minor Health",
     "",
     RecipeColor::Trivial,
     6,
     "Spell_Holy_GreaterHeal",
     "Runed Copper Rod",
     0,
     0,
     "Adds a lasting +5 health to bracers.",
     {{{"INV_Enchant_DustStrange", "Strange Dust", 6, 1}}}},
    {"Runed Copper Rod",
     "",
     RecipeColor::Trivial,
     1,
     "INV_Staff_Goldfeathered_01",
     "",
     0,
     0,
     "",
     {{{"INV_Ingot_02", "Copper Rod", 1, 1},
       {"INV_Enchant_DustStrange", "Strange Dust", 6, 1},
       {"INV_Enchant_EssenceMagicSmall", "Lesser Magic Essence", 1, 1}}}},
};
// A level 12 pet with 12 training points left.
constexpr u32 kPetLevel = 12;
constexpr u32 kPetPoints = 12;
const SampleCraft kBeastTraining[] = {
    {"Bite",
     "Rank 2",
     RecipeColor::None,
     0,
     "Ability_Racial_Cannibalize",
     "",
     8,
     4,
     "Your pet learns Bite (Rank 2).",
     {}},
    {"Bite",
     "Rank 3",
     RecipeColor::None,
     0,
     "Ability_Racial_Cannibalize",
     "",
     16,
     7,
     "Your pet learns Bite (Rank 3).",
     {}},
    {"Dash",
     "Rank 1",
     RecipeColor::None,
     0,
     "Ability_Druid_Dash",
     "",
     30,
     15,
     "Your pet learns Dash (Rank 1).",
     {}},
    {"Growl",
     "Rank 2",
     RecipeColor::None,
     0,
     "Ability_Physical_Taunt",
     "",
     10,
     1,
     "Your pet learns Growl (Rank 2).",
     {}},
    {"Bite",
     "Rank 1",
     RecipeColor::Used,
     0,
     "Ability_Racial_Cannibalize",
     "",
     1,
     1,
     "Your pet learns Bite (Rank 1).",
     {}},
    {"Cower",
     "Rank 1",
     RecipeColor::Used,
     0,
     "Ability_Druid_Cower",
     "",
     5,
     1,
     "Your pet learns Cower (Rank 1).",
     {}},
};

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

} // namespace

void TradeSkillGallery::draw(DialogContext& c, Page page) {
    VD_PROFILE_ZONE("TradeSkillGallery::draw");
    switch (page) {
    case Page::TradeSkill: tradeSkill(c); break;
    case Page::Craft: craft(c, false); break;
    case Page::BeastTraining: craft(c, true); break;
    }
}

void TradeSkillGallery::tradeSkill(DialogContext& c) {
    // The rows as GetTradeSkillInfo flattens them: each header, then its recipes unless folded.
    m_rows.clear();
    m_rowRecipe.clear();
    for (u32 h = 0; h < std::size(kHeaders); ++h) {
        if (m_subclass >= 0 && static_cast<u32>(m_subclass) != h) {
            continue;
        }
        m_rows.push_back({true, kHeaders[h], kHeaderColor, !m_collapsed[h], 0, {}, {}});
        m_rowRecipe.push_back(-1);
        for (u32 i = 0; i < std::size(kRecipes) && !m_collapsed[h]; ++i) {
            const SampleRecipe& r = kRecipes[i];
            if (r.header == h) {
                m_rows.push_back({false, r.name, recipeRowColor(r.color), true, r.available, {}, {}});
                m_rowRecipe.push_back(static_cast<i32>(i));
            }
        }
    }
    TradeSkillView v;
    v.title = "Blacksmithing";
    v.rankName = "Blacksmithing";
    v.rank = 75;
    v.maxRank = 150;
    v.rows = m_rows;
    v.hasRows = true;
    v.allCollapsed =
        std::all_of(m_collapsed.begin(), m_collapsed.begin() + std::size(kHeaders), [](bool b) { return b; });
    v.subclassText = m_subclass >= 0 ? std::string_view(kHeaders[m_subclass]) : text::get("ALL_SUBCLASSES");
    v.slotText = text::get("ALL_INVENTORY_SLOTS");
    std::array<ReagentView, 3> reagents{};
    u32 reagentCount = 0;
    for (u32 row = 0; row < m_rowRecipe.size(); ++row) {
        if (m_rowRecipe[row] == m_selected) {
            v.selected = static_cast<i32>(row);
        }
    }
    if (m_selected >= 0) {
        const SampleRecipe& r = kRecipes[m_selected];
        v.hasDetail = true;
        v.detail.iconHash = icon(r.icon);
        v.detail.name = r.name;
        v.detail.made = r.made;
        v.detail.requirements = r.requirements;
        v.canCreate = true;
        for (const SampleReagent& s : r.reagents) {
            if (s.name == nullptr) {
                continue;
            }
            ReagentView& rv = reagents[reagentCount++];
            rv.item.iconHash = icon(s.icon);
            rv.item.name = s.name;
            rv.have = s.have;
            rv.need = s.need;
            v.canCreate = v.canCreate && s.have >= s.need;
        }
        v.detail.reagents = {reagents.data(), reagentCount};
        if (m_selected == 1) {
            // As a transmute's would read: COOLDOWN_REMAINING and SecondsToTime.
            char time[64];
            const std::string_view left = game::secondsToTime(time, 2 * 3600 + 5 * 60);
            const std::string_view label = text::get("COOLDOWN_REMAINING");
            std::snprintf(m_cooldown.data(), m_cooldown.size(), "%.*s %.*s", static_cast<int>(label.size()),
                          label.data(), static_cast<int>(left.size()), left.data());
            v.detail.cooldown = m_cooldown.data();
        }
    }
    std::snprintf(m_countText.data(), m_countText.size(), "%u", m_count);
    v.countText = m_countText.data();
    const TradeSkillResult r = tradeSkillWindow(c, v);
    char line[96];
    if (r.select >= 0 && static_cast<usize>(r.select) < m_rowRecipe.size()) {
        const i32 recipe = m_rowRecipe[static_cast<usize>(r.select)];
        if (recipe < 0) {
            u32 h = 0;
            while (h < std::size(kHeaders) &&
                   std::string_view(kHeaders[h]) != m_rows[static_cast<usize>(r.select)].name) {
                ++h;
            }
            if (h < std::size(kHeaders)) {
                m_collapsed[h] = !m_collapsed[h];
            }
        } else {
            m_selected = recipe;
            m_count = 1;
        }
    }
    if (r.toggleAll) {
        const bool fold = !v.allCollapsed;
        m_collapsed.fill(fold);
    }
    if (r.subclassMenu) {
        m_menu = 1;
    }
    if (r.slotMenu) {
        m_menu = 2;
    }
    if (r.increment && m_count < 100) {
        ++m_count;
    }
    if (r.decrement && m_count > 1) {
        --m_count;
    }
    if (r.create || r.createAll) {
        std::snprintf(line, sizeof(line), "%s %s x %u", r.createAll ? "create all" : "create",
                      m_selected >= 0 ? kRecipes[m_selected].name : "-",
                      r.createAll && m_selected >= 0 ? kRecipes[m_selected].available : m_count);
        logResult(line);
    }
    if (r.countTapped) {
        logResult("count box tapped (the number keyboard opens)");
    }
    if (r.reagentTapped >= 0 || r.productTapped) {
        logResult("item tapped (its link goes to the chat while it is open)");
    }
    if (r.closed) {
        logResult("trade skill closed");
    }
    if (m_menu != 0) {
        m_choices.clear();
        if (m_menu == 1) {
            m_choices.push_back({text::get("ALL_SUBCLASSES"), ui::kWhite, m_subclass < 0, true});
            for (u32 h = 0; h < std::size(kHeaders); ++h) {
                m_choices.push_back({kHeaders[h], ui::kWhite, m_subclass == static_cast<i32>(h), true});
            }
        } else {
            m_choices.push_back({text::get("ALL_INVENTORY_SLOTS"), ui::kWhite, true, true});
            for (const char* key : {"WAISTSLOT", "LEGSSLOT", "WRISTSLOT", "MAINHANDSLOT", "NONEQUIPSLOT"}) {
                m_choices.push_back({text::get(key), ui::kWhite, false, true});
            }
        }
        DialogContext modal = c;
        modal.panel = {};
        const i32 picked =
            choiceList(modal, "tradeskill.menu", m_menu == 1 ? v.subclassText : v.slotText, m_choices);
        if (picked >= 0 && m_menu == 1) {
            m_subclass = picked - 1;
        }
        if (picked != -1) {
            m_menu = 0;
        }
    }
}

void TradeSkillGallery::craft(DialogContext& c, bool beasts) {
    const std::span<const SampleCraft> crafts =
        beasts ? std::span<const SampleCraft>(kBeastTraining) : std::span<const SampleCraft>(kEnchanting);
    m_rows.clear();
    for (u32 i = 0; i < crafts.size(); ++i) {
        const SampleCraft& s = crafts[i];
        RecipeRowView row{false, s.name, recipeRowColor(s.color), true, s.available, {}, {}};
        if (s.rank[0] != '\0') {
            row.sub = text::format(m_subs[i], "PARENS_TEMPLATE", s.rank);
        }
        if (s.points > 0) {
            row.cost = text::format(m_costs[i], "TRAINER_LIST_TP", s.points);
        }
        m_rows.push_back(row);
    }
    const i32 selected = std::min<i32>(m_craftSelected, static_cast<i32>(crafts.size()) - 1);
    CraftView v;
    v.title = beasts ? "Beast Training" : "Enchanting";
    if (!beasts) {
        v.rankName = "Enchanting";
        v.rank = 85;
        v.maxRank = 150;
    }
    v.rows = m_rows;
    v.selected = selected;
    v.button = text::get(beasts ? "TRAIN" : "ENSCRIBE");
    v.trainingPoints = beasts ? static_cast<i32>(kPetPoints) : -1;
    std::array<ReagentView, 3> reagents{};
    u32 reagentCount = 0;
    const SampleCraft& s = crafts[static_cast<u32>(selected)];
    v.hasDetail = true;
    v.detail.iconHash = icon(s.icon);
    v.detail.name = s.name;
    v.detail.requirements = s.tool;
    if (s.petLevel > 0) {
        v.detail.requirements =
            text::format(m_requirements,
                         kPetLevel >= s.petLevel ? "TRAINER_PET_LEVEL" : "TRAINER_PET_LEVEL_RED", s.petLevel);
    }
    if (s.points > 0) {
        const std::string_view costLabel = text::get("COSTS_LABEL");
        const std::string_view pointsLabel = text::get("TRAINING_POINTS_LABEL");
        const bool enough = kPetPoints >= s.points;
        std::snprintf(m_cost.data(), m_cost.size(), "%.*s %s%u%s %.*s", static_cast<int>(costLabel.size()),
                      costLabel.data(), enough ? "" : "|cffff2020", s.points, enough ? "" : "|r",
                      static_cast<int>(pointsLabel.size()), pointsLabel.data());
        v.detail.cost = m_cost.data();
    }
    v.detail.description = s.description;
    v.canCraft = s.color != RecipeColor::Used;
    for (const SampleReagent& r : s.reagents) {
        if (r.name == nullptr) {
            continue;
        }
        ReagentView& rv = reagents[reagentCount++];
        rv.item.iconHash = icon(r.icon);
        rv.item.name = r.name;
        rv.have = r.have;
        rv.need = r.need;
        v.canCraft = v.canCraft && r.have >= r.need;
    }
    v.detail.reagents = {reagents.data(), reagentCount};
    const CraftResult r = craftWindow(c, v);
    if (r.select >= 0) {
        m_craftSelected = r.select;
    }
    char line[96];
    if (r.craft) {
        std::snprintf(line, sizeof(line), "%s %s", beasts ? "train" : "enchant", s.name);
        logResult(line);
    }
    if (r.closed) {
        logResult("craft closed");
    }
}

} // namespace vd
