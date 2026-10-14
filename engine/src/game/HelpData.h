#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Phase 15 (help): the client tables of the help frame (db.vdpk): GMTicketCategory.dbc
// (GetGMTicketCategories: the categories HelpFrameGM lists, their ids the ticket's category) and the GM
// survey (GMSurveyCurrentSurvey.dbc picks the survey of the client's language, GMSurveySurveys.dbc its
// questions, GMSurveyQuestions.dbc their text: GMSurveyQuestion). Read at start-up; lookups do not allocate.
class HelpData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    struct Category {
        u32 id = 0;
        std::string_view name;
    };
    // Every category in the table's order (at most 16).
    std::span<const Category> categories() const { return {m_categories.data(), m_categoryCount}; }
    std::string_view categoryName(u32 id) const;

    // The survey of the client's language (GMSurveyCurrentSurvey's row 0, enUS) and its questions
    // (GMSurveyQuestion 1..10), empty texts ending the list.
    u32 survey() const { return m_survey; }
    struct Question {
        u32 id = 0;
        std::string_view text;
    };
    std::span<const Question> questions() const { return {m_questions.data(), m_questionCount}; }

private:
    db::DbcView m_categoryTable, m_current, m_surveys, m_questionTable;
    std::array<Category, 16> m_categories{};
    u32 m_categoryCount = 0;
    u32 m_survey = 0;
    std::array<Question, 10> m_questions{};
    u32 m_questionCount = 0;
    bool m_ready = false;
};

} // namespace vd::game
