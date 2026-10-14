#pragma once

#include "app/Dialogs.h"
#include "app/SocialWindows.h"
#include "app/TradeSkillWindows.h"
#include "core/Types.h"

#include <array>
#include <vector>

namespace vd {

// Development (phase 13, VD_UI_GALLERY=tradeskill|craft|beasttraining): the profession windows with sample
// data, drawn by the same views (app/TradeSkillWindows): a level 75 / 150 blacksmith (headers by the created
// items' subclass, every difficulty colour, a reagent missing, the anvil and the hammer), an enchanter (the
// rank bar, a craft that needs an item) and a hunter's Beast Training (training points, ranks the pet has, a
// pet level too low). Taps select, headers fold, the drop-downs open their lists, the count changes; what the
// player asks for goes to the log. Sample names are English, as the client's Spell.dbc and the server's item
// templates have them.
class TradeSkillGallery {
public:
    enum class Page : u8 { TradeSkill, Craft, BeastTraining };
    void draw(DialogContext& c, Page page);

private:
    void tradeSkill(DialogContext& c);
    void craft(DialogContext& c, bool beasts);

    i32 m_selected = 1; // TradeSkillFrame: the selected sample recipe
    i32 m_craftSelected = 0;
    std::array<bool, 8> m_collapsed{};
    u32 m_count = 1;
    i32 m_subclass = -1;
    u8 m_menu = 0; // 1 subclass, 2 slots
    std::vector<RecipeRowView> m_rows;
    std::vector<i32> m_rowRecipe; // row -> sample recipe (-1 header)
    std::vector<ChoiceView> m_choices;
    std::array<char, 8> m_countText{};
    std::array<char, 96> m_cooldown{};
    std::array<char, 96> m_requirements{};
    std::array<char, 96> m_cost{};
    std::array<std::array<char, 32>, 8> m_subs{};
    std::array<std::array<char, 32>, 8> m_costs{};
};

} // namespace vd
