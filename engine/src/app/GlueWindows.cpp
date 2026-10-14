#include "app/GlueWindows.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;
using ui::WidgetState;
using Point = GlueLayout::Point;

// GlueButtons.xml: GluePanelButton*Texture (texels 0..148 x 0..48 of 256 x 64; the highlight 0..160 x 0..44,
// ADD).
constexpr u64 kButtonUp = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Up.blp");
constexpr u64 kButtonDown = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Down.blp");
constexpr u64 kButtonDisabled = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Disabled.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Highlight.blp");
constexpr ui::TexCoords kButtonUv{0.0f, 0.0f, 0.578125f, 0.75f};
constexpr ui::TexCoords kButtonHighlightUv{0.0f, 0.0f, 0.625f, 0.6875f};
// AccountLogin.xml.
constexpr u64 kWowLogo = assetPathHash("Interface\\Glues\\Common\\Glues-WoW-Logo.blp");
constexpr u64 kBlizzardLogo = assetPathHash("Interface\\Glues\\MainMenu\\Glues-BlizzardLogo.blp");
constexpr u64 kEsrbRating =
    assetPathHash("Interface\\Glues\\Login\\Glues-ESRBRating.blp"); // LocalizeFrames (enUS)
constexpr u64 kTooltipBackground = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Background.blp");
constexpr u64 kGlueTooltipBackground = assetPathHash("Interface\\Glues\\Common\\Glue-Tooltip-Background.blp");
constexpr u64 kGlueTooltipBorder = assetPathHash("Interface\\Glues\\Common\\Glue-Tooltip-Border.blp");
constexpr u64 kCheckUp = assetPathHash("Interface\\Buttons\\UI-CheckBox-Up.blp");
constexpr u64 kCheckDown = assetPathHash("Interface\\Buttons\\UI-CheckBox-Down.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\UI-CheckBox-Highlight.blp");
constexpr u64 kCheckMark = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
// GlueDialog.xml, CharacterDeleteDialog, CharacterRenameDialog.
constexpr u64 kDialogBackground = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Background.blp");
constexpr u64 kDialogBorder = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Border.blp");
constexpr u64 kDialogHeader = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Header.blp");
constexpr u64 kAlertIcon = assetPathHash("Interface\\DialogFrame\\DialogAlertIcon.blp");
constexpr u64 kInputLeft = assetPathHash("Interface\\ChatFrame\\UI-ChatInputBorder-Left.blp");
constexpr u64 kInputRight = assetPathHash("Interface\\ChatFrame\\UI-ChatInputBorder-Right.blp");
// RealmList.xml.
constexpr u64 kHelpFrame[6] = {assetPathHash("Interface\\HelpFrame\\HelpFrame-TopLeft.blp"),
                               assetPathHash("Interface\\HelpFrame\\HelpFrame-Top.blp"),
                               assetPathHash("Interface\\HelpFrame\\HelpFrame-TopRight.blp"),
                               assetPathHash("Interface\\HelpFrame\\HelpFrame-BotLeft.blp"),
                               assetPathHash("Interface\\HelpFrame\\HelpFrame-Bottom.blp"),
                               assetPathHash("Interface\\HelpFrame\\HelpFrame-BotRight.blp")};
constexpr u64 kColumnTabs = assetPathHash("Interface\\FriendsFrame\\WhoFrame-ColumnTabs.blp");
constexpr u64 kSortArrow = assetPathHash("Interface\\Buttons\\UI-SortArrow.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
constexpr u64 kRowHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp");
constexpr u64 kCloseUp = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Up.blp");
constexpr u64 kCloseDown = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Down.blp");
constexpr u64 kCloseHighlight = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight.blp");
// CharacterSelect.xml.
constexpr u64 kCharacterHighlight =
    assetPathHash("Interface\\Glues\\CharacterSelect\\Glue-CharacterSelect-Highlight.blp");

// GlueFonts.xml.
constexpr u32 kGlueGold = rgba(255, 199, 0);    // GlueFontNormal (1.0, 0.78, 0)
constexpr u32 kGlueWhite = rgba(255, 255, 255); // GlueFontHighlight
constexpr u32 kGlueGray = rgba(128, 128, 128);  // GlueFontDisable
// DEFAULT_TOOLTIP_COLOR (AccountLogin.lua): the edit boxes' and the character frame's border and background.
constexpr u32 kTooltipBorderColor = rgba(204, 204, 204);
constexpr u32 kTooltipBackgroundColor = rgba(23, 23, 23);

u32 tint(u32 color, f32 alpha) {
    return ui::withAlpha(color, alpha);
}

// The glue's text: Friz with GlueFonts' outline and shadow (1 unit; 2 for Large and Huge).
TextStyle glueFont(const GlueLayout& L, f32 size, u32 color, f32 alpha) {
    TextStyle style;
    style.font = ui::FontId::Friz;
    style.size = L.u(size);
    style.color = tint(color, alpha);
    style.outline = std::max(0.75f, L.u(0.9f));
    style.shadow = std::max(1.0f, L.u(size >= 18.0f ? 2.0f : 1.0f));
    return style;
}

// GlueEditBoxFont: Arial Narrow 18, white.
TextStyle editFont(const GlueLayout& L, f32 alpha) {
    TextStyle style;
    style.font = ui::FontId::Arial;
    style.size = L.u(18.0f);
    style.color = tint(kGlueWhite, alpha);
    return style;
}

// At least a finger's size, centered on the art.
Rect touchRect(const Gui& g, const Rect& r) {
    const f32 min = g.px(ui::kTouchDp);
    Rect out = r;
    if (out.w < min) {
        out.x -= (min - out.w) * 0.5f;
        out.w = min;
    }
    if (out.h < min) {
        out.y -= (min - out.h) * 0.5f;
        out.h = min;
    }
    return out;
}

WidgetState hitIf(Gui& g, bool interactive, WidgetId id, const Rect& r) {
    return interactive ? ui::hit(g, id, r) : WidgetState{};
}

// One line of text centered vertically in `r`.
void line(Gui& g, const Rect& r, std::string_view text, const TextStyle& style) {
    ui::label(g, r, text, style);
}

// Word-wrapped text from the top of `r` (lines broken at "\n"); returns its height.
f32 wrapped(Gui& g, const Rect& r, std::string_view text, const TextStyle& style) {
    return g.draw.wrappedText(r, text, style);
}

// A text field's text with its caret, the end kept in sight.
void fieldText(Gui& g, const GlueLayout& L, const Rect& inner, std::string_view text, const TextStyle& style,
               bool focused, f32 alpha) {
    const f32 baseline = ui::centeredBaseline(g.draw, style, inner.centerY());
    g.draw.pushClip(inner);
    const f32 width = g.draw.measure(text, style.font, style.size);
    const f32 caret = std::max(1.0f, L.u(1.5f));
    const f32 x = width > inner.w - caret * 2.0f ? inner.right() - caret * 2.0f - width : inner.x;
    g.draw.text(x, baseline, text, style);
    if (focused && (g.ui.nowMs() / 500) % 2 == 0) {
        g.draw.solid({x + width + caret * 0.5f, inner.centerY() - style.size * 0.5f, caret, style.size},
                     tint(kGlueWhite, alpha));
    }
    g.draw.popClip();
}

// GlueButtonTemplate (15-unit font, text 3 units left and up of the center) and GlueButtonSmallTemplate (12,
// 3 up). The highlight (shown under the mouse in 1.12) shows while the finger holds it, the font turning
// white.
bool glueButton(Gui& g, const GlueLayout& L, WidgetId id, const Rect& r, std::string_view text, bool small,
                bool enabled, f32 alpha, bool interactive) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hitIf(g, interactive && enabled, id, target);
    const bool down = enabled && ui::held(state, target);
    g.draw.sprite(r,
                  !enabled ? kButtonDisabled
                  : down   ? kButtonDown
                           : kButtonUp,
                  tint(ui::kWhite, alpha), kButtonUv);
    if (down) {
        g.draw.spriteAdditive(r, kButtonHighlight, tint(ui::kWhite, alpha), kButtonHighlightUv);
    }
    TextStyle style = glueFont(L, small ? 12.0f : 15.0f,
                               !enabled ? kGlueGray
                               : down   ? kGlueWhite
                                        : kGlueGold,
                               alpha);
    style.align = Align::Center;
    const f32 scale =
        std::min(1.5f, r.h / L.u(small ? 38.0f : 45.0f)); // the offset follows a resized button's art
    const f32 dx = small ? 0.0f : L.u(-3.0f) * scale, dy = L.u(-3.0f) * scale;
    line(g, {r.x + dx, r.y + dy, r.w, r.h}, text, style);
    return enabled && interactive && ui::tapped(state);
}

