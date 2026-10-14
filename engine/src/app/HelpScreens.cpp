#include "app/HelpScreens.h"

#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "app/SocialScreens.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/SocialData.h"
#include "net/UpdateFields.gen.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

namespace uf = net::uf;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26); // UI_ERROR_MESSAGE
constexpr u32 kInfoColor = rgba(255, 209, 0);  // NORMAL_FONT_COLOR: GMSURVEY_SUBMITTED's AddMessage

constexpr ui::WidgetId kTicketField = ui::widgetId("help.ticketEdit");
constexpr ui::WidgetId kCommentField = ui::widgetId("gmSurvey.commentEdit");
constexpr ui::WidgetId kSuggestField = ui::widgetId("suggest.textEdit");

// The two entries this client adds to the GM categories (HELPFRAME_BUG_* and HELPFRAME_SUGGESTION_*: 1.12's
// strings for the help frame's bug and suggestion buttons, left from an earlier HelpFrame; they open
// SuggestFrame).
constexpr u32 kBugEntries = 2;

} // namespace

void HelpScreens::init(const game::GameData* data, const pack::PackReader* db) {
    m_gameData = data;
    if (db) {
        m_data.init(*db);
    }
    m_categories.reserve(m_data.categories().size() + kBugEntries);
    m_questions.reserve(10);
    m_choices.reserve(16);
    m_ticketText.reserve(512);
    m_comment.reserve(512);
    m_suggestText.reserve(1024);
    m_playerInfo.reserve(256);
    m_answers.fill(-1);
}

void HelpScreens::message(u32 color, std::string_view line) {
    if (m_message) {
        char buffer[256];
        const usize n = std::min(line.size(), sizeof(buffer) - 1);
        std::copy_n(line.data(), n, buffer);
        buffer[n] = '\0';
        m_message(m_messageContext, color, false, buffer);
    }
}

void HelpScreens::leaveWorld() {
    m_cursorsPlayer = 0;
    m_want = Want::None;
    m_page = HelpPage::Home;
}

HelpScreens::Want HelpScreens::takeWant() {
    const Want w = m_want;
    m_want = Want::None;
    return w;
}

void HelpScreens::update(GameSession& session, u32 nowMs) {
    (void)nowMs;
    game::HelpDesk& help = session.play().help();
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        m_noticeCursor = help.notices().end();
    }
    while (const game::GuildNotice* n = help.notices().next(m_noticeCursor)) {
        message(kErrorColor, text::get(n->key));
    }
    if (help.takeQueueDisabledAlert() && m_popups) {
        m_popups->askHelpQueueDisabled(session); // UPDATE_GM_STATUS -1
    }
}

void HelpScreens::opened(GameSession& session) {
    m_page = HelpPage::Home; // HelpFrame_OnShow: HelpFrame_ShowFrame("Home"), GetGMStatus()
    session.play().help().requestStatus();
    if (m_popups) {
        m_popups->hideHelpTicket(); // ToggleHelpFrame
    }
}

void HelpScreens::openTicketPage(GameSession& session) {
    const game::GmTicketState& t = session.play().help().ticket();
    openTicket(session, t.open ? t.category : m_ticketType);
}

void HelpScreens::openTicket(GameSession& session, u8 category) {
    game::HelpDesk& help = session.play().help();
    if (!help.queueActive()) { // HelpFrame_ShowFrame("OpenTicket") while the queue is down
        m_want = Want::CloseHelp;
        if (m_popups) {
            m_popups->askHelpQueueDisabled(session);
        }
        return;
    }
    m_page = HelpPage::OpenTicket;
    m_ticketType = category;
    m_ticketLoaded =
        false; // HelpFrameOpenTicketDropDown_OnShow: GetGMTicket (the text comes with UPDATE_TICKET)
    help.requestTicket();
}

