#include "app/HelpWindows.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using namespace kit;

constexpr u64 kBullet = assetPathHash("Interface\\QuestFrame\\UI-Quest-BulletPoint.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\HelpFrame\\HelpFrameButton-Highlight.blp");
constexpr u64 kTicketIcon = assetPathHash("Interface\\HelpFrame\\OpenTicketIcon.blp");
constexpr u64 kRadio = assetPathHash("Interface\\Buttons\\UI-RadioButton.blp");
constexpr u64 kDivider = assetPathHash("Interface\\QuestFrame\\UI-HorizontalBreak.blp");
constexpr u64 kDropDownArrow = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");

constexpr u32 kWhiteText = ui::kWhite;

// GENERAL_HELPFRAME (HelpFrame.lua): the general page of each category (2, harassment, has its own frame).
struct GeneralPage {
    const char* title;
    const char* titleText;
    const char* bulletTitle1;
    std::array<const char*, 7> bullets1;
    const char* bulletTitle2;
    std::array<const char*, 3> bullets2;
    const char* endText;
    const char* button;
    const char* button2;
};
constexpr GeneralPage kStuck = {
    "STUCK_OPTION",      "HELPFRAME_STUCK_TEXT1", nullptr, {}, nullptr, {}, nullptr,
    "STUCK_BUTTON_TEXT", "STUCK_BUTTON2_TEXT"};
constexpr GeneralPage kGuild = {
    "HELPFRAME_GUILD_TITLE",
    "HELPFRAME_GUILD_TEXT",
    "HELPFRAME_GUILD_BULLET_TITLE1",
    {"HELPFRAME_GUILD_BULLET1", "HELPFRAME_GUILD_BULLET2", "HELPFRAME_GUILD_BULLET3"},
    nullptr,
    {},
    nullptr,
    "HELPFRAME_GUILD_BUTTON_TEXT",
    nullptr};
constexpr GeneralPage kItem = {"HELPFRAME_ITEM_TITLE",
                               "HELPFRAME_ITEM_TEXT",
                               "HELPFRAME_ITEM_BULLET_TITLE1",
                               {"HELPFRAME_ITEM_BULLET1", "HELPFRAME_ITEM_BULLET2", "HELPFRAME_ITEM_BULLET3",
                                "HELPFRAME_ITEM_BULLET4", "HELPFRAME_ITEM_BULLET5"},
                               "HELPFRAME_ITEM_BULLET_TITLE2",
                               {"HELPFRAME_ITEM_BULLET6", "HELPFRAME_ITEM_BULLET7"},
                               nullptr,
                               "HELPFRAME_ITEM_BUTTON_TEXT",
                               nullptr};
constexpr GeneralPage kEnvironmental = {
    "HELPFRAME_ENVIRONMENTAL_TITLE",
    "HELPFRAME_ENVIRONMENTAL_TEXT",
    "HELPFRAME_ENVIRONMENTAL_BULLET_TITLE1",
    {"HELPFRAME_ENVIRONMENTAL_BULLET1", "HELPFRAME_ENVIRONMENTAL_BULLET2", "HELPFRAME_ENVIRONMENTAL_BULLET3",
     "HELPFRAME_ENVIRONMENTAL_BULLET4"},
    "HELPFRAME_ENVIRONMENTAL_BULLET_TITLE2",
    {"HELPFRAME_ENVIRONMENTAL_BULLET5", "HELPFRAME_ENVIRONMENTAL_BULLET6"},
    nullptr,
    "HELPFRAME_ENVIRONMENTAL_BUTTON_TEXT",
    nullptr};
constexpr GeneralPage kNonQuest = {
    "HELPFRAME_NONQUEST_TITLE",
    "HELPFRAME_NONQUEST_TEXT",
    "HELPFRAME_NONQUEST_BULLET_TITLE1",
    {"HELPFRAME_NONQUEST_BULLET1", "HELPFRAME_NONQUEST_BULLET2", "HELPFRAME_NONQUEST_BULLET3",
     "HELPFRAME_NONQUEST_BULLET4"},
    "HELPFRAME_NONQUEST_BULLET_TITLE2",
    {"HELPFRAME_NONQUEST_BULLET5", "HELPFRAME_NONQUEST_BULLET6", "HELPFRAME_NONQUEST_BULLET7"},
    nullptr,
    "HELPFRAME_NONQUEST_BUTTON_TEXT",
    nullptr};