// A backdrop: the background inside its insets (tinted), then the border (edge units wide).
void backdrop(Gui& g, const GlueLayout& L, const Rect& r, u64 background, u32 backgroundColor, u64 border,
              u32 borderColor, f32 edge, f32 left, f32 right, f32 top, f32 bottom, f32 alpha) {
    g.draw.sprite({r.x + L.u(left), r.y + L.u(top), r.w - L.u(left + right), r.h - L.u(top + bottom)},
                  background, tint(backgroundColor, alpha));
    g.draw.backdropBorder(r, border, L.u(edge), tint(borderColor, alpha));
}

// AccountLogin's edit boxes: UI-Tooltip-Background inside Glue-Tooltip-Border (insets 10/5/4/9, edge 16) in
// DEFAULT_TOOLTIP_COLOR, the text in GlueEditBoxFont 15 units from the left, clear of the 5 units at the
// bottom.
bool editBox(Gui& g, const GlueLayout& L, WidgetId id, const Rect& r, std::string_view text, bool focused,
             f32 alpha, bool interactive) {
    const WidgetState state = hitIf(g, interactive, id, touchRect(g, r));
    backdrop(g, L, r, kTooltipBackground, kTooltipBackgroundColor, kGlueTooltipBorder, kTooltipBorderColor,
             16.0f, 10.0f, 5.0f, 4.0f, 9.0f, alpha);
    fieldText(g, L, {r.x + L.u(15.0f), r.y, r.w - L.u(22.0f), r.h - L.u(5.0f)}, text, editFont(L, alpha),
              focused, alpha);
    return interactive && ui::tapped(state);
}

// GlueDialog's edit boxes: UI-ChatInputBorder-Left / Right reaching 10 units past the box on each side (the
// left art's long middle covers a box wider than the XML's 130 units).
bool chatInputBox(Gui& g, const GlueLayout& L, WidgetId id, const Rect& r, std::string_view text,
                  bool focused) {
    const WidgetState state = ui::hit(g, id, touchRect(g, r));
    const f32 half = r.w * 0.5f + L.u(10.0f);
    const f32 u = std::min(1.0f, half / L.u(256.0f));
    g.draw.sprite({r.x - L.u(10.0f), r.y, half, r.h}, kInputLeft, ui::kWhite, {0.0f, 0.0f, u, 1.0f});
    g.draw.sprite({r.centerX(), r.y, half, r.h}, kInputRight, ui::kWhite, {1.0f - u, 0.0f, 1.0f, 1.0f});
    TextStyle style = glueFont(L, 15.0f, kGlueWhite, 1.0f); // GlueFontHighlight
    style.outline = 0.0f;
    fieldText(g, L, {r.x + L.u(2.0f), r.y, r.w - L.u(4.0f), r.h}, text, style, focused, 1.0f);
    return ui::tapped(state);
}

// The dialogs' backdrop (UI-DialogBox-Background inside UI-DialogBox-Border: insets 11/12/12/11, edge 32).
void dialogBackdrop(Gui& g, const GlueLayout& L, const Rect& r) {
    backdrop(g, L, r, kDialogBackground, ui::kWhite, kDialogBorder, ui::kWhite, 32.0f, 11.0f, 12.0f, 12.0f,
             11.0f, 1.0f);
}