void HelpScreens::drawHelp(GameSession& session, DialogContext& c, ui::TextInput& text) {
    VD_PROFILE_ZONE("HelpScreens::help");
    game::Gameplay& play = session.play();
    game::HelpDesk& help = play.help();
    const game::GmTicketState& ticket = help.ticket();
    HelpView v;
    v.page = m_page;
    if (m_page == HelpPage::Categories) {
        m_categories.clear();
        for (const game::HelpData::Category& cat : m_data.categories()) {
            m_categories.push_back({cat.name, {}});
        }
        m_categories.push_back(
            {text::get("HELPFRAME_BUG_BUTTON_TEXT"), text::get("HELPFRAME_BUG_BUTTON_DESCRIPTION")});
        m_categories.push_back({text::get("HELPFRAME_SUGGESTION_BUTTON_TEXT"),
                                text::get("HELPFRAME_SUGGESTION_BUTTON_DESCRIPTION")});
        v.categories = m_categories;
    }
    if (m_page == HelpPage::General) {
        v.general = helpGeneralPage(m_category);
    }
    const bool editing = text.active() && text.field() == kTicketField;
    if (m_page == HelpPage::OpenTicket) {
        if (editing) {
            m_ticketText.assign(text.text());
        } else if (!m_ticketLoaded && ticket.known) {
            // HelpFrameOpenTicket_OnEvent: the open ticket's text and category, or an empty box.
            m_ticketText.assign(ticket.open ? std::string_view(ticket.text) : std::string_view{});
            if (ticket.open) {
                m_ticketType = ticket.category;
            }
            m_ticketLoaded = true;
        }
        v.hasTicket = ticket.open;
        v.ticketText = m_ticketText;
        v.editing = editing;
    }
    const HelpResult r = helpWindow(c, v);
    if (r.toCategories) {
        m_page = HelpPage::Categories; // HelpFrame_ShowFrame("GMHome")
    }
    if (r.category >= 0) {
        const u32 i = static_cast<u32>(r.category);
        const std::span<const game::HelpData::Category> cats = m_data.categories();
        if (i < cats.size()) {
            // HelpFrameButton_OnClick: the category's frame (2: harassment, the rest general) and the
            // ticket's type.
            m_category = cats[i].id;
            m_ticketType = static_cast<u8>(cats[i].id);
            m_page = m_category == net::kTicketHarassment ? HelpPage::Harassment : HelpPage::General;
        } else {
            m_want = Want::OpenSuggest; // ours: the bug and suggestion entries open SuggestFrame
            m_bugCategory = 0;
            m_suggestText.clear();
        }
    }
    if (r.generalButton) {
        if (m_category == net::kTicketStuck) {
            // HelpFrameUnstick_OnClick: Stuck() (the player's SPELL_EFFECT_STUCK spell) and the frame hides.
            const game::SpellSource source =
                m_gameData ? game::spellSource(*m_gameData) : game::SpellSource{};
            if (const u32 spell = game::HelpDesk::stuckSpell(play.spells().known(), source)) {
                play.castSpell(spell, 0);
            }
            m_want = Want::CloseHelp;
        } else {
            openTicket(session, m_ticketType); // HelpFrameGeneralButton_OnClick
        }
    }
    if (r.generalButton2 || r.reportVerbal || r.reportPhysical) {
        openTicket(session, m_ticketType);
    }
    if (r.ticketTapped && !editing) {
        text.begin(kTicketField, m_ticketText, net::kTicketTextMax, ui::TextKind::Message);
    }
    if (r.submit) { // HelpFrameOpenTicketSubmit_OnClick
        const game::Entity* me = session.entities().find(session.playerGuid());
        if (ticket.open) {
            help.updateText(m_ticketType, m_ticketText);
        } else if (me) {
            const glm::vec3 at = me->positionAt(session.lastUpdateMs());
            help.create(m_ticketType, m_ticketText, play.travel().zone().map, at.x, at.y, at.z);
        }
        if (editing) {
            text.end();
        }
        m_want = Want::CloseHelp;
    }
    if (r.back) {
        m_page = m_page == HelpPage::Categories ? HelpPage::Home : HelpPage::Categories;
        if (editing) {
            text.end();
        }
    }
    if (r.closed) {
        if (m_page == HelpPage::OpenTicket) {
            help.requestTicket(); // HelpFrameOpenTicketCancel: GetGMTicket()
        }
        if (editing) {
            text.end();
        }
        m_want = Want::CloseHelp;
    }
}