constexpr GeneralPage kQuest = {
    "HELPFRAME_QUEST_TITLE",
    "HELPFRAME_QUEST_TEXT",
    "HELPFRAME_QUEST_BULLET_TITLE1",
    {"HELPFRAME_QUEST_BULLET1", "HELPFRAME_QUEST_BULLET2", "HELPFRAME_QUEST_BULLET3"},
    "HELPFRAME_QUEST_BULLET_TITLE2",
    {"HELPFRAME_QUEST_BULLET4", "HELPFRAME_QUEST_BULLET5"},
    nullptr,
    "HELPFRAME_QUEST_BUTTON_TEXT",
    nullptr};
constexpr GeneralPage kTechnical = {"HELPFRAME_TECHNICAL_TITLE",
                                    "HELPFRAME_TECHNICAL_TEXT",
                                    "HELPFRAME_TECHNICAL_BULLET_TITLE1",
                                    {"HELPFRAME_TECHNICAL_BULLET1", "HELPFRAME_TECHNICAL_BULLET2",
                                     "HELPFRAME_TECHNICAL_BULLET3", "HELPFRAME_TECHNICAL_BULLET4",
                                     "HELPFRAME_TECHNICAL_BULLET5", "HELPFRAME_TECHNICAL_BULLET6",
                                     "HELPFRAME_TECHNICAL_BULLET7"},
                                    nullptr,
                                    {},
                                    "HELPFRAME_TECHNICAL_BULLET_TITLE2",
                                    nullptr,
                                    nullptr};
constexpr GeneralPage kAccount = {
    "HELPFRAME_ACCOUNT_TITLE",
    "HELPFRAME_ACCOUNT_TEXT",
    "HELPFRAME_ACCOUNT_BULLET_TITLE1",
    {"HELPFRAME_ACCOUNT_BULLET1", "HELPFRAME_ACCOUNT_BULLET2", "HELPFRAME_ACCOUNT_BULLET3"},
    nullptr,
    {},
    "HELPFRAME_ACCOUNT_ENDTEXT",
    "HELPFRAME_ACCOUNT_BUTTON_TEXT",
    nullptr};
constexpr GeneralPage kCharacter = {"HELPFRAME_CHARACTER_TITLE",
                                    "HELPFRAME_CHARACTER_TEXT",
                                    "HELPFRAME_CHARACTER_BULLET_TITLE1",
                                    {"HELPFRAME_CHARACTER_BULLET1", "HELPFRAME_CHARACTER_BULLET2",
                                     "HELPFRAME_CHARACTER_BULLET3", "HELPFRAME_CHARACTER_BULLET4",
                                     "HELPFRAME_CHARACTER_BULLET5"},
                                    nullptr,
                                    {},
                                    nullptr,
                                    "HELPFRAME_CHARACTER_BUTTON_TEXT",
                                    nullptr};

const GeneralPage* generalPage(u32 category) {
    switch (category) {
    case 1: return &kStuck;
    case 3: return &kGuild;
    case 4: return &kItem;
    case 5: return &kEnvironmental;
    case 6: return &kNonQuest;
    case 7: return &kQuest;
    case 8: return &kTechnical;
    case 9: return &kAccount;
    case 10: return &kCharacter;
    default: return nullptr;
    }
}

u32 keyCount(std::span<const char* const> keys) {
    u32 n = 0;
    while (n < keys.size() && keys[n]) {
        ++n;
    }
    return n;
}

// A column of HelpFrame's texts: GameFontNormalLarge titles, GameFontHighlight text, GameFontNormal headers
// and HelpFrameBulletPointTemplate lines (UI-Quest-BulletPoint and GameFontHighlightSmall).
struct HelpFlow {
    Gui& g;
    const Metrics& m;
    f32 x, w, y;