// GlueDialogButtonTemplate: 200 x 40 (GluePanelButton art, GlueFontNormal, its text 2 units up).
bool dialogButton(Gui& g, const GlueLayout& L, WidgetId id, const Rect& r, std::string_view text,
                  bool enabled) {
    const Rect target = touchRect(g, r);
    const WidgetState state = enabled ? ui::hit(g, id, target) : WidgetState{};
    const bool down = enabled && ui::held(state, target);
    g.draw.sprite(r, !enabled ? kButtonDisabled : down ? kButtonDown : kButtonUp, ui::kWhite, kButtonUv);
    if (down) {
        g.draw.spriteAdditive(r, kButtonHighlight, ui::kWhite, kButtonHighlightUv);
    }
    TextStyle style = glueFont(L, 15.0f, !enabled ? kGlueGray : down ? kGlueWhite : kGlueGold, 1.0f);
    style.align = Align::Center;
    line(g, {r.x, r.y - L.u(2.0f), r.w, r.h}, text, style);
    return enabled && ui::tapped(state);
}

// The buttons of a GlueDialog at its bottom: two (Button1 ending 6 units left of the middle, Button2 13 units
// after it), or one centered.
void dialogButtons(Gui& g, const GlueLayout& L, const Rect& panel, std::string_view name, std::string_view b1,
                   bool b1Enabled, std::string_view b2, GlueDialogResult& out) {
    const f32 w = std::min(L.u(200.0f), (panel.w - L.u(40.0f)) * 0.5f), h = L.u(40.0f);
    const f32 y = panel.bottom() - L.u(16.0f) - h;
    if (b2.empty()) {
        out.button1 =
            dialogButton(g, L, widgetId(name, 1), {panel.centerX() - w * 0.5f, y, w, h}, b1, b1Enabled);
        return;
    }
    out.button1 =
        dialogButton(g, L, widgetId(name, 1), {panel.centerX() - L.u(6.0f) - w, y, w, h}, b1, b1Enabled);
    out.button2 = dialogButton(g, L, widgetId(name, 2), {panel.centerX() + L.u(7.0f), y, w, h}, b2, true);
}

// GlueCloseButton (UI-Panel-MinimizeButton) inside a finger's target.
bool closeButton(Gui& g, WidgetId id, const Rect& r, bool interactive) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hitIf(g, interactive, id, target);
    const bool down = ui::held(state, target);
    g.draw.sprite(r, down ? kCloseDown : kCloseUp);
    if (down) {
        g.draw.spriteAdditive(r, kCloseHighlight);
    }
    return interactive && ui::tapped(state);
}

// Where a dialog centers: the screen, or what the keyboard leaves of it.
Rect dialogArea(const GlueLayout& L) {
    return L.keyboard ? L.visible : L.screen;
}

} // namespace

// --- Layout ------------------------------------------------------------------------------------------------

ui::Rect GlueLayout::at(Point point, f32 x, f32 y, f32 w, f32 h) const {
    const f32 pw = u(w), ph = u(h);
    f32 ax = screen.x, ay = screen.y; // the screen's point
    f32 fx = 0.0f, fy = 0.0f;         // the frame's point, from its top left
    switch (point) {
    case Point::TopLeft: break;
    case Point::Top:
        ax = screen.centerX();
        fx = pw * 0.5f;
        break;
    case Point::TopRight:
        ax = screen.right();
        fx = pw;
        break;
    case Point::Left:
        ay = screen.centerY();
        fy = ph * 0.5f;
        break;
    case Point::Center:
        ax = screen.centerX();
        ay = screen.centerY();
        fx = pw * 0.5f;
        fy = ph * 0.5f;
        break;
    case Point::Right:
        ax = screen.right();
        ay = screen.centerY();
        fx = pw;
        fy = ph * 0.5f;
        break;
    case Point::BottomLeft:
        ay = screen.bottom();
        fy = ph;
        break;
    case Point::Bottom:
        ax = screen.centerX();
        ay = screen.bottom();
        fx = pw * 0.5f;
        fy = ph;
        break;
    case Point::BottomRight:
        ax = screen.right();
        ay = screen.bottom();
        fx = pw;
        fy = ph;
        break;
    }
    return {ax + u(x) - fx, ay - u(y) - fy, pw, ph};
}

GlueLayout glueLayout(const DialogContext& c) {
    GlueLayout L;
    L.screen = c.ui.safeArea();
    if (L.screen.w <= 0.0f || L.screen.h <= 0.0f) {
        L.screen = c.safeArea;
    }
    L.visible = c.safeArea;
    L.keyboard = L.visible.bottom() < L.screen.bottom() - 1.0f;
    L.unit = std::max(L.screen.h / 768.0f, c.ui.px(kGlueMinUnitDp));
    L.heightUnits = L.screen.h / L.unit;
    L.widthUnits = L.screen.w / L.unit;
    L.compact = L.heightUnits < kGlueCompactUnits;
    return L;
}

void glueLogo(DialogContext& c, f32 alpha) {
    const GlueLayout L = glueLayout(c);
    // TOPLEFT (3, -7), 256 x 128; at most a quarter of a short screen's height.
    const f32 k = std::clamp(L.heightUnits * 0.24f / 128.0f, 0.55f, 1.0f);
    c.draw.sprite(L.at(Point::TopLeft, 3.0f, -7.0f, 256.0f * k, 128.0f * k), kWowLogo,
                  tint(ui::kWhite, alpha));
}

// --- AccountLogin ------------------------------------------------------------------------------------------

