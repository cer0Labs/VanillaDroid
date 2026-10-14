#pragma once

#include "app/Dialogs.h"
#include "app/HelpWindows.h"
#include "app/SocialWindows.h"
#include "core/Types.h"
#include "game/HelpData.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

namespace game {
class GameData;
}
namespace pack {
class PackReader;
}
namespace ui {
class TextInput;
}

class GameSession;
class PopupScreens;
class SocialScreens;

// Phase 15 (help): the game's side of the help windows (app/HelpWindows): HelpFrame's pages (UiPanel::Help, a
// center window opened by the micro menu's Help button: ToggleHelpFrame), the GM ticket it opens, edits and
// abandons (game::HelpDesk), "Stuck" (the player's SPELL_EFFECT_STUCK spell), TicketStatusFrame at the top
// right (its button: HELP_TICKET or the GM survey), GMSurveyFrame (UiPanel::GmSurvey) and SuggestFrame
// (UiPanel::Suggest: bugs and suggestions, CMSG_BUG). GameScreens owns it and draws the panels; the windows'
// slots are their state. Main thread.
class HelpScreens {
public:
    // `db`: GMTicketCategory and the GM survey tables (null: no categories, no survey).
    void init(const game::GameData* data, const pack::PackReader* db);
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    void setSocialScreens(const SocialScreens* social) { m_socialScreens = social; }
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void setMessageSink(MessageSink sink, void* context) {
        m_message = sink;
        m_messageContext = context;
    }

    // Once a frame in the world: the help desk's notices (UIErrorsFrame), the queue's alert.
    void update(GameSession& session, u32 nowMs);
    void leaveWorld();

    // HelpFrame_OnShow (the panel opened): the home page, GetGMStatus.
    void opened(GameSession& session);
    // HELP_TICKET's "Edit My Ticket": the ticket page (GameScreens shows the panel first).
    void openTicketPage(GameSession& session);
    // What the panels' buttons want next frame (GameScreens toggles the slots): read once.
    enum class Want : u8 { None, CloseHelp, OpenSurvey, OpenSuggest, CloseSurvey, CloseSuggest };
    Want takeWant();

    void drawHelp(GameSession& session, DialogContext& c, ui::TextInput& text);
    void drawSurvey(GameSession& session, DialogContext& c, ui::TextInput& text);
    void drawSuggest(GameSession& session, DialogContext& c, ui::TextInput& text);
    // TicketStatusFrame at the right end of `area`'s top (the space left of the minimap), when a ticket is
    // open or a survey offered. `helpShown`: HelpFrame is up (the button does not ask then).
    void drawStatus(GameSession& session, DialogContext& c, const ui::Rect& area, bool helpShown, u64 nowMs);
    // Development (gallery, VD_HELP_DEMO): a page with sample state.
    const game::HelpData& data() const { return m_data; }

private:
    void openTicket(GameSession& session, u8 category);
    void message(u32 color, std::string_view line);

    const game::GameData* m_gameData = nullptr;
    game::HelpData m_data;
    PopupScreens* m_popups = nullptr;
    const SocialScreens* m_socialScreens = nullptr;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;

    HelpPage m_page = HelpPage::Home;
    u32 m_category = 0;  // the General page's (GMTicketCategory id)
    u8 m_ticketType = 0; // HelpFrameOpenTicket.ticketType
    std::string m_ticketText;
    bool m_ticketLoaded = false;
    std::vector<HelpCategoryView> m_categories;
    Want m_want = Want::None;
    // GMSurveyFrame.
    std::array<i32, 10> m_answers{};
    std::string m_comment;
    std::vector<SurveyQuestionView> m_questions;
    // SuggestFrame.
    i32 m_bugCategory = 0; // 0: BUG_CATEGORY_CHOOSE, 1..15 BUG_CATEGORY1..15
    std::string m_suggestText;
    std::string m_playerInfo;
    bool m_categoryMenu = false;
    std::vector<ChoiceView> m_choices;
    std::array<char, 160> m_wait{};
    TicketStatusState m_status; // where the player dragged TicketStatusFrame
    u32 m_noticeCursor = 0;
    u64 m_cursorsPlayer = 0;
};

} // namespace vd