    void title(const char* key) {
        if (key) {
            y += g.draw.wrappedText({x, y, w, 1e6f}, text::get(key), ui::gameFont(m.heading)) + g.px(8.0f);
        }
    }
    void text(const char* key, bool small = false, u32 color = kWhiteText, f32 after = 10.0f) {
        if (key) {
            y += g.draw.wrappedText({x, y, w, 1e6f}, text::get(key),
                                    ui::gameFont(small ? m.small : m.text, color)) +
                 g.px(after);
        }
    }
    void header(const char* key) {
        if (key) {
            y += g.draw.wrappedText({x, y, w, 1e6f}, text::get(key), ui::gameFont(m.text)) + g.px(4.0f);
        }
    }
    void bullets(std::span<const char* const> keys) {
        for (const char* key : keys) {
            const f32 b = m.small;
            g.draw.sprite({x + g.px(4.0f), y + g.px(1.0f), b, b}, kBullet);
            y += g.draw.wrappedText({x + b + g.px(10.0f), y, w - b - g.px(10.0f), 1e6f}, text::get(key),
                                    ui::gameFont(m.small, kWhiteText)) +
                 g.px(3.0f);
        }
        y += g.px(8.0f);
    }
    bool button(const char* key, u64 id) {
        if (!key) {
            return false;
        }
        const std::string_view label = text::get(key);
        const f32 bw = std::min(buttonWidth(g, label, m.button) + g.px(40.0f), w);
        const bool tapped =
            ui::button(g, widgetId("help.button", id), {x + (w - bw) * 0.5f, y, bw, m.button}, label);
        y += m.button + g.px(14.0f);
        return tapped;
    }
};

// HelpFrameHomeButtonTemplate / GMCategoryButtonTemplate: the bullet, the name (GameFontNormal), the
// description (GameFontHighlightSmall) under it, HelpFrameButton-Highlight while pressed.
bool categoryButton(Gui& g, const Metrics& m, u64 id, f32 x, f32 y, f32 w, std::string_view name,
                    std::string_view description, f32& height) {
    height = std::max(m.row, m.text * 1.4f + (description.empty() ? 0.0f : m.small * 1.4f) + g.px(10.0f));
    const Rect r{x, y, w, height};
    const ui::WidgetState state = ui::hit(g, widgetId("help.category", id), r);
    if (ui::held(state, r)) {
        g.draw.spriteAdditive(r, kButtonHighlight, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.578125f});
    }
    const f32 b = m.text;
    g.draw.sprite({x, y + g.px(4.0f), b, b}, kBullet);
    ui::label(g, {x + b + g.px(8.0f), y + g.px(2.0f), w - b - g.px(8.0f), m.text * 1.4f}, name,
              ui::gameFont(m.text));
    if (!description.empty()) {
        ui::label(g, {x + b + g.px(8.0f), y + g.px(2.0f) + m.text * 1.4f, w - b - g.px(8.0f), m.small * 1.4f},
                  description, ui::gameFont(m.small, kWhiteText));
    }
    return ui::tapped(state);
}

} // namespace

HelpGeneralView helpGeneralPage(u32 category) {
    HelpGeneralView v;
    if (const GeneralPage* p = generalPage(category)) {
        v.title = p->title;
        v.titleText = p->titleText;
        v.bulletTitle1 = p->bulletTitle1;
        v.bullets1 = {p->bullets1.data(), keyCount(p->bullets1)};
        v.bulletTitle2 = p->bulletTitle2;
        v.bullets2 = {p->bullets2.data(), keyCount(p->bullets2)};
        v.endText = p->endText;
        v.button = p->button;
        v.button2 = p->button2;
    }
    return v;
}