AccountLoginResult accountLogin(DialogContext& c, const AccountLoginView& v) {
    VD_PROFILE_ZONE("GlueWindows::accountLogin");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    const f32 a = v.alpha;
    AccountLoginResult result;
    static constexpr text::Key kLabels[2] = {"ACCOUNT_NAME", "PASSWORD"};
    static constexpr WidgetId kFields[2] = {widgetId("glue.login.account"), widgetId("glue.login.password")};
    std::array<char, 64> stars{};
    const u32 starCount = std::min<u32>(v.passwordLength, static_cast<u32>(stars.size()));
    std::fill_n(stars.begin(), starCount, '*');
    const std::string_view texts[2] = {v.account, {stars.data(), starCount}};

    // The keyboard covers the bottom: the edited field (with its label) and LOGIN, in what it leaves.
    if (L.keyboard && v.editing >= 0 && v.editing < 2) {
        const Rect& area = L.visible;
        const f32 labelH = L.u(24.0f), fieldH = L.u(37.0f), gap = L.u(14.0f), buttonH = L.u(45.0f);
        const bool button = area.h >= labelH + fieldH + gap + buttonH + L.u(16.0f);
        const f32 total = labelH + fieldH + (button ? gap + buttonH : 0.0f);
        f32 y = area.y + std::max(L.u(4.0f), (area.h - total) * 0.5f);
        const f32 fieldW = std::min(area.w - L.u(20.0f), L.u(320.0f));
        TextStyle label = glueFont(L, 15.0f, kGlueGold, 1.0f);
        label.align = Align::Center;
        const i32 i = v.editing;
        line(g, {area.centerX() - L.u(128.0f), y, L.u(256.0f), labelH}, text::get(kLabels[i]), label);
        y += labelH;
        if (editBox(g, L, kFields[i], {area.centerX() - fieldW * 0.5f, y, fieldW, fieldH}, texts[i], true,
                    1.0f, true)) {
            result.edit = i;
        }
        y += fieldH + gap;
        if (button && glueButton(g, L, widgetId("glue.login.login"),
                                 {area.centerX() - L.u(85.0f), y, L.u(170.0f), buttonH}, text::get("LOGIN"),
                                 false, true, 1.0f, true)) {
            result.login = true;
        }
        return result;
    }

    // AccountLoginUI as AccountLogin.xml anchors it. Short screens: the logos smaller, the fields and LOGIN
    // lower.
    const bool interactive = v.interactive && a > 0.5f;
    glueLogo(c, a);
    const f32 shift = std::max(0.0f, 700.0f - L.heightUnits) * 0.42f;
    const f32 small = L.compact ? 0.7f : 1.0f;
    c.draw.sprite(L.at(Point::Bottom, 0.0f, 8.0f, 100.0f * small, 100.0f * small), kBlizzardLogo,
                  tint(ui::kWhite, a));
    TextStyle disclaimer = glueFont(L, 12.0f, kGlueGold, a);
    disclaimer.align = Align::Center;
    line(g, L.at(Point::Bottom, 0.0f, 10.0f, std::min(L.widthUnits, 600.0f), 14.0f),
         text::get("BLIZZ_DISCLAIMER"), disclaimer);
    // The ESRB notice (BOTTOMLEFT 20 / 45; phase 21: GlueLocalization.lua's LocalizeFrames shows it in the enUS
    // client only) and the version's two lines from 10 units above the bottom left.
    if (text::get("VD_GLUE_SHOW_ESRB_RATING") != "0") {
        c.draw.sprite(L.at(Point::BottomLeft, 20.0f, 45.0f, 128.0f * small, 128.0f * small), kEsrbRating,
                      tint(ui::kWhite, a));
    }
    const TextStyle version = glueFont(L, 12.0f, kGlueGold, a);
    const f32 versionH = g.draw.measureWrapped(L.u(400.0f), v.version, version.font, version.size);
    wrapped(g, {L.screen.x, L.screen.bottom() - L.u(10.0f) - versionH, L.u(400.0f), versionH + 1.0f},
            v.version, version);

    // The fields with their labels (GlueFontNormal, centered 9 units above the box) and LOGIN.
    const Rect fields[2] = {L.at(Point::Bottom, 8.0f, 345.0f - shift, 160.0f, 37.0f),
                            L.at(Point::Bottom, 8.0f, 270.0f - shift, 160.0f, 37.0f)};
    for (i32 i = 0; i < 2; ++i) {
        TextStyle label = glueFont(L, 15.0f, kGlueGold, a);
        label.align = Align::Center;
        line(g, {fields[i].centerX() - L.u(128.0f), fields[i].y - L.u(19.0f), L.u(256.0f), L.u(20.0f)},
             text::get(kLabels[i]), label);
        if (editBox(g, L, kFields[i], fields[i], texts[i], v.editing == i, a, interactive)) {
            result.edit = i;
        }
    }
    result.login = glueButton(g, L, widgetId("glue.login.login"),
                              L.at(Point::Bottom, 8.0f, 204.0f - shift, 170.0f, 45.0f), text::get("LOGIN"),
                              false, true, a, interactive);
    const Rect quit = L.at(Point::BottomRight, -5.0f, 29.0f, 150.0f, 38.0f);
    result.quit =
        glueButton(g, L, widgetId("glue.login.quit"), quit, text::get("QUIT"), true, true, a, interactive);

    // Ours in AccountLoginTOSButton's place (80 units above QUIT; this client has no Terms of Use, Credits or
    // Cinematics buttons): the realm server's button (1.12's realmlist.wtf), and hanging under it as under
    // the TOS button AccountLoginRealmName (GlueFontDisableSmall, right-aligned 8 units in, 10 units down),
    // then the server's address, or VD_LOGIN_NO_SERVER in gold while there is none.
    {
        const Rect button{quit.x, quit.y - L.u(80.0f) - quit.h, quit.w, quit.h};
        result.server = glueButton(g, L, widgetId("glue.login.server"), button, text::get("VD_LOGIN_SERVER"),
                                   true, true, a, interactive);
        TextStyle realm = glueFont(L, 12.0f, kGlueGray, a);
        realm.align = Align::Right;
        const f32 right = button.right() - L.u(8.0f);
        const f32 lineH = L.u(15.0f);
        f32 y = button.bottom() + L.u(10.0f);
        if (!v.realmName.empty()) {
            line(g, {right - L.u(256.0f), y, L.u(256.0f), lineH}, v.realmName, realm);
            y += lineH;
        }
        if (v.server.empty()) {
            realm.color = tint(kGlueGold, a);
        }
        line(g, {right - L.u(256.0f), y, L.u(256.0f), lineH},
             v.server.empty() ? text::get("VD_LOGIN_NO_SERVER") : v.server, realm);
    }

    // AccountLoginSaveAccountName: 20 x 20, 17 units from the left with its top 115 up; its label 24 units
    // right (10 units, gold); the whole row takes the finger.
    {
        const Rect box = L.at(Point::BottomLeft, 17.0f, 95.0f, 20.0f, 20.0f);
        TextStyle label = glueFont(L, 10.0f, kGlueGold, a);
        label.outline = 0.0f;
        const std::string_view caption = text::get("SAVE_ACCOUNT_NAME");
        const f32 textW = g.draw.measure(caption, label.font, label.size);
        const Rect target = touchRect(g, {box.x, box.y, L.u(24.0f) + textW, box.h});
        const WidgetState state = hitIf(g, interactive, widgetId("glue.login.save"), target);
        const bool down = ui::held(state, target);
        g.draw.sprite(box, down ? kCheckDown : kCheckUp, tint(ui::kWhite, a));
        if (v.saveAccountName) {
            g.draw.sprite(box, kCheckMark, tint(ui::kWhite, a));
        }
        if (down) {
            g.draw.spriteAdditive(box, kCheckHighlight, tint(ui::kWhite, a));
        }
        line(g, {box.x + L.u(24.0f), box.y, textW + L.u(2.0f), box.h}, caption, label);
        result.toggleSave = interactive && ui::tapped(state);
    }
    if (v.offline && glueButton(g, L, widgetId("glue.login.offline"),
                                L.at(Point::BottomLeft, 10.0f, 175.0f, 150.0f, 38.0f),
                                text::get("VD_LOGIN_OFFLINE"), true, true, a, interactive)) {
        result.offline = true;
    }
    return result;
}