void HelpScreens::drawStatus(GameSession& session, DialogContext& c, const ui::Rect& area, bool helpShown,
                             u64 nowMs) {
    game::HelpDesk& help = session.play().help();
    const game::GmTicketState& t = help.ticket();
    if (!t.open && !help.surveyOffered()) {
        return; // TicketStatusFrame:Hide()
    }
    TicketStatusView v;
    if (help.surveyOffered()) {
        v.title = text::get("CHOSEN_FOR_GMSURVEY");
        v.flash = true;
    } else {
        v.title = text::get("TICKET_STATUS1");
        const game::TicketWait w = game::HelpDesk::waitText(t, static_cast<u32>(nowMs));
        if (w.key) {
            if (w.seconds > 0) {
                char time[64];
                v.time = text::format(m_wait, w.key, game::secondsToTime(time, w.seconds, true));
            } else {
                v.time = text::get(w.key);
            }
        }
    }
    if (ticketStatusFrame(c, area, v, m_status, nowMs)) {
        // TicketStatusButton's OnClick.
        if (help.surveyOffered()) {
            m_want = Want::OpenSurvey;
        } else if (m_popups && m_popups->helpTicketShown()) {
            m_popups->hideHelpTicket();
        } else if (!helpShown && m_popups) {
            m_popups->askHelpTicket(session);
        }
    }
}

void HelpScreens::drawSurvey(GameSession& session, DialogContext& c, ui::TextInput& text) {
    VD_PROFILE_ZONE("HelpScreens::survey");
    const std::span<const game::HelpData::Question> questions = m_data.questions();
    m_questions.clear();
    for (u32 i = 0; i < questions.size(); ++i) {
        m_questions.push_back({questions[i].text, m_answers[i]});
    }
    const bool editing = text.active() && text.field() == kCommentField;
    if (editing) {
        m_comment.assign(text.text());
    }
    GmSurveyView v;
    v.questions = m_questions;
    v.comment = m_comment;
    v.editing = editing;
    const GmSurveyResult r = gmSurveyWindow(c, v);
    if (r.question >= 0) {
        m_answers[static_cast<u32>(r.question)] = r.answer; // GMSurveyRadioButton_OnClick
    }
    if (r.commentTapped && !editing) {
        text.begin(kCommentField, m_comment, net::kTicketTextMax, ui::TextKind::Message);
    }
    if (r.submit) {
        // GMSurveySubmitButton_OnClick: each question's radio button (none: 0), the comment, then the survey.
        std::array<net::GmSurveyAnswer, 10> answers{};
        for (u32 i = 0; i < questions.size(); ++i) {
            answers[i] = {questions[i].id, static_cast<u8>(std::max(m_answers[i], 0)), {}};
        }
        session.play().help().submitSurvey(m_data.survey(), {answers.data(), questions.size()}, m_comment);
        message(kInfoColor, text::get("GMSURVEY_SUBMITTED"));
        m_answers.fill(-1);
        m_comment.clear();
        m_want = Want::CloseSurvey;
    }
    if (r.closed) {
        m_want = Want::CloseSurvey;
    }
    if ((r.submit || r.closed) && editing) {
        text.end();
    }
}