HelpResult helpWindow(DialogContext& c, const HelpView& v) {
    VD_PROFILE_ZONE("HelpWindows::help");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    HelpResult result;
    const Dialog d = beginDialog(c, g, "help", wide ? 760.0f : 600.0f, wide ? 460.0f : 760.0f,
                                 text::get("HELP_FRAME_TITLE"), true, true);
    result.closed = d.closed;
    const ui::ScrollView view =
        ui::beginScroll(g, widgetId("help.scroll", static_cast<u64>(v.page)), d.body.inset(g.px(4.0f)));
    HelpFlow f{g, m, view.viewport.x + g.px(4.0f), view.viewport.w - g.px(8.0f), view.top + g.px(4.0f)};
    switch (v.page) {
    case HelpPage::Home:
        // HelpFrameHome: what GMs do not help with, then the button to the categories.
        f.title("PETITION_GAME_MASTER");
        f.text("HELPFRAME_HOME_TEXT1");
        f.header("HELPFRAME_HOME_ISSUE1_HEADER");
        f.text("HELPFRAME_HOME_ISSUE1", true);
        f.header("HELPFRAME_HOME_ISSUE2_HEADER");
        f.text("HELPFRAME_HOME_ISSUE2", true);
        f.header("HELPFRAME_HOME_ISSUE3_HEADER");
        f.text("HELPFRAME_HOME_ISSUE3", true, kWhiteText, 4.0f);
        f.text("PVP_POLICY_URL", false, ui::kGold);
        f.text("HELPFRAME_HOME_TEXT2");
        result.toCategories = f.button("HELPFRAME_HOME_BUTTON_TEXT", 1);
        break;
    case HelpPage::Categories:
        // HelpFrameGM: HELPFRAME_HOME_TEXT and the categories (GetGMTicketCategories).
        f.text("HELPFRAME_HOME_TEXT", true);
        for (u32 i = 0; i < v.categories.size(); ++i) {
            f32 h = 0.0f;
            if (categoryButton(g, m, i, f.x, f.y, f.w, v.categories[i].name, v.categories[i].description,
                               h)) {
                result.category = static_cast<i32>(i);
            }
            f.y += h + g.px(4.0f);
        }
        break;
    case HelpPage::General:
        // HelpFrameGeneral (HelpFrame_SetupGeneralFrame).
        f.title(v.general.title);
        f.text(v.general.titleText);
        f.header(v.general.bulletTitle1);
        f.bullets(v.general.bullets1);
        f.header(v.general.bulletTitle2);
        f.bullets(v.general.bullets2);
        f.text(v.general.endText, true);
        result.generalButton = f.button(v.general.button, 2);
        result.generalButton2 = f.button(v.general.button2, 3);
        break;
    case HelpPage::Harassment: {
        // HelpFrameHarassment: the verbal and the physical sections, each with its tips and its button.
        static constexpr const char* kVerbalTips[] = {"VERBAL_HARASSMENT_TEXT2", "VERBAL_HARASSMENT_TEXT3",
                                                      "VERBAL_HARASSMENT_TEXT4"};
        static constexpr const char* kPhysicalTips[] = {
            "PHYSICAL_HARASSMENT_TEXT2", "PHYSICAL_HARASSMENT_TEXT3", "PHYSICAL_HARASSMENT_TEXT4",
            "PHYSICAL_HARASSMENT_TEXT5", "PHYSICAL_HARASSMENT_TEXT6"};
        f.text("HARASSMENT_TEXT");
        f.header("VERBAL_HARASSMENT");
        f.text("VERBAL_HARASSMENT_DESCRIPTION", true);
        f.text("VERBAL_HARASSMENT_TEXT1", true, ui::kGold, 4.0f);
        f.bullets(kVerbalTips);
        result.reportVerbal = f.button("REPORT_VERBAL_HARASSMENT", 4);
        g.draw.sprite({f.x, f.y - g.px(10.0f), f.w, g.px(8.0f)}, kDivider);
        f.header("PHYSICAL_HARASSMENT");
        f.text("PHYSICAL_HARASSMENT_DESCRIPTION", true);
        f.text("PHYSICAL_HARASSMENT_TEXT1", true, ui::kGold, 4.0f);
        f.bullets(kPhysicalTips);
        result.reportPhysical = f.button("REPORT_PHYSICAL_HARASSMENT", 5);
        break;
    }
    case HelpPage::OpenTicket: {
        // HelpFrameOpenTicket: HELPFRAME_OPENTICKET_TEXT (or _EDITTEXT for the open ticket), the divider and
        // the text box (500 letters).
        f.text(v.hasTicket ? "HELPFRAME_OPENTICKET_EDITTEXT" : "HELPFRAME_OPENTICKET_TEXT", false, kWhiteText,
               4.0f);
        g.draw.sprite({f.x, f.y, f.w, g.px(8.0f)}, kDivider);
        f.y += g.px(12.0f);
        const f32 boxH = std::max(view.viewport.bottom() - f.y - g.px(6.0f), m.text * 6.0f);
        const Rect box{f.x, f.y, f.w, boxH};
        ui::tooltipPanel(g, box, rgba(0, 0, 0, 140));
        const Rect in = box.inset(g.px(8.0f));
        const TextStyle s = ui::chatFont(m.text, kWhiteText);
        const f32 h = g.draw.wrappedText({in.x, in.y, in.w, 1e6f}, v.ticketText, s);
        if (v.editing) {
            g.draw.solid({in.x, in.y + std::max(h - s.size, 0.0f), g.px(2.0f), s.size}, ui::kWhite);
        }
        result.ticketTapped = ui::tapped(ui::hit(g, widgetId("help.ticket"), box));
        f.y += boxH;
        break;
    }
    }
    ui::endScroll(g, view, f.y - view.top + g.px(8.0f));
    // The bottom: Back (but at home) and Cancel; the ticket page's Submit / Save Changes and Cancel / Exit.
    bool left = false, right = false;
    switch (v.page) {
    case HelpPage::Home:
        footerButtons(g, d.footer, "help", {}, false, left, text::get("CANCEL"), right);
        break;
    case HelpPage::OpenTicket:
        footerButtons(g, d.footer, "help", text::get(v.hasTicket ? "EDIT_TICKET" : "SUBMIT"), true, left,
                      text::get(v.hasTicket ? "EXIT" : "CANCEL"), right);
        result.submit = left;
        left = false;
        break;
    default:
        footerButtons(g, d.footer, "help", text::get("BACK"), true, left, text::get("CANCEL"), right);
        break;
    }
    result.back = left;
    result.closed = result.closed || right;
    return result;
}