// --- GlueDialog --------------------------------------------------------------------------------------------

GlueDialogResult glueDialog(DialogContext& c, const GlueDialogView& v) {
    VD_PROFILE_ZONE("GlueWindows::dialog");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    GlueDialogResult result;
    ui::modalBlocker(g, widgetId(v.name, 100), 0.0f); // GlueDialog covers GlueParent: nothing under it reacts
    const Rect area = dialogArea(L);
    // GlueDialogBackground: 512 wide (600 with the alert icon), GlueDialogText 440 wide 16 units from the top
    // (GlueFontNormalLarge), 16 + text + 8 [+ edit box + 8] + button + 16 high.
    const f32 width = std::min(L.u(v.alert ? 600.0f : 512.0f), area.w - L.u(8.0f));
    TextStyle style = glueFont(L, 18.0f, kGlueGold, 1.0f);
    style.align = Align::Center;
    const f32 textW = std::min(L.u(440.0f), width - L.u(v.alert ? 160.0f : 40.0f));
    const f32 textH = g.draw.measureWrapped(textW, v.text, style.font, style.size);
    const f32 editH = v.editBox ? L.u(32.0f + 8.0f) : 0.0f;
    const f32 height = std::min(area.h, L.u(16.0f) + textH + L.u(8.0f) + editH + L.u(40.0f + 16.0f));
    const Rect panel{area.centerX() - width * 0.5f, area.centerY() - height * 0.5f, width, height};
    dialogBackdrop(g, L, panel);
    g.ui.blocker(widgetId(v.name, 101), panel);
    if (v.alert) { // LEFT (12, 0), 64 x 64
        c.draw.sprite({panel.x + L.u(12.0f), panel.centerY() - L.u(32.0f), L.u(64.0f), L.u(64.0f)},
                      kAlertIcon);
    }
    wrapped(g, {panel.centerX() - textW * 0.5f, panel.y + L.u(16.0f), textW, textH + 1.0f}, v.text, style);
    if (v.editBox) {
        const f32 w = std::min(L.u(v.editWidth), panel.w - L.u(60.0f));
        result.edit = chatInputBox(
            g, L, widgetId(v.name, 3),
            {panel.centerX() - w * 0.5f, panel.y + L.u(16.0f) + textH + L.u(8.0f), w, L.u(32.0f)}, v.editText,
            v.editing);
    }
    dialogButtons(g, L, panel, v.name, v.button1, v.button1Enabled, v.button2, result);
    return result;
}

GlueDialogResult glueEditDialog(DialogContext& c, const GlueEditDialogView& v) {
    VD_PROFILE_ZONE("GlueWindows::editDialog");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    GlueDialogResult result;
    ui::modalBlocker(g, widgetId(v.name, 100), 0.0f);
    const Rect area = dialogArea(L);
    const f32 width = std::min(L.u(512.0f), area.w - L.u(8.0f));
    const f32 textW = std::min(L.u(400.0f), width - L.u(170.0f));
    TextStyle style1 = glueFont(L, 18.0f, kGlueGold, 1.0f); // GlueFontNormalLarge
    style1.align = Align::Center;
    TextStyle style2 = glueFont(L, 12.0f, kGlueGold, 1.0f); // GlueFontNormalSmall
    style2.align = Align::Center;
    const f32 h1 = g.draw.measureWrapped(textW, v.text1, style1.font, style1.size);
    const f32 h2 = g.draw.measureWrapped(textW, v.text2, style2.font, style2.size);
    // CharacterDeleteDialog_OnShow: 16 + text1 + text2 + 23 + edit box + 8 + button + 16; the rename dialog:
    // 256.
    f32 height = L.u(16.0f) + h1 + h2 + L.u(23.0f + 32.0f + 8.0f + 40.0f + 16.0f);
    if (v.fixedHeight) {
        height = std::max(height, L.u(256.0f));
    }
    height = std::min(height, area.h);
    const Rect panel{area.centerX() - width * 0.5f, area.centerY() - height * 0.5f, width, height};
    dialogBackdrop(g, L, panel);
    g.ui.blocker(widgetId(v.name, 101), panel);
    // The alert icon: LEFT (12, 10).
    c.draw.sprite({panel.x + L.u(12.0f), panel.centerY() - L.u(32.0f) - L.u(10.0f), L.u(64.0f), L.u(64.0f)},
                  kAlertIcon);
    f32 top = panel.y + L.u(16.0f);
    wrapped(g, {panel.centerX() - textW * 0.5f, top, textW, h1 + 1.0f}, v.text1, style1);
    top += h1 + L.u(20.0f);
    wrapped(g, {panel.centerX() - textW * 0.5f, top, textW, h2 + 1.0f}, v.text2, style2);
    top += h2 + L.u(5.0f);
    const f32 boxW = L.u(130.0f);
    result.edit = chatInputBox(g, L, widgetId(v.name, 3),
                               {panel.centerX() - boxW * 0.5f, top, boxW, L.u(32.0f)}, v.editText, v.editing);
    dialogButtons(g, L, panel, v.name, text::get("OKAY"), v.okEnabled, text::get("CANCEL"), result);
    return result;
}

