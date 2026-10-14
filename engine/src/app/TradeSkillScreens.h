#pragma once

#include "app/SocialWindows.h"
#include "app/TradeSkillWindows.h"
#include "core/Types.h"
#include "game/TradeSkills.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

namespace game {
class AbilityData;
class GameData;
} // namespace game
namespace ui {
class TextInput;
}

class GameSession;
class UiResources;

// Phase 13 (professions): the game's side of the TradeSkillFrame and the CraftFrame: fills their views
// (app/TradeSkillWindows) from game::TradeSkills and applies what the player did. GameScreens owns it and
// draws the windows in their panel place (UiPanel::TradeSkill / Craft). Main thread; the views are rebuilt
// only when the game state changed (TradeSkills::version, the bags, a template that arrived), into reserved
// storage.
class TradeSkillScreens {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities, UiResources* resources);

    // The panels (GameScreens: panelWanted / closePanel / drawPanel).
    static bool tradeSkillOpen(const GameSession& session);
    static bool craftOpen(const GameSession& session);
    void closeTradeSkill(GameSession& session);
    void closeCraft(GameSession& session);
    void drawTradeSkill(GameSession& session, DialogContext& c, ui::TextInput& input, u32 nowMs);
    void drawCraft(GameSession& session, DialogContext& c, u32 nowMs);

private:
    enum class Menu : u8 { None, Subclass, Slot };
    struct RowText {
        std::array<char, 24> sub{};
        std::array<char, 24> cost{};
    };

    // VD_TRACE_GAMEPLAY: what the player asked for, in the log.
    void trace(const char* format, ...) const;
    ItemView item(GameSession& session, u32 entry);
    // The detail of `spellId` into m_detail / m_reagents (the selected recipe or craft).
    void buildDetail(GameSession& session, u32 spellId, bool craft, u32 nowMs);
    void castCraft(GameSession& session, u32 spellId);
    void menu(GameSession& session, DialogContext& c);
    void syncCount(ui::TextInput& input);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false;

    // TradeSkillFrame.
    u32 m_tradeVersion = 0;        // TradeSkills::version the rows were built for
    u32 m_tradeItems = 0;          // QueryCache::version (reagents' names and icons that came)
    u32 m_tradeBags = 0;           // Inventory::version (the reagents' counts)
    u32 m_tradeCooldownSecond = 0; // the cooldown text's second
    std::vector<RecipeRowView> m_rows;
    TradeSkillView m_tradeView;
    u32 m_count = 1;         // TradeSkillInputBox
    u32 m_countSelected = 0; // the selection the count was set for
    u32 m_countRepeat = 1;   // the repeat count last shown
    bool m_editing = false;
    std::array<char, 8> m_countText{};
    Menu m_menu = Menu::None;
    std::vector<ChoiceView> m_choices;
    std::vector<i32> m_choiceValues;

    // CraftFrame.
    u32 m_craftVersion = 0;
    u32 m_craftItems = 0;
    u32 m_craftBags = 0;
    std::vector<RecipeRowView> m_craftRows;
    std::vector<RowText> m_craftTexts;
    CraftView m_craftView;

    // The selected recipe's detail (one window draws at a time).
    RecipeDetailView m_detail;
    std::vector<ReagentView> m_reagents;
    std::array<char, 16> m_made{};
    std::array<char, 192> m_requirements{};
    std::array<char, 96> m_cooldown{};
    std::array<char, 128> m_cost{};
    std::string m_description;
    std::array<char, 64> m_rankText{};
    std::array<char, 96> m_title{};
};

} // namespace vd