// --- TicketStatusFrame
// -----------------------------------------------------------------------------------------

bool ticketStatusFrame(DialogContext& c, const Rect& area, const TicketStatusView& v,
                       TicketStatusState& state, u64 nowMs) {
    VD_PROFILE_ZONE("HelpWindows::ticketStatus");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    // 208 units wide, the title and the time (168 wide) 10 in, TicketStatusButton (32 x 32, OpenTicketIcon)
    // at the top right; its height follows its text (TicketStatusFrame:SetHeight).
    const f32 w = std::min(g.unit(208.0f) * 1.25f, area.w);
    const f32 textW = w - g.unit(42.0f);
    const TextStyle title = ui::gameFont(m.small);
    const TextStyle time = ui::gameFont(m.small, kWhiteText);
    const f32 titleH = g.draw.measureWrapped(textW, v.title, title.font, title.size);
    const f32 timeH =
        v.time.empty() ? 0.0f : g.draw.measureWrapped(textW, v.time, time.font, time.size) + g.px(2.0f);
    const f32 h = std::max(titleH + timeH + g.px(20.0f), g.unit(40.0f));
    // Where the finger moved it, kept on the screen.
    const Rect place{area.right() - w, area.y, w, h};
    const auto clamp = [&](f32& dx, f32& dy) {
        dx = std::clamp(dx, c.safeArea.x - place.x, c.safeArea.right() - place.right());
        dy = std::clamp(dy, c.safeArea.y - place.y, c.safeArea.bottom() - place.bottom());
    };
    clamp(state.dx, state.dy);
    const Rect r{place.x + state.dx, place.y + state.dy, w, h};
    const ui::WidgetState input = ui::hit(g, widgetId("ticketStatus"), r);
    if (input.touch && input.touch->began) {
        state.grabDx = state.dx;
        state.grabDy = state.dy;
    }
    if (input.pressed && input.touch && input.touch->moved) { // StartMoving
        state.dx = state.grabDx + input.touch->x - input.touch->startX;
        state.dy = state.grabDy + input.touch->y - input.touch->startY;
        clamp(state.dx, state.dy);
    }
    ui::tooltipPanel(g, r);
    f32 y = r.y + g.px(8.0f);
    y += g.draw.wrappedText({r.x + g.px(10.0f), y, textW, titleH + 1.0f}, v.title, title) + g.px(2.0f);
    if (!v.time.empty()) {
        g.draw.wrappedText({r.x + g.px(10.0f), y, textW, timeH + 1.0f}, v.time, time);
    }
    const f32 icon = g.unit(32.0f);
    // UIFrameFlash(TicketStatusButton, 0.75, 0.75, 20): fading in and out every 1.5 s.
    f32 alpha = 1.0f;
    if (v.flash) {
        const f32 phase = static_cast<f32>(nowMs % 1500) / 750.0f;
        alpha = phase < 1.0f ? phase : 2.0f - phase;
    }
    g.draw.sprite({r.right() - icon - g.px(3.0f), r.y, icon, icon}, kTicketIcon,
                  ui::withAlpha(ui::kWhite, alpha));
    return ui::tapped(input);
}