// --- RealmList ---------------------------------------------------------------------------------------------

RealmListResult realmList(DialogContext& c, const RealmListView& v) {
    VD_PROFILE_ZONE("GlueWindows::realmList");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    RealmListResult result;
    const bool interactive = v.interactive;
    const f32 a = v.alpha;
    // RealmList: the screen dimmed to 75% black, RealmListBackground (640 x 512, 24 units right of the
    // center) on it, smaller on a short screen.
    ui::modalBlocker(g, widgetId("glue.realms.blocker"), 0.75f * a);
    const f32 scale = std::min({1.0f, (L.heightUnits - 16.0f) / 512.0f, (L.widthUnits - 16.0f) / 640.0f});
    const auto s = [&](f32 units) { return L.u(units * scale); };
    Rect panel = L.at(Point::Center, 24.0f * scale, 0.0f, 640.0f * scale, 512.0f * scale);
    panel.x = std::clamp(panel.x, L.screen.x, L.screen.right() - panel.w);
    g.ui.blocker(widgetId("glue.realms.panel"), panel);
    // HelpFrame-TopLeft / Top / TopRight (256, 256, 128 wide) over BotLeft / Bottom / BotRight, 256 high
    // each.
    static constexpr f32 kColumn[3] = {0.0f, 256.0f, 512.0f}, kColumnWidth[3] = {256.0f, 256.0f, 128.0f};
    for (u32 i = 0; i < 6; ++i) {
        const u32 k = i % 3;
        c.draw.sprite(
            {panel.x + s(kColumn[k]), panel.y + s(i < 3 ? 0.0f : 256.0f), s(kColumnWidth[k]), s(256.0f)},
            kHelpFrame[i], tint(ui::kWhite, a));
    }
    // RealmListHeader (UI-DialogBox-Header 256 x 64, TOP -12 / +12) with SERVER_SELECTION
    // (GlueFontNormalSmall).
    {
        const Rect header{panel.centerX() - s(12.0f) - s(128.0f), panel.y - s(12.0f), s(256.0f), s(64.0f)};
        c.draw.sprite(header, kDialogHeader, tint(ui::kWhite, a));
        TextStyle title = glueFont(L, 12.0f, kGlueGold, a);
        title.align = Align::Center;
        line(g, {header.x, header.y + s(14.0f) - L.u(2.0f), header.w, L.u(16.0f)},
             text::get("SERVER_SELECTION"), title);
    }
    if (closeButton(g, widgetId("glue.realms.close"),
                    {panel.right() - s(42.0f) - s(32.0f), panel.y + s(3.0f), s(32.0f), s(32.0f)},
                    interactive)) {
        result.cancel = true;
    }
    // The column headers (RealmSortButtonTemplate: WhoFrame-ColumnTabs, GlueFontHighlightSmall,
    // UI-SortArrow), from 21 units in, their bottoms 50 units below the top: name 223, type 80, characters
    // 110, population 144 wide.
    static constexpr f32 kHeaderWidth[4] = {223.0f, 80.0f, 110.0f, 144.0f};
    static constexpr text::Key kHeaderText[4] = {"REALM_NAME", "REALM_TYPE", "REALM_CHARACTERS",
                                                 "REALM_LOAD"};
    static constexpr RealmSortKey kHeaderKey[4] = {RealmSortKey::Name, RealmSortKey::Mode,
                                                   RealmSortKey::Characters, RealmSortKey::Load};
    const f32 headerH = s(19.0f);
    f32 x = panel.x + s(21.0f);
    const f32 headerY = panel.y + s(50.0f) - headerH;
    for (u32 i = 0; i < 4; ++i) {
        const Rect r{x, headerY, s(kHeaderWidth[i]), headerH};
        x += r.w;
        const f32 left = s(5.0f), right = s(4.0f);
        c.draw.sprite({r.x, r.y, left, r.h}, kColumnTabs, tint(ui::kWhite, a),
                      {0.0f, 0.0f, 0.078125f, 0.59375f});
        c.draw.sprite({r.x + left, r.y, r.w - left - right, r.h}, kColumnTabs, tint(ui::kWhite, a),
                      {0.078125f, 0.0f, 0.90625f, 0.59375f});
        c.draw.sprite({r.right() - right, r.y, right, r.h}, kColumnTabs, tint(ui::kWhite, a),
                      {0.90625f, 0.0f, 0.96875f, 0.59375f});
        const Rect target{r.x, r.y - std::max(0.0f, c.ui.px(ui::kTouchDp) - r.h) * 0.5f, r.w,
                          std::max(r.h, c.ui.px(ui::kTouchDp))};
        const WidgetState state = hitIf(g, interactive, widgetId("glue.realms.sort", i), target);
        if (ui::held(state, target)) {
            c.draw.spriteAdditive({r.x, r.y - s(2.0f), r.w + s(4.0f), r.h + s(5.0f)}, kTabHighlight,
                                  tint(ui::kWhite, a));
        }
        const TextStyle label = glueFont(L, 12.0f, kGlueWhite, a);
        const std::string_view caption = text::get(kHeaderText[i]);
        const f32 textW = std::min(g.draw.measure(caption, label.font, label.size), r.w - s(22.0f));
        line(g, {r.x + s(8.0f), r.y, textW + 1.0f, r.h}, caption, label);
        c.draw.sprite({r.x + s(8.0f) + textW + s(3.0f), r.centerY() - s(3.0f), s(9.0f), s(8.0f)}, kSortArrow,
                      tint(ui::kWhite, a), {0.0f, 0.0f, 0.5625f, 1.0f});
        if (interactive && ui::tapped(state)) {
            result.sort = static_cast<i32>(kHeaderKey[i]);
        }
    }

    // The rows (RealmListRealmButtonTemplate) from 56 units below the top, one every 20 units but at least a
    // finger high; they scroll when more are listed than fit above the buttons.
    const f32 rowH = std::max(s(20.0f), c.ui.px(ui::kTouchDp));
    const f32 listTop = panel.y + s(56.0f);
    const Rect listArea{panel.x + s(22.0f), listTop, s(557.0f),
                        panel.bottom() - s(13.0f + 35.0f + 14.0f) - listTop};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("glue.realms.scroll"), listArea);
    f32 y = view.top;
    for (usize i = 0; i < v.rows.size(); ++i) {
        const RealmRow& row = v.rows[i];
        const Rect r{listArea.x, y, listArea.w, rowH};
        y += rowH;
        const WidgetState state = hitIf(g, interactive && !row.down, widgetId("glue.realms.row", i), r);
        const bool selected = static_cast<i32>(i) == v.selected;
        const bool lit = selected && !row.down;
        const Rect strip{r.x, r.centerY() - s(8.0f), r.w, s(16.0f)};
        if (lit) { // RealmListHighlight: UI-QuestLogTitleHighlight (ADD) in the name's color
            c.draw.spriteAdditive(strip, kRowHighlight, tint(realmHighlightColor(row), a));
        } else if (ui::held(state, r)) {
            c.draw.spriteAdditive(strip, kRowHighlight, tint(rgba(255, 255, 255, 110), a));
        }
        // The name (GlueFontNormal, 220 wide from 5 units in; white when highlighted), the type 10 units
        // after it, the characters 51 after that, the population 50 further.
        const TextStyle name = glueFont(L, 15.0f, lit ? kGlueWhite : realmNameColor(row), a);
        line(g, {r.x + s(5.0f), strip.y, s(220.0f), strip.h}, row.name, name);
        const TextStyle type = glueFont(L, 12.0f, lit ? kGlueWhite : realmTypeColor(row), a);
        line(g, {r.x + s(235.0f), strip.y, s(95.0f), strip.h}, realmTypeText(row), type);
        if (row.characters > 0) {
            char count[16];
            const i32 n = std::snprintf(count, sizeof(count), "(%u)", row.characters);
            line(g, {r.x + s(336.0f), strip.y, s(70.0f), strip.h},
                 {count, static_cast<usize>(std::max(0, n))}, glueFont(L, 12.0f, kGlueWhite, a));
        }
        const TextStyle load = glueFont(L, 12.0f, lit ? kGlueWhite : realmLoadColor(row), a);
        line(g, {r.x + s(418.0f), strip.y, s(135.0f), strip.h}, realmLoadText(row), load);
        if (interactive && ui::tapped(state)) {
            if (selected) {
                result.enter = static_cast<i32>(i); // OnDoubleClick (touch: a second tap)
            } else {
                result.select = static_cast<i32>(i);
            }
        }
    }
    ui::endScroll(g, view, y - view.top);

    // RealmListCancelButton (125 x 35, BOTTOMRIGHT -46 / 13) and RealmListOkButton left of it (8 units over
    // it).
    const f32 bw = s(125.0f), bh = s(35.0f);
    const Rect cancel{panel.right() - s(46.0f) - bw, panel.bottom() - s(13.0f) - bh, bw, bh};
    const Rect ok{cancel.x - bw + s(8.0f), cancel.y, bw, bh};
    result.ok =
        interactive && dialogButton(g, L, widgetId("glue.realms.ok"), ok, text::get("OKAY"), v.okEnabled);
    if (interactive &&
        dialogButton(g, L, widgetId("glue.realms.cancel"), cancel, text::get("CANCEL"), true)) {
        result.cancel = true;
    }
    return result;
}

