#include "game/HelpData.h"

#include "core/Locale.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <string>

namespace vd::game {

namespace {

// 1.12.1 layouts: GMTicketCategory and GMSurveyQuestions are an id, a name in eight locales and the locales'
// flags; GMSurveyCurrentSurvey a language and its survey; GMSurveySurveys a survey and ten question ids.
constexpr u32 kNamedFields = 10, kCurrentFields = 2, kSurveyFields = 11;

bool open(const pack::PackReader& db, const char* name, u32 fields, db::DbcView& out) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    Result<db::DbcView> view =
        asset ? db::DbcView::parse(asset->data, asset->size) : Result<db::DbcView>(Error{"missing"});
    if (!view || view.value().fieldCount() != fields) {
        VD_LOG_WARN("%s.dbc: %s", name, view ? "unexpected layout" : view.error().message.c_str());
        return false;
    }
    out = view.value();
    return true;
}

} // namespace

bool HelpData::init(const pack::PackReader& db) {
    m_ready = open(db, "GMTicketCategory", kNamedFields, m_categoryTable);
    m_categoryCount = 0;
    for (u32 r = 0; m_ready && r < m_categoryTable.recordCount() && m_categoryCount < m_categories.size();
         ++r) {
        m_categories[m_categoryCount++] = {m_categoryTable.getU32(r, 0), m_categoryTable.getString(r, 1)};
    }
    // The survey: optional (a client without it never shows one).
    m_survey = 0;
    m_questionCount = 0;
    if (open(db, "GMSurveyCurrentSurvey", kCurrentFields, m_current) &&
        open(db, "GMSurveySurveys", kSurveyFields, m_surveys) &&
        open(db, "GMSurveyQuestions", kNamedFields, m_questionTable)) {
        // The row of the client's language (phase 21: the db's, esES = 6 under its overlay), else enUS's (0).
        const int locale = localeIndex(db.dataLocale());
        const u32 wanted = locale > 0 ? static_cast<u32>(locale) : 0u;
        for (u32 r = 0; r < m_current.recordCount(); ++r) {
            const u32 row = m_current.getU32(r, 0);
            if (row == wanted || (row == 0 && m_survey == 0)) {
                m_survey = m_current.getU32(r, 1);
            }
        }
        const u32 s = m_survey != 0 ? m_surveys.findById(m_survey) : m_surveys.recordCount();
        for (u32 i = 0; s < m_surveys.recordCount() && i < m_questions.size(); ++i) {
            const u32 id = m_surveys.getU32(s, 1 + i);
            const u32 q = id != 0 ? m_questionTable.findById(id) : m_questionTable.recordCount();
            if (q >= m_questionTable.recordCount()) {
                break;
            }
            m_questions[m_questionCount++] = {id, m_questionTable.getString(q, 1)};
        }
    }
    return m_ready;
}

std::string_view HelpData::categoryName(u32 id) const {
    for (u32 i = 0; i < m_categoryCount; ++i) {
        if (m_categories[i].id == id) {
            return m_categories[i].name;
        }
    }
    return {};
}

} // namespace vd::game
