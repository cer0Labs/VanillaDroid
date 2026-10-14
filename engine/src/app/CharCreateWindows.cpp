#include "app/CharCreateWindows.h"

#include "app/CharCreateLayout.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TexCoords;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;

// CharacterCreate.xml's art.
constexpr u64 kRaceIcons = assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Races.blp");
constexpr u64 kClassIcons =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes.blp");
constexpr u64 kGenderIcons =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Gender.blp");
constexpr u64 kFactionIcons =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Factions.blp");
constexpr u64 kIconShadow =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-IconShadow.blp");
constexpr u64 kBanners = assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Banners.blp");
constexpr u64 kBackground =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Background.blp");
constexpr u64 kLabelFrame =
    assetPathHash("Interface\\Glues\\CharacterCreate\\CharacterCreate-LabelFrame.blp");
constexpr u64 kScrollTop =
    assetPathHash("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-ScrollBar-Top.blp");
constexpr u64 kScrollBottom = assetPathHash("Interface\\ClassTrainerFrame\\UI-ClassTrainer-ScrollBar.blp");
// GlueTemplates.xml, GlueButtons.xml, GlueDialog.xml, GlueTooltip.xml.
constexpr u64 kTextPanelBorder = assetPathHash("Interface\\Glues\\Common\\TextPanel-Border.blp");
constexpr u64 kGlueTooltipBorder = assetPathHash("Interface\\Glues\\Common\\Glue-Tooltip-Border.blp");
constexpr u64 kTooltipBackground = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Background.blp");
constexpr u64 kTooltipBorder = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Border.blp");
constexpr u64 kGlueButtonUp = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Up.blp");
constexpr u64 kGlueButtonDown = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Down.blp");
constexpr u64 kButtonHilight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kLeftArrowUp = assetPathHash("Interface\\Glues\\Common\\Glue-LeftArrow-Button-Up.blp");
constexpr u64 kLeftArrowDown = assetPathHash("Interface\\Glues\\Common\\Glue-LeftArrow-Button-Down.blp");
constexpr u64 kRightArrowUp = assetPathHash("Interface\\Glues\\Common\\Glue-RightArrow-Button-Up.blp");
constexpr u64 kRightArrowDown = assetPathHash("Interface\\Glues\\Common\\Glue-RightArrow-Button-Down.blp");
constexpr u64 kScrollUpUp = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Up.blp");
constexpr u64 kScrollUpDown = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Down.blp");
constexpr u64 kScrollUpDisabled =
    assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Disabled.blp");
constexpr u64 kScrollDownUp = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Up.blp");
constexpr u64 kScrollDownDown = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Down.blp");
constexpr u64 kScrollDownDisabled =
    assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Disabled.blp");
constexpr u64 kScrollKnob = assetPathHash("Interface\\Buttons\\UI-ScrollBar-Knob.blp");
constexpr u64 kDialogBackground = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Background.blp");
constexpr u64 kDialogBorder = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Border.blp");

// GlueFonts.xml's colors.
constexpr u32 kGlueGold = rgba(255, 199, 0);    // GlueFontNormal (1.0, 0.78, 0)
constexpr u32 kGlueWhite = rgba(255, 255, 255); // GlueFontHighlight
// FACTION_BACKDROP_COLOR_TABLE (the texts' backdrops) and the name box's and GlueTooltip's colors.
constexpr u32 kAllianceBackdrop = rgba(23, 23, 48);
constexpr u32 kHordeBackdrop = rgba(48, 13, 13);
constexpr u32 kNameBorder = rgba(128, 128, 128);
constexpr u32 kTooltipColor = rgba(23, 23, 48);

constexpr f32 kDragDegreesPerUnit = 0.6f;       // CHARACTER_ROTATION_CONSTANT
constexpr u32 kNameLetters = 12;                // CharacterCreateNameEdit letters="12"
constexpr u32 kNameBytes = 47;

constexpr WidgetId kNameField = widgetId("create.name");
constexpr WidgetId kInfoScroll[3] = {widgetId("create.info", 0), widgetId("create.info", 1),
                                     widgetId("create.info", 2)};