// --- CharacterSelect ---------------------------------------------------------------------------------------

CharacterSelectResult characterSelect(DialogContext& c, const CharacterSelectView& v) {
    VD_PROFILE_ZONE("GlueWindows::characterSelect");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    const f32 a = v.alpha;
    const bool interactive = v.interactive && a > 0.5f;
    CharacterSelectResult result;

    // CharacterSelectUI takes the fingers nothing else does: a horizontal drag on the scene turns the
    // character (CharacterSelectFrame_OnMouseDown / OnUpdate: CHARACTER_ROTATION_CONSTANT degrees per unit
    // dragged).
    {
        const WidgetState state = hitIf(g, interactive, widgetId("glue.chars.scene"), L.screen);
        if (state.pressed && state.touch && state.touch->moved) {
            result.turnDegrees = state.touch->dx / L.unit * kCharacterRotationPerUnit;
        }
    }
    glueLogo(c, a);

    // CharacterSelectCharacterFrame: 260 wide, TOPRIGHT -5 / -15, 642 high (on a short screen down to the
    // Back button's row); the Glue-Tooltip backdrop in DEFAULT_TOOLTIP_COLOR (the background at 85%).
    const f32 frameH = std::min(642.0f, L.heightUnits - 15.0f - 72.0f);
    const Rect frame = L.at(Point::TopRight, -5.0f, -15.0f, 260.0f, frameH);
    g.ui.blocker(widgetId("glue.chars.frame"), frame);
    backdrop(g, L, frame, kGlueTooltipBackground, ui::withAlpha(kTooltipBackgroundColor, 0.85f),
             kGlueTooltipBorder, kTooltipBorderColor, 16.0f, 10.0f, 5.0f, 4.0f, 9.0f, a);
    // CharSelectRealmName (GlueFontDisableLarge, TOP -10), CharSelectChangeRealmButton under it (text + 50
    // wide, 33).
    TextStyle realm = glueFont(L, 18.0f, kGlueGray, a);
    realm.align = Align::Center;
    const f32 realmH =
        std::max(L.u(13.0f), g.draw.measureWrapped(L.u(256.0f), v.realm, realm.font, realm.size));
    wrapped(g, {frame.centerX() - L.u(128.0f), frame.y + L.u(7.0f), L.u(256.0f), realmH + 1.0f}, v.realm,
            realm);
    {
        const std::string_view caption = text::get("CHANGE_REALM");
        const f32 w = g.draw.measure(caption, ui::FontId::Friz, L.u(12.0f)) + L.u(50.0f);
        const Rect r{frame.centerX() - w * 0.5f, frame.y + L.u(10.0f) + realmH, w, L.u(33.0f)};
        result.changeRealm =
            glueButton(g, L, widgetId("glue.chars.realm"), r, caption, true, v.connected, a, interactive);
    }

    // The characters (CharSelectCharacterButtonTemplate: 256 x 70 from 24 units in and 65 down, one every 57
    // units, 55 of them taking the finger): the name (GlueFontNormal), "Level N Class"
    // (GlueFontHighlightSmall), the zone (GlueFontDisableSmall); Glue-CharacterSelect-Highlight (ADD) on the
    // selected one. They scroll on a short frame.
    const f32 createH = v.canCreate ? 45.0f + 15.0f + 6.0f : 12.0f;
    const f32 listTop = frame.y + L.u(65.0f) - L.u(10.0f);
    const Rect listArea{frame.x + L.u(4.0f), listTop, frame.w - L.u(8.0f),
                        frame.bottom() - L.u(createH) - listTop};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("glue.chars.scroll"), listArea);
    f32 y = view.top + L.u(10.0f);
    for (usize i = 0; i < v.characters.size() && i < kMaxCharactersDisplayed; ++i) {
        const CharacterRowView& ch = v.characters[i];
        const Rect button{frame.x + L.u(24.0f), y, L.u(232.0f), L.u(70.0f)};
        const Rect hit{frame.x + L.u(6.0f), button.y - L.u(1.0f), frame.w - L.u(12.0f), L.u(57.0f)};
        y += L.u(57.0f);
        const WidgetState state = hitIf(g, interactive, widgetId("glue.chars.row", i), hit);
        const bool selected = static_cast<i32>(i) == v.selected;
        if (selected || ui::held(state, hit)) {
            c.draw.spriteAdditive({button.x - L.u(20.0f), button.y - L.u(8.0f), L.u(256.0f), L.u(74.0f)},
                                  kCharacterHighlight,
                                  tint(selected ? ui::kWhite : rgba(255, 255, 255, 120), a));
        }
        // The name keeps GlueFontNormal selected or not (the button's highlight font is for its own text).
        line(g, {button.x, button.y + L.u(3.0f), L.u(217.0f), L.u(18.0f)}, ch.name,
             glueFont(L, 15.0f, kGlueGold, a));
        line(g, {button.x, button.y + L.u(22.0f), L.u(217.0f), L.u(12.0f)}, ch.info,
             glueFont(L, 12.0f, kGlueWhite, a));
        line(g, {button.x, button.y + L.u(35.0f), L.u(217.0f), L.u(16.0f)}, ch.location,
             glueFont(L, 12.0f, kGlueGray, a));
        if (interactive && ui::tapped(state)) {
            if (selected) {
                result.enter = static_cast<i32>(i); // OnDoubleClick (touch: a second tap)
            } else {
                result.select = static_cast<i32>(i);
            }
        }
    }
    ui::endScroll(g, view, y - view.top + L.u(4.0f));
    // CharSelectCreateCharacterButton (GlueButtonSmallTemplate, text + 50 wide, 45 high, BOTTOM +15).
    if (v.canCreate) {
        const std::string_view caption = text::get("CREATE_NEW_CHARACTER");
        const f32 w = std::min(frame.w - L.u(16.0f),
                               g.draw.measure(caption, ui::FontId::Friz, L.u(12.0f)) + L.u(50.0f));
        const Rect r{frame.centerX() - w * 0.5f, frame.bottom() - L.u(15.0f) - L.u(45.0f), w, L.u(45.0f)};
        result.create =
            glueButton(g, L, widgetId("glue.chars.create"), r, caption, true, true, a, interactive);
    }

    // CharSelectCharacterName (GlueFontNormalHuge, BOTTOM +100), CharSelectEnterWorldButton (200 x 60, BOTTOM
    // +30). Touch: no CharacterSelectRotateLeft / Right under it, a finger dragged on the scene turns the character.
    TextStyle bigName = glueFont(L, 22.0f, kGlueGold, a);
    bigName.align = Align::Center;
    line(g, L.at(Point::Bottom, 0.0f, 100.0f, 400.0f, 26.0f), v.selectedName, bigName);
    const Rect enter = L.at(Point::Bottom, 0.0f, 30.0f, 200.0f, 60.0f);
    result.enterWorld = glueButton(g, L, widgetId("glue.chars.enter"), enter, text::get("ENTER_WORLD"), false,
                                   v.connected && v.selected >= 0, a, interactive);
    // CharacterSelectBackButton (100 x 35, BOTTOMRIGHT -30 / 25), CharacterSelectDeleteButton (165 x 35) left
    // of it.
    const Rect back = L.at(Point::BottomRight, -30.0f, 25.0f, 100.0f, 35.0f);
    result.back =
        glueButton(g, L, widgetId("glue.chars.back"), back, text::get("BACK"), true, true, a, interactive);
    const Rect remove{back.x - L.u(165.0f), back.y, L.u(165.0f), back.h};
    result.remove = glueButton(g, L, widgetId("glue.chars.delete"), remove, text::get("DELETE_CHARACTER"),
                               true, v.connected && v.selected >= 0, a, interactive);
    return result;
}

} // namespace vd