void HelpScreens::drawSuggest(GameSession& session, DialogContext& c, ui::TextInput& text) {
    VD_PROFILE_ZONE("HelpScreens::suggest");
    game::Gameplay& play = session.play();
    const bool editing = text.active() && text.field() == kSuggestField;
    if (editing) {
        m_suggestText.assign(text.text());
    }
    // SuggestFrame's OnShow: name, gender, race, class, LEVEL level, then LOCATION_COLON and the position.
    const game::Entity* me = session.entities().find(session.playerGuid());
    const game::SocialData* social = m_socialScreens ? m_socialScreens->socialData() : nullptr;
    m_playerInfo.clear();
    if (me) {
        const std::string* known = session.playerName(me->guid);
        const std::string_view name = known ? std::string_view(*known) : std::string_view{};
        const std::string_view gender =
            text::get(me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) ? "FEMALE" : "MALE");
        const std::string_view race =
            social ? social->raceName(me->byteValue(uf::UNIT_FIELD_BYTES_0, 0)) : "";
        const std::string_view cls =
            social ? social->className(me->byteValue(uf::UNIT_FIELD_BYTES_0, 1)) : "";
        const std::string_view level = text::get("LEVEL");
        const std::string_view location = text::get("LOCATION_COLON");
        const game::ZoneState& zone = play.travel().zone();
        const std::string_view where = !zone.subzoneName.empty() ? zone.subzoneName : zone.zoneName;
        const auto n = [](std::string_view s) { return static_cast<int>(s.size()); };
        char line[256];
        std::snprintf(line, sizeof(line), "%.*s %.*s %.*s %.*s - %.*s %u\n%.*s %.*s (%.1f, %.1f, %.1f)",
                      n(name), name.data(), n(gender), gender.data(), n(race), race.data(), n(cls),
                      cls.data(), n(level), level.data(), me->value(uf::UNIT_FIELD_LEVEL), n(location),
                      location.data(), n(where), where.data(), static_cast<double>(me->position.x),
                      static_cast<double>(me->position.y), static_cast<double>(me->position.z));
        m_playerInfo.append(line);
    }
    char category[96];
    SuggestView v;
    v.playerInfo = m_playerInfo;
    if (m_bugCategory > 0) {
        std::snprintf(category, sizeof(category), "BUG_CATEGORY%d", m_bugCategory);
        v.category = text::get(category);
    } else {
        v.category = text::get("BUG_CATEGORY_CHOOSE");
    }
    v.text = m_suggestText;
    v.editing = editing;
    const SuggestResult r = suggestWindow(c, v);
    if (r.textTapped && !editing) {
        text.begin(kSuggestField, m_suggestText, 1024, ui::TextKind::Message);
    }
    if (r.categoryMenu) {
        m_categoryMenu = true;
    }
    if (r.sendBug || r.sendSuggestion) {
        if (m_bugCategory == 0) {
            message(kErrorColor, text::get("BUG_CATEGORY_ERROR")); // message(BUG_CATEGORY_ERROR)
        } else {
            // ReportBug("Category: " .. category .. "\n" .. text) / ReportSuggestion(text, category): the
            // report as SuggestFrame.xml builds it (data for the server, not interface text).
            std::string report;
            if (r.sendBug) {
                report.append("Category: ").append(v.category).append("\n");
            }
            report.append(m_suggestText);
            play.help().report(r.sendSuggestion, m_playerInfo, report);
            message(kInfoColor, text::get("SUGGEST_SUBMITTED"));
            m_suggestText.clear();
            m_want = Want::CloseSuggest;
            if (editing) {
                text.end();
            }
        }
    }
    if (r.closed) {
        m_want = Want::CloseSuggest;
        if (editing) {
            text.end();
        }
    }
    if (m_categoryMenu) {
        // SuggestFrameCategoryDropDown_Initialize: BUG_CATEGORY_CHOOSE, then BUG_CATEGORY1.. while they
        // exist.
        m_choices.clear();
        static std::array<std::array<char, 20>, 16> keys{};
        for (u32 i = 0; i < 16; ++i) {
            if (i == 0) {
                m_choices.push_back({text::get("BUG_CATEGORY_CHOOSE")});
                continue;
            }
            std::snprintf(keys[i].data(), keys[i].size(), "BUG_CATEGORY%u", i);
            if (!text::has(keys[i].data())) {
                break;
            }
            ChoiceView& ch = m_choices.emplace_back();
            ch.text = text::get(keys[i].data());
            ch.selected = static_cast<i32>(i) == m_bugCategory;
        }
        const i32 picked = choiceList(c, "suggest.categories", text::get("SUGGESTFRAME_TITLE"), m_choices);
        if (picked >= 0) {
            m_bugCategory = picked;
        }
        if (picked != -1) {
            m_categoryMenu = false;
        }
    }
}

} // namespace vd
