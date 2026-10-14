#include "app/CharCreateGallery.h"

#include "app/CharCreateScreens.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/CharCreate.h"
#include "game/SocialText.h"
#include "net/ResponseCodes.gen.h"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace vd {
namespace {

// CharBaseInfo of 1.12 (without the dwarf mage), in GetClassesForRace's order: the sample's classes.
u32 sampleClasses(u8 race, std::array<u8, 8>& out) {
    static constexpr u8 kTable[9][7] = {{0},
                                        {1, 2, 4, 5, 8, 9},
                                        {1, 3, 4, 7, 9},
                                        {1, 2, 3, 4, 5},
                                        {1, 3, 4, 5, 11},
                                        {1, 4, 5, 8, 9},
                                        {1, 3, 7, 11},
                                        {1, 4, 8, 9},
                                        {1, 3, 4, 5, 7, 8}};
    u32 n = 0;
    for (const u8 cls : kTable[race < 9 ? race : 0]) {
        if (cls != 0) {
            out[n++] = cls;
        }
    }
    return n;
}

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

} // namespace

bool CharCreateGallery::select(const char* name) {
    static constexpr const char* kNames[] = {"create", "create2", "create3", "create4"};
    for (u32 i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(name, kNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

void CharCreateGallery::fill() {
    CharCreateView& v = m_view;
    v.races = game::CharCreateData::kRaces;
    v.classes = {m_classes.data(), sampleClasses(v.race, m_classes)};
    describeCharCreate(v, m_abilities);
    // The rows' labels as GetHairCustomization / GetFacialHairCustomization name them for the sample's races.
    const bool tauren = v.race == 6;
    const game::FeatureKind facial = tauren && v.gender == 1 ? game::FeatureKind::Hair
                                     : v.race == 8           ? game::FeatureKind::Tusks
                                     : v.gender == 1         ? game::FeatureKind::Piercings
                                                             : game::FeatureKind::FacialHair;
    for (u32 i = 0; i < v.rows.size(); ++i) {
        v.rows[i].label = text::get(
            game::customizationKey(static_cast<game::AppearanceOption>(i),
                                   tauren ? game::FeatureKind::Horns : game::FeatureKind::HairStyle, facial));
        v.rows[i].shown = true;
    }
}

void CharCreateGallery::draw(DialogContext& c, ui::TextInput& text) {
    CharCreateView& v = m_view;
    if (!m_filled) {
        m_filled = true;
        v.race = m_page == Page::Tauren ? 6 : 1;
        v.gender = m_page == Page::Tauren ? 1 : 0;
        v.cls = m_page == Page::Tauren ? 11 : 1;
        m_name.set(m_page == Page::Tauren ? "Mulgore" : "Arthel");
        if (m_page == Page::Creating) {
            v.dialog = CharCreateDialog::Creating;
            v.dialogText = text::get("CHAR_CREATE_IN_PROGRESS");
        } else if (m_page == Page::Error) {
            v.dialog = CharCreateDialog::Message;
            v.dialogText = game::charResultText(static_cast<u8>(net::ResponseCode::CHAR_CREATE_NAME_IN_USE));
        }
    }
    fill();
    const CharCreateResult r = characterCreateScreen(c, text, m_name, v, m_ui);
    char line[64];
    if (r.race > 0 && r.race != v.race) {
        v.race = static_cast<u8>(r.race);
        v.cls = 1; // SetSelectedRace: the race's first class
        logResult("create race");
    }
    if (r.gender >= 0) {
        v.gender = static_cast<u8>(r.gender);
        logResult("create gender");
    }
    if (r.cls > 0) {
        v.cls = static_cast<u8>(r.cls);
        logResult("create class");
    }
    if (r.row >= 0) {
        std::snprintf(line, sizeof(line), "create row %d step %d", r.row, r.step);
        logResult(line);
    }
    if (r.randomize) {
        logResult("create randomize");
    }
    if (r.randomName) {
        m_name.set("Thalorin");
        logResult("create random name");
    }
    if (r.accept) {
        logResult("create accept");
    }
    if (r.back) {
        logResult("create back");
    }
    if (r.dialogButton) {
        v.dialog = CharCreateDialog::None;
        logResult("create dialog button");
    }
    if (r.turnDegrees != 0.0f) {
        VD_LOG_DEBUG("UI gallery: create turn %.1f", r.turnDegrees);
    }
}

} // namespace vd