// RACE_ICON_TCOORDS: four columns, the male icons in the top two rows (by ChrRaces id).
TexCoords raceIcon(u8 race, u8 gender) {
    static constexpr u8 kCell[9][2] = {{0, 0}, {0, 0}, {3, 1}, {1, 0}, {3, 0},
                                       {1, 1}, {0, 1}, {2, 0}, {2, 1}};
    const u8* cell = kCell[race < 9 ? race : 0];
    const f32 u = static_cast<f32>(cell[0]) * 0.25f;
    const f32 v = static_cast<f32>(cell[1] + (gender != 0 ? 2 : 0)) * 0.25f;
    return {u, v, u + 0.25f, v + 0.25f};
}

// CLASS_ICON_TCOORDS: four columns (the last three 63 texels wide), rows of a quarter (by ChrClasses id).
TexCoords classIcon(u8 cls) {
    static constexpr f32 kU[5] = {0.0f, 0.25f, 0.49609375f, 0.7421875f, 0.98828125f};
    static constexpr u8 kCell[12][2] = {{0, 0}, {0, 0}, {0, 2}, {0, 1}, {2, 0}, {2, 1},
                                        {0, 0}, {1, 1}, {1, 0}, {3, 1}, {0, 0}, {3, 0}};
    const u8* cell = kCell[cls < 12 ? cls : 0];
    const f32 v = static_cast<f32>(cell[1]) * 0.25f;
    return {kU[cell[0]], v, kU[cell[0] + 1], v + 0.25f};
}

std::string_view nameOf(const std::array<std::string_view, 16>& names, u32 id) {
    return id < names.size() ? names[id] : std::string_view{};
}

// A glue font (GlueFonts.xml: Friz, outlined, a shadow) at `units` of the layout, never under `minDp`.
TextStyle glueFont(const Gui& g, const CharCreateLayout& l, f32 units, f32 minDp, u32 color) {
    TextStyle style = ui::gameFont(std::max(units * l.unit, g.px(minDp)), color);
    style.outline = std::max(1.0f, style.size * 0.06f);
    return style;
}

