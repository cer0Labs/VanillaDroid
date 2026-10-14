#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "game/TradeSkills.h"
#include "ui/Context.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 13 (professions): the TradeSkillFrame (Blizzard_TradeSkillUI) and the CraftFrame (Blizzard_CraftUI)
// as pure views like app/Dialogs: a view-model in, what the player did out; no game state, no network. Laid
// out as their XML lays them out (the rank bar, the "All" collapse button and the two filter drop-downs over
// the recipe list, the selected recipe's detail under it, Create All / - count + / Create / Exit at the
// bottom), in the panel place app/UiPanels gives them ("left", as UIPanelWindows has them); on wide places
// the detail goes beside the list. Touch: a tap is the original's click; a long press on the product or a
// reagent is the hover (its tooltip, through kit::takeLongPressedItem); with the chat's edit box open a tap
// on them is the Shift + click that links them; the drop-downs open their lists in a small modal panel (the
// glue draws it: app/SocialWindows choiceList); a tap on the count box opens the number keyboard. Recipe
// names are client data; the interface's words come from core/Text (the client's keys: CREATE, CREATE_ALL,
// REQUIRES_LABEL, SPELL_REAGENTS, COOLDOWN_REMAINING, TRAIN...).

// TradeSkillTypeColor / CraftTypeColor of a recipe's colour (CraftSubTypeColor's darker green for "none" is
// only the sub-text's; this is the name's).
u32 recipeRowColor(game::RecipeColor color);

struct RecipeRowView {
    bool header = false;
    std::string_view name;
    u32 color = 0;         // recipeRowColor, or the headers' gold
    bool expanded = true;  // headers: the minus button (else the plus)
    u32 available = 0;     // " [n]" after the name when > 0
    std::string_view sub;  // crafts: "(Rank 2)" (PARENS_TEMPLATE) after the name
    std::string_view cost; // Beast Training: "10 TP" (TRAINER_LIST_TP) at the right
};

struct ReagentView {
    ItemView item; // icon, name, entry (its tooltip on a long press)
    u32 have = 0;  // carried (shown as "*" from 100)
    u32 need = 0;
};

// The selected recipe (TradeSkillDetailScrollChildFrame / CraftDetailScrollChildFrame).
struct RecipeDetailView {
    u64 iconHash = 0;
    u32 itemEntry = 0; // the product (TradeSkillSkillIcon's tooltip and link), 0 none
    std::string_view name;
    std::string_view made; // "2", "1-3" or "~3" on the icon (empty: one)
    std::string_view
        requirements; // the tools and the focus (BuildColoredListString: |c codes for what is missing)
    std::string_view cooldown;    // "Cooldown remaining: 2 Hrs 5 Mins" (red), empty: none
    std::string_view cost;        // crafts: "Cost: 10 Training Points" (a |c code when too few)
    std::string_view description; // crafts: GetCraftDescription
    std::span<const ReagentView> reagents;
};

// --- TradeSkillFrame ---------------------------------------------------------------------------------------

struct TradeSkillView {
    std::string_view title;    // TRADE_SKILL_TITLE with the skill line ("Blacksmithing")
    std::string_view rankName; // TradeSkillRankFrame: the skill line, rank / max
    u32 rank = 0, maxRank = 0;
    std::span<const RecipeRowView> rows;
    i32 selected = -1;             // the selected row
    bool allCollapsed = false;     // TradeSkillCollapseAllButton shows the plus
    bool hasRows = false;          // the collapse button and the filters work
    std::string_view subclassText; // the drop-downs' current choice ("All Subclasses", "Daggers"...)
    std::string_view slotText;
    bool hasDetail = false;
    RecipeDetailView detail;
    std::string_view countText; // TradeSkillInputBox
    bool countEditing = false;
    bool canCreate = false; // every reagent carried (TradeSkillCreateButton / CreateAllButton enabled)
};
struct TradeSkillResult {
    i32 select = -1; // a row tapped (a header expands or collapses)
    bool toggleAll = false;
    bool subclassMenu = false;
    bool slotMenu = false;
    bool decrement = false, increment = false, countTapped = false;
    bool create = false, createAll = false;
    bool productTapped = false; // the icon (a link while the chat is open)
    i32 reagentTapped = -1;
    bool closed = false; // the X or Exit
};
TradeSkillResult tradeSkillWindow(DialogContext& c, const TradeSkillView& v);

// --- CraftFrame --------------------------------------------------------------------------------------------

struct CraftView {
    std::string_view title;    // GetCraftName: the spell that opened it ("Enchanting", "Beast Training")
    std::string_view rankName; // GetCraftDisplaySkillLine (Enchanting); empty: no rank bar
    u32 rank = 0, maxRank = 0;
    std::span<const RecipeRowView> rows;
    i32 selected = -1;
    bool headers = false; // CraftExpandButtonFrame shown
    bool allCollapsed = false;
    bool hasDetail = false;
    RecipeDetailView detail;
    std::string_view button; // GetCraftButtonToken: "Enchant", "Train"
    bool canCraft = false;
    i32 trainingPoints = -1; // CraftFramePointsText (-1: hidden, no pet points)
};
struct CraftResult {
    i32 select = -1;
    bool toggleAll = false;
    bool craft = false; // DoCraft
    bool productTapped = false;
    i32 reagentTapped = -1;
    bool closed = false;
};
CraftResult craftWindow(DialogContext& c, const CraftView& v);

// A drop-down's field (UIDropDownMenuTemplate): its current text and the arrow; returns true when tapped.
bool dropDownField(DialogContext& c, ui::WidgetId id, const ui::Rect& r, std::string_view text);

} // namespace vd