// --- GMSurveyFrame
// ---------------------------------------------------------------------------------------------

GmSurveyResult gmSurveyWindow(DialogContext& c, const GmSurveyView& v) {
    VD_PROFILE_ZONE("HelpWindows::survey");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    GmSurveyResult result;
    const Dialog d = beginDialog(c, g, "gmSurvey", wide ? 760.0f : 600.0f, wide ? 460.0f : 760.0f,
                                 text::get("GMSURVEY_TITLE"), true, true);
    result.closed = d.closed;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("gmSurvey.scroll"), d.body.inset(g.px(4.0f)));
    const f32 x = view.viewport.x + g.px(4.0f);
    const f32 w = view.viewport.w - g.px(8.0f);
    f32 y = view.top;
    y += g.draw.wrappedText({x, y, w, 1e6f}, text::get("GMSURVEY_REQUEST_TEXT"),
                            ui::gameFont(m.text, kWhiteText)) +
         g.px(8.0f);
    // GMSurveyQuestionTemplate: the question (GameFontNormal) in a tooltip backdrop, the six radio buttons
    // under it (N/A, then 1..5: GMSURVEYRATING1..5), each with its score under it.
    static constexpr const char* kScores[] = {"GM_SURVEY_NOT_APPLICABLE", "GMSURVEYRATING1",
                                              "GMSURVEYRATING2",          "GMSURVEYRATING3",
                                              "GMSURVEYRATING4",          "GMSURVEYRATING5"};
    const f32 radio = std::max(g.px(28.0f), m.text * 1.4f);
    for (u32 q = 0; q < v.questions.size(); ++q) {
        const SurveyQuestionView& question = v.questions[q];
        const f32 textH = g.draw.measureWrapped(w - g.px(16.0f), question.text, ui::FontId::Friz, m.text);
        const f32 h = textH + radio + m.small * 1.4f + g.px(24.0f);
        const Rect box{x, y, w, h};
        ui::tooltipPanel(g, box, rgba(0, 0, 0, 120));
        g.draw.wrappedText({x + g.px(8.0f), y + g.px(8.0f), w - g.px(16.0f), textH + 1.0f}, question.text,
                           ui::gameFont(m.text));
        const f32 cellW = (w - g.px(16.0f)) / 6.0f;
        const f32 ry = y + g.px(10.0f) + textH;
        for (u32 a = 0; a < 6; ++a) {
            const Rect cell{x + g.px(8.0f) + static_cast<f32>(a) * cellW, ry, cellW, radio + m.small * 1.4f};
            const ui::WidgetState state = ui::hit(g, widgetId("gmSurvey.radio", q * 8 + a), cell);
            const bool checked = question.selected == static_cast<i32>(a);
            // UIRadioButtonTemplate: UI-RadioButton's unchecked (0..0.25) / checked (0.25..0.5) quarters.
            const Rect art{cell.centerX() - radio * 0.5f, ry, radio, radio};
            g.draw.sprite(art, kRadio, ui::kWhite,
                          {checked ? 0.25f : 0.0f, 0.0f, checked ? 0.5f : 0.25f, 1.0f});
            TextStyle s = ui::gameFont(m.small, kWhiteText);
            s.align = Align::Center;
            ui::label(g, {cell.x, ry + radio, cell.w, m.small * 1.4f}, text::get(kScores[a]), s);
            if (ui::tapped(state) && !checked) {
                result.question = static_cast<i32>(q);
                result.answer = static_cast<i32>(a);
            }
        }
        y += h + g.px(6.0f);
    }
    // ADDITIONAL_COMMENTS and the comment box (500 letters).
    y += g.draw.wrappedText({x, y, w, 1e6f}, text::get("ADDITIONAL_COMMENTS"), ui::gameFont(m.text)) +
         g.px(4.0f);
    const Rect box{x, y, w, m.text * 5.0f};
    ui::tooltipPanel(g, box, rgba(0, 0, 0, 140));
    const Rect in = box.inset(g.px(8.0f));
    const f32 h = g.draw.wrappedText({in.x, in.y, in.w, 1e6f}, v.comment, ui::chatFont(m.text, kWhiteText));
    if (v.editing) {
        g.draw.solid({in.x, in.y + std::max(h - m.text, 0.0f), g.px(2.0f), m.text}, ui::kWhite);
    }
    result.commentTapped = ui::tapped(ui::hit(g, widgetId("gmSurvey.comment"), box));
    y += box.h + g.px(8.0f);
    ui::endScroll(g, view, y - view.top);
    bool submit = false, cancel = false;
    footerButtons(g, d.footer, "gmSurvey", text::get("SUBMIT"), true, submit, text::get("CANCEL"), cancel);
    result.submit = submit;
    result.closed = result.closed || cancel;
    return result;
}