Rect touchTarget(const Gui& g, const Rect& r) {
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

void centered(Gui& g, const Rect& r, std::string_view s, TextStyle style) {
    style.align = Align::Center;
    ui::label(g, r, s, style);
}

// A text that shrinks to fit `r` instead of being cut (the rows' labels in long languages).
void fitted(Gui& g, const Rect& r, std::string_view s, TextStyle style, f32 minSize) {
    const f32 w = g.draw.measure(s, style.font, style.size);
    if (w > r.w && w > 0.0f) {
        style.size = std::max(minSize, style.size * r.w / w);
    }
    centered(g, r, s, style);
}

// What the long press of a button asks (its GlueTooltip).
struct Tip {
    std::string_view text;
    Rect owner;
};

// CharacterCreateIconButtonTemplate: the icon, its shadow (class and gender buttons), ButtonHilight-Square
// while it is pressed or chosen (LockHighlight), and the chosen one's name over its bottom (HighlightText). A
// long press is its hover: the GlueTooltip with its name (OnEnter: not for the chosen one). Returns true when
// tapped.
bool iconButton(Gui& g, const CharCreateLayout& l, WidgetId id, const Rect& r, u64 sprite, TexCoords uv,
                bool selected, bool shadow, std::string_view name, Tip& tip) {
    if (shadow) {
        // UI-CharacterCreate-IconShadow: 64 x 64 at the centre (2, -2).
        const f32 s = r.w / 48.0f;
        g.draw.sprite({r.centerX() - 30.0f * s, r.centerY() - 30.0f * s, 64.0f * s, 64.0f * s}, kIconShadow);
    }
    const Rect target = touchTarget(g, r);
    const ui::WidgetState state = ui::hit(g, id, target);
    const bool down = ui::held(state, target);
    g.draw.sprite(r, sprite, ui::kWhite, uv);
    if (selected || down) {
        g.draw.spriteAdditive(r, kButtonHilight);
    }
    if (selected && !name.empty()) {
        TextStyle style = glueFont(g, l, 12.0f, 11.0f, kGlueGold);
        style.align = Align::Center;
        const f32 baseline = r.bottom() - 1.0f * l.unit - g.draw.lineHeight(style.font, style.size) * 0.25f;
        g.draw.text(r.centerX(), baseline, name, style);
    }
    const bool longPressed = state.touch && state.touch->longPressed;
    if (state.pressed && longPressed && !selected) {
        tip = {name, r};
    }
    return ui::tapped(state) && !longPressed;
}

// GlueButtonTemplate (`big`: GlueFontNormal) / GlueButtonSmallTemplate (GlueFontNormalSmall):
// Glue-Panel-Button.
bool glueButton(Gui& g, const CharCreateLayout& l, WidgetId id, const Rect& r, std::string_view s, bool big) {
    const Rect target = touchTarget(g, r);
    const ui::WidgetState state = ui::hit(g, id, target);
    const bool down = ui::held(state, target);
    g.draw.sprite(r, down ? kGlueButtonDown : kGlueButtonUp, ui::kWhite, {0.0f, 0.0f, 0.578125f, 0.75f});
    TextStyle style = glueFont(g, l, big ? 15.0f : 12.0f, big ? 14.0f : 12.5f, down ? kGlueWhite : kGlueGold);
    // ButtonText at the centre, 3 units up (GlueButtonTemplate also 3 units left).
    const f32 push = down ? std::max(1.0f, g.px(1.0f)) : 0.0f;
    centered(g, {r.x - (big ? 3.0f : 0.0f) * l.unit + push, r.y - 3.0f * l.unit + push, r.w, r.h}, s, style);
    return ui::tapped(state);
}

// A backdrop of the client: the background inset (left, top, right, bottom), the edge file in 8 cells.
void backdrop(Gui& g, const Rect& r, u64 background, u32 color, f32 left, f32 top, f32 right, f32 bottom,
              u64 edge, f32 edgeSize, u32 edgeColor = ui::kWhite) {
    g.draw.sprite({r.x + left, r.y + top, r.w - left - right, r.h - top - bottom}, background, color);
    g.draw.backdropBorder(r, edge, edgeSize, edgeColor);
}

// UI-CharacterCreate-Background inside the right column's border (TextPanel-Border, the texts' backdrop): the
// original's UI-CharacterCreate-OuterBorder is open at the top, where its WoW logo hangs (left out here).
void configFrame(Gui& g, const CharCreateLayout& l) {
    const f32 k = l.unit;
    backdrop(g, l.config, kBackground, ui::kWhite, 8.0f * k, 4.0f * k, 4.0f * k, 8.0f * k, kTextPanelBorder,
             32.0f * k);
}

// CharacterCustomizationFrameTemplate: the label plate (CharacterCreate-LabelFrame, 64 units of art around
// the row's middle) with the label, the two arrows. Returns -1 / +1 for an arrow tapped, 0 otherwise.
i32 customizationRow(Gui& g, const CharCreateLayout& l, u32 i, std::string_view label) {
    const f32 k = l.unit;
    const Rect& plate = l.plates[i];
    const f32 h = 64.0f * k, y = plate.centerY() - h * 0.5f;
    const f32 cap = 25.0f * k;
    g.draw.sprite({plate.x, y, cap, h}, kLabelFrame, ui::kWhite, {0.0f, 0.0f, 0.1953125f, 1.0f});
    g.draw.sprite({plate.x + cap, y, plate.w - 2.0f * cap, h}, kLabelFrame, ui::kWhite,
                  {0.1953125f, 0.0f, 0.8046875f, 1.0f});
    g.draw.sprite({plate.right() - cap, y, cap, h}, kLabelFrame, ui::kWhite, {0.8046875f, 0.0f, 1.0f, 1.0f});
    // GlueFontHighlightSmall at the plate's middle, 3 units up.
    fitted(g, {plate.x + cap * 0.6f, plate.y - 3.0f * k, plate.w - cap * 1.2f, plate.h}, label,
           glueFont(g, l, 12.0f, 12.0f, kGlueWhite), g.px(9.0f));
    i32 step = 0;
    for (u32 side = 0; side < 2; ++side) {
        const Rect& target = side == 0 ? l.leftArrows[i] : l.rightArrows[i];
        const ui::WidgetState state =
            ui::hit(g, widgetId(side == 0 ? "create.prev" : "create.next", i), target);
        const bool down = ui::held(state, target);
        // The arrow's art at its own size (32 units, a little more on small screens) in the finger's square.
        const f32 art = std::min(target.w, std::max(32.0f * k, g.px(34.0f)));
        const Rect a{target.centerX() - art * 0.5f, target.centerY() - art * 0.5f, art, art};
        g.draw.sprite(a, side == 0 ? (down ? kLeftArrowDown : kLeftArrowUp)
                                   : (down ? kRightArrowDown : kRightArrowUp));
        if (ui::tapped(state)) {
            step = side == 0 ? -1 : 1;
        }
    }
    return step;
}

// GlueScrollFrameTemplate beside a text that scrolls: the trough (UI-CharacterCreate-ScrollBar-Top and the
// trainer's scroll bar), the up and down buttons (disabled at the ends) and the knob. Only shown when it scrolls
// (OnScrollRangeChanged hides the trough otherwise). Besides the finger dragging the text itself, the buttons
// scroll half the bar's height (GlueScrollBarTemplate's OnClick) and the knob follows a finger along the bar (the
// Slider). True when a button scrolled (its UChatScrollButton).
bool scrollBar(Gui& g, const CharCreateLayout& l, u32 i, const Rect& view, ui::ScrollState& s) {
    if (s.content <= s.viewport + 1.0f) {
        return false;
    }
    const f32 k = l.unit;
    g.draw.sprite(
        {view.right() - 3.0f * k, view.y - 4.0f * k, 32.0f * k, std::min(128.0f * k, view.h + 4.0f * k)},
        kScrollTop, ui::kWhite, {0.0f, 0.0f, 1.0f, std::min(1.0f, (view.h + 4.0f * k) / (128.0f * k))});
    const f32 bottomH = std::min(123.0f * k, view.h * 0.5f);
    g.draw.sprite({view.right() - 3.0f * k, view.bottom() + 2.0f * k - bottomH, 30.0f * k, bottomH},
                  kScrollBottom, ui::kWhite,
                  {0.53125f, 1.0f - (bottomH / (123.0f * k)) * 0.96875f, 1.0f, 1.0f});
    const f32 x = view.right() + 6.0f * k, w = 16.0f * k;
    const f32 top = view.y + 16.0f * k, bottom = view.bottom() - 16.0f * k;
    const f32 track = std::max(0.0f, bottom - top - w);
    // A finger's size, but never over the text (a finger there scrolls it).
    auto target = [&](const Rect& r) {
        Rect t = touchTarget(g, r);
        const f32 left = std::max(t.x, view.right());
        t.w = t.right() - left;
        t.x = left;
        return t;
    };
    const ui::WidgetState bar = ui::hit(g, widgetId("create.infoBar", i), target({x, top, w, bottom - top}));
    if (bar.pressed && bar.touch && track > 0.0f) {
        s.offset = std::clamp((bar.touch->y - top - 0.5f * w) / track, 0.0f, 1.0f) * s.maxOffset();
        s.velocity = 0.0f;
    }
    const Rect up{x, top - w, w, w}, down{x, bottom, w, w};
    const Rect upTarget = target(up), downTarget = target(down);
    const ui::WidgetState upState = ui::hit(g, widgetId("create.infoUp", i), upTarget);
    const ui::WidgetState downState = ui::hit(g, widgetId("create.infoDown", i), downTarget);
    const f32 half = (bottom - top) * 0.5f; // parent:GetHeight() / 2
    bool scrolled = false;
    if (s.offset > 0.5f && ui::tapped(upState)) {
        s.offset = std::max(0.0f, s.offset - half);
        s.velocity = 0.0f;
        scrolled = true;
    }
    if (s.offset < s.maxOffset() - 0.5f && ui::tapped(downState)) {
        s.offset = std::min(s.maxOffset(), s.offset + half);
        s.velocity = 0.0f;
        scrolled = true;
    }
    const TexCoords uv{0.25f, 0.25f, 0.75f, 0.75f}; // GlueScrollBarButton
    const bool atTop = s.offset <= 0.5f, atEnd = s.offset >= s.maxOffset() - 0.5f;
    const bool upDown = !atTop && ui::held(upState, upTarget), downDown = !atEnd && ui::held(downState, downTarget);
    g.draw.sprite(up, atTop ? kScrollUpDisabled : upDown ? kScrollUpDown : kScrollUpUp, ui::kWhite, uv);
    g.draw.sprite(down, atEnd ? kScrollDownDisabled : downDown ? kScrollDownDown : kScrollDownUp, ui::kWhite, uv);
    const f32 knob = top + track * (s.maxOffset() > 0.0f ? s.offset / s.maxOffset() : 0.0f);
    g.draw.sprite({x, knob, w, w}, kScrollKnob, ui::kWhite, uv);
    return scrolled;
}

// The faction, race and class texts (CharacterCreateCharacterFaction / Race / Class): the backdrop in the
// faction's color, the icon over the top left corner, the title and the text scrolling below. A closed frame
// (wide screens) shows its icon and title only; a tap on it opens it. `scrolled`: a scroll button scrolled it.
bool infoFrame(Gui& g, const CharCreateLayout& l, u32 i, const Rect& r, bool open, bool alliance, u64 icon,
               TexCoords iconUv, std::string_view title, std::string_view body, std::string_view abilities,
               ui::ScrollState& scroll, bool& scrolled) {
    const f32 k = l.unit;
    const ui::WidgetState state =
        ui::hit(g, widgetId("create.infoFrame", i), r); // the frame takes its fingers
    backdrop(g, r, kTooltipBackground, alliance ? kAllianceBackdrop : kHordeBackdrop, 8.0f * k, 4.0f * k,
             4.0f * k, 8.0f * k, kTextPanelBorder, 32.0f * k);
    const TextStyle titleStyle = glueFont(g, l, 18.0f, 16.0f, kGlueGold);
    const f32 titleH = titleStyle.size * 1.25f;
    const Rect view{r.x + 17.0f * k, r.y + 10.0f * k, 190.0f * k, std::max(0.0f, r.h - 20.0f * k)};
    if (!open) {
        ui::label(g, {view.x + 32.0f * k, r.y, view.w - 32.0f * k, r.h}, title, titleStyle);
    } else {
        // The scroll frame: its title and texts move together (they are its scroll child).
        ui::hit(g, kInfoScroll[i], view);
        scroll.longPressScrolls = true; // a text: nothing to pick up, and a finger often rests before it drags
        ui::scrollStep(scroll, g.ui.touches(), view, g.ui.dp(), g.ui.nowMs());
        g.draw.pushClip(view);
        f32 y = view.y - scroll.offset;
        ui::label(g, {view.x + 32.0f * k, y, view.w - 32.0f * k, titleH}, title, titleStyle);
        // The text under the title and under the icon (its bottom 40 units below the frame's top): the icon
        // would cover its first letter.
        y = std::max(y + titleH + 2.0f * k, r.y + 42.0f * k - scroll.offset);
        // GlueFontCharacterCreate (white, spacing 1.5) and the abilities in GlueFontNormalSmall (gold).
        TextStyle bodyStyle = glueFont(g, l, 12.0f, 12.5f, kGlueWhite);
        bodyStyle.outline = 0.0f;
        y += g.draw.wrappedText({view.x, y, view.w, 1e6f}, body, bodyStyle);
        if (!abilities.empty()) {
            TextStyle abilityStyle = glueFont(g, l, 12.0f, 12.5f, kGlueGold);
            abilityStyle.outline = 0.0f;
            y += 2.0f * k + g.draw.lineHeight(abilityStyle.font, abilityStyle.size);
            y += g.draw.wrappedText({view.x, y, view.w, 1e6f}, abilities, abilityStyle);
        }
        g.draw.popClip();
        ui::scrollSetContent(scroll, y + scroll.offset - view.y + 5.0f * k, view.h);
        scrolled = scrollBar(g, l, i, view, scroll) || scrolled;
    }
    g.draw.sprite({r.x - 3.0f * k, r.y - 8.0f * k, 48.0f * k, 48.0f * k}, icon, ui::kWhite, iconUv);
    return !open && ui::tapped(state);
}

// The GlueTooltip (GlueTooltip_SetOwner(nil, nil, -3, -5) / GlueTooltip_SetText(text, nil, 1, 1, 1)): white
// GlueFontNormalSmall in the tooltip backdrop, its bottom left at the owner's top right (-3, -5).
void glueTooltip(Gui& g, const CharCreateLayout& l, const Tip& tip) {
    if (tip.text.empty()) {
        return;
    }
    const f32 k = l.unit;
    const TextStyle style = glueFont(g, l, 12.0f, 13.0f, kGlueWhite);
    const f32 w = g.draw.measure(tip.text, style.font, style.size) + 25.0f * k;
    const f32 h = g.draw.lineHeight(style.font, style.size) + 20.0f * k;
    Rect r{tip.owner.right() - 3.0f * k, tip.owner.y + 5.0f * k - h, w, h};
    r.x = std::clamp(r.x, 0.0f, std::max(0.0f, g.ui.width() - w));
    r.y = std::clamp(r.y, 0.0f, std::max(0.0f, g.ui.height() - h));
    backdrop(g, r, kTooltipBackground, kTooltipColor, 5.0f * k, 5.0f * k, 5.0f * k, 5.0f * k, kTooltipBorder,
             16.0f * k);
    ui::label(g, {r.x + 10.0f * k, r.y + 10.0f * k, w - 15.0f * k, h - 20.0f * k}, tip.text, style);
}

// GlueDialog over the screen (modal, no dim): the dialog box 512 units wide, its text (GlueFontNormalLarge,
// 440 units wide) and its one button (GlueDialogButtonTemplate, 200 x 40). Returns true when the button is
// tapped.
bool glueDialog(Gui& g, const CharCreateLayout& l, const Rect& area, std::string_view text,
                std::string_view button) {
    ui::modalBlocker(g, widgetId("create.dialog.blocker"), 0.0f);
    const f32 k = l.unit;
    const f32 w = std::min(512.0f * k, area.w - 2.0f * g.px(8.0f));
    const f32 textW = w - 72.0f * k;
    const TextStyle style = glueFont(g, l, 18.0f, 16.0f, kGlueGold);
    const f32 textH = g.draw.measureWrapped(textW, text, style.font, style.size);
    const f32 buttonH = std::max(40.0f * k, g.px(ui::kTouchDp));
    const f32 h = 16.0f * k + textH + 8.0f * k + buttonH + 16.0f * k;
    const Rect box{area.centerX() - w * 0.5f, area.centerY() - h * 0.5f, w, h};
    backdrop(g, box, kDialogBackground, ui::kWhite, 11.0f * k, 12.0f * k, 12.0f * k, 11.0f * k, kDialogBorder,
             32.0f * k);
    TextStyle centeredStyle = style;
    centeredStyle.align = Align::Center;
    g.draw.wrappedText({box.centerX() - textW * 0.5f, box.y + 16.0f * k, textW, textH + 1.0f}, text,
                       centeredStyle);
    const f32 buttonW = std::min(200.0f * k, w - 32.0f * k);
    return glueButton(g, l, widgetId("create.dialog.button"),
                      {box.centerX() - buttonW * 0.5f, box.bottom() - 16.0f * k - buttonH, buttonW, buttonH},
                      button, true);
}

// The letters of a name, at most `letters` of them (UTF-8).
std::string_view firstLetters(std::string_view s, u32 letters) {
    usize i = 0;
    for (u32 n = 0; n < letters && i < s.size(); ++n) {
        ui::decodeUtf8(s, i);
    }
    return s.substr(0, i);
}

} // namespace

CharCreateResult characterCreateScreen(DialogContext& c, ui::TextInput& text, FormText<48>& name,
                                       const CharCreateView& v, CharCreateUiState& state) {
    VD_PROFILE_ZONE("CharCreateWindows::screen");
    Gui g{c.ui, c.draw};
    CharCreateResult result;
    const u64 nowMs = c.ui.nowMs();

    // The name box: what the keyboard types (12 letters), Enter accepts (OnEnterPressed:
    // CharacterCreate_Okay).
    const bool editing = text.active() && text.field() == kNameField;
    if (editing) {
        const std::string_view typed = firstLetters(text.text(), kNameLetters);
        if (typed.size() < text.text().size()) {
            text.setText(typed);
        }
        name.set(text.text());
        if (text.submitted()) {
            result.accept = true;
            text.end();
        }
    }

    // The layout: the area without the keyboard; the name rises over it while it is edited.
    const Rect area = v.layoutArea.w > 0.0f ? v.layoutArea : c.safeArea;
    CharCreateLayoutInput in;
    in.area = area;
    in.keyboardTop = editing && c.safeArea.bottom() < area.bottom() - 1.0f ? c.safeArea.bottom() : 0.0f;
    in.dp = c.ui.dp();
    in.wide = c.profile == ScreenProfile::Wide;
    in.classCount = static_cast<u32>(std::max<usize>(1, v.classes.size()));
    in.facialHair = v.rows[4].shown;
    in.expanded = state.expanded;
    in.randomizeEm = g.draw.measure(text::get("RANDOMIZE"), ui::FontId::Friz, 100.0f) / 100.0f;
    const CharCreateLayout l = charCreateLayout(in);
    const f32 k = l.unit;

    // The scene: every finger that lands on no frame turns the character (CharacterCreateFrame's mouse).
    const ui::WidgetState scene =
        ui::hit(g, widgetId("create.scene"), {0.0f, 0.0f, c.ui.width(), c.ui.height()});
    if (scene.pressed && scene.touch && scene.touch->moved) {
        result.turnDegrees += scene.touch->dx / std::max(k, 0.01f) * kDragDegreesPerUnit;
    }

    Tip tip;
    // CharacterCreateConfigurationFrame.
    ui::hit(g, widgetId("create.config"), l.config);
    configFrame(g, l);
    g.draw.sprite(l.banners[0], kBanners, ui::kWhite, {0.0f, 0.0f, 0.5f, 1.0f});
    g.draw.sprite(l.banners[1], kBanners, ui::kWhite, {0.5f, 0.0f, 1.0f, 1.0f});
    const TextStyle factionStyle = glueFont(g, l, 15.0f, 13.0f, kGlueGold);
    centered(g, l.allianceLabel, text::get("ALLIANCE"), factionStyle);
    centered(g, l.hordeLabel, text::get("HORDE"), factionStyle);
    for (u32 i = 0; i < v.races.size() && i < l.races.size(); ++i) {
        const u8 race = v.races[i];
        if (iconButton(g, l, widgetId("create.race", race), l.races[i], kRaceIcons, raceIcon(race, v.gender),
                       race == v.race, false, nameOf(v.raceNames, race), tip)) {
            result.race = race;
        }
    }
    static constexpr const char* kGenders[2] = {"MALE", "FEMALE"};
    for (u8 gender = 0; gender < 2; ++gender) {
        const f32 u = static_cast<f32>(gender) * 0.5f;
        if (iconButton(g, l, widgetId("create.gender", gender), l.genders[gender], kGenderIcons,
                       {u, 0.0f, u + 0.5f, 1.0f}, gender == v.gender, true, text::get(kGenders[gender]),
                       tip)) {
            result.gender = gender;
        }
    }
    for (u32 i = 0; i < v.classes.size() && i < l.classes.size(); ++i) {
        const u8 cls = v.classes[i];
        if (iconButton(g, l, widgetId("create.class", cls), l.classes[i], kClassIcons, classIcon(cls),
                       cls == v.cls, true, nameOf(v.classNames, cls), tip)) {
            result.cls = cls;
        }
    }
    for (u32 i = 0; i < v.rows.size(); ++i) {
        if (!v.rows[i].shown) {
            continue;
        }
        if (const i32 step = customizationRow(g, l, i, v.rows[i].label); step != 0) {
            result.row = static_cast<i32>(i);
            result.step = step;
        }
    }
    result.randomize =
        glueButton(g, l, widgetId("create.randomize"), l.randomize, text::get("RANDOMIZE"), false);

    // The texts: back to their top when what they describe changes (ScrollBar:SetValue(0)).
    if (state.shownAlliance != v.alliance) {
        state.scrolls[0].offset = 0.0f;
        state.shownAlliance = v.alliance;
    }
    if (state.shownRace != v.race) {
        state.scrolls[1].offset = 0.0f;
        state.shownRace = v.race;
    }
    if (state.shownClass != v.cls) {
        state.scrolls[2].offset = 0.0f;
        state.shownClass = v.cls;
    }
    const std::string_view titles[3] = {v.factionName, nameOf(v.raceNames, v.race),
                                        nameOf(v.classNames, v.cls)};
    const std::string_view bodies[3] = {v.factionText, v.raceText, v.classText};
    const u64 icons[3] = {kFactionIcons, kRaceIcons, kClassIcons};
    const TexCoords iconUvs[3] = {{v.alliance ? 0.0f : 0.5f, 0.0f, v.alliance ? 0.5f : 1.0f, 1.0f},
                                  raceIcon(v.race, v.gender),
                                  classIcon(v.cls)};
    for (u32 i = 0; i < 3; ++i) {
        if (infoFrame(g, l, i, l.info[i], l.infoOpen[i], v.alliance, icons[i], iconUvs[i], titles[i],
                      bodies[i], i == 1 ? v.abilityText : std::string_view{}, state.scrolls[i],
                      result.scrollButton)) {
            state.expanded = i;
        }
    }

    // The name: "Name" over the box (GlueFontNormalLarge), the box (the tooltip backdrop in the Alliance's
    // colors, CharacterCreate_OnLoad), GlueEditBoxFont inside; Randomize under it.
    centered(g, l.nameLabel, text::get("NAME"), glueFont(g, l, 18.0f, 16.0f, kGlueGold));
    {
        const Rect& box = l.name;
        const ui::WidgetState field = ui::hit(g, kNameField, touchTarget(g, box));
        backdrop(g, box, kTooltipBackground, kTooltipColor, 10.0f * k, 4.0f * k, 5.0f * k, 9.0f * k,
                 kGlueTooltipBorder, 16.0f * k, kNameBorder);
        TextStyle style;
        style.font = ui::FontId::Arial;
        style.size = std::max(18.0f * k, g.px(16.0f));
        style.color = kGlueWhite;
        const Rect inner{box.x + 15.0f * k, box.y, box.w - 22.0f * k, box.h - 5.0f * k};
        const f32 baseline = ui::centeredBaseline(g.draw, style, inner.centerY());
        g.draw.pushClip(inner);
        const f32 width = g.draw.measure(name.view(), style.font, style.size);
        const f32 x = width > inner.w - 4.0f ? inner.right() - 4.0f - width : inner.x;
        g.draw.text(x, baseline, name.view(), style);
        if (editing && (nowMs / 500u) % 2u == 0u) {
            const f32 caret = std::max(1.0f, g.px(1.5f));
            g.draw.solid({x + width + caret, inner.centerY() - style.size * 0.5f, caret, style.size},
                         kGlueWhite);
        }
        g.draw.popClip();
        if (ui::tapped(field) && !editing && v.dialog == CharCreateDialog::None) {
            text.begin(kNameField, name.view(), kNameBytes, ui::TextKind::Plain);
        }
    }
    // Phase 21: GlueLocalization.lua shows CharacterCreateRandomName in the enUS client only ("for English only").
    result.randomName = text::get("VD_CREATE_SHOW_RANDOM_NAME") != "0" &&
                        glueButton(g, l, widgetId("create.randomName"), l.randomName, text::get("RANDOMIZE"), false);

    // CharCreateOkayButton (GlueButtonTemplate) over CharCreateBackButton (GlueButtonSmallTemplate).
    result.accept =
        glueButton(g, l, widgetId("create.accept"), l.accept, text::get("CHARACTER_CREATE_ACCEPT"), true) ||
        result.accept;
    result.back = glueButton(g, l, widgetId("create.back"), l.back, text::get("BACK"), false);

    glueTooltip(g, l, tip);

    if (v.dialog != CharCreateDialog::None) {
        // Nothing under the dialog answers while it is up.
        result = CharCreateResult{};
        result.dialogButton = glueDialog(
            g, l, area, v.dialogText, text::get(v.dialog == CharCreateDialog::Creating ? "CANCEL" : "OKAY"));
    }
    if (result.back || result.accept) {
        text.end();
    }
    return result;
}

} // namespace vd