// --- SuggestFrame
// ----------------------------------------------------------------------------------------------

SuggestResult suggestWindow(DialogContext& c, const SuggestView& v) {
    VD_PROFILE_ZONE("HelpWindows::suggest");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    SuggestResult result;
    const Dialog d = beginDialog(c, g, "suggest", wide ? 640.0f : 520.0f, wide ? 440.0f : 640.0f,
                                 text::get("SUGGESTFRAME_TITLE"), true, true);
    result.closed = d.closed;
    const Rect& b = d.body;
    f32 y = b.y;
    // SuggestFramePlayerInfo (GameFontHighlightSmall), the category drop-down (BUG_CATEGORY*), the text box
    // with SUGGEST_TOOLTIP_TEXT as its hint.
    const TextStyle info = ui::gameFont(m.small, kWhiteText);
    y += g.draw.wrappedText({b.x, y, b.w, m.small * 3.0f}, v.playerInfo, info) + g.px(8.0f);
    const f32 fieldH = std::max(g.px(36.0f), m.text * 1.9f);
    const Rect drop{b.x, y, b.w, fieldH};
    ui::tooltipPanel(g, drop, rgba(0, 0, 0, 160));
    ui::label(g, {drop.x + g.px(8.0f), drop.y, drop.w - fieldH - g.px(8.0f), drop.h}, v.category,
              ui::gameFont(m.text, kWhiteText));
    g.draw.sprite({drop.right() - fieldH, drop.y, fieldH, fieldH}, kDropDownArrow);
    result.categoryMenu = ui::tapped(ui::hit(g, widgetId("suggest.category"), drop));
    y += fieldH + g.px(8.0f);
    const Rect box{b.x, y, b.w, b.bottom() - y};
    ui::tooltipPanel(g, box, rgba(0, 0, 0, 140));
    const Rect in = box.inset(g.px(8.0f));
    if (v.text.empty() && !v.editing) {
        g.draw.wrappedText({in.x, in.y, in.w, in.h}, text::get("SUGGEST_TOOLTIP_TEXT"),
                           ui::gameFont(m.small, ui::kDisabledText));
    } else {
        const f32 h = g.draw.wrappedText({in.x, in.y, in.w, 1e6f}, v.text, ui::chatFont(m.text, kWhiteText));
        if (v.editing) {
            g.draw.solid({in.x, in.y + std::max(h - m.text, 0.0f), g.px(2.0f), m.text}, ui::kWhite);
        }
    }
    result.textTapped = ui::tapped(ui::hit(g, widgetId("suggest.text"), box));
    // SuggestFrameSendBugButton (SEND_BUG) and SuggestFrameSendSuggestButton (SEND_SUGGEST).
    bool bug = false, suggest = false;
    footerButtons(g, d.footer, "suggest", text::get("SEND_BUG"), v.bugEnabled, bug, text::get("SEND_SUGGEST"),
                  suggest);
    result.sendBug = bug;
    result.sendSuggestion = suggest && v.suggestEnabled;
    return result;
}

} // namespace vd
