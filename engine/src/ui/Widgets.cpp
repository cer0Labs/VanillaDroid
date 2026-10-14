#include "ui/Widgets.h"

#include "core/Hash.h"
#include "core/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vd::ui {
namespace {

// Client textures, with the FrameXML template that uses them.
constexpr u64 kDialogBackground = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Background.blp");
constexpr u64 kDialogBorder = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Border.blp");
constexpr u64 kDialogHeader = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Header.blp");
constexpr u64 kTooltipBackground = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Background.blp");
constexpr u64 kTooltipBorder = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Border.blp");
constexpr u64 kParchmentTopLeft = assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-TopLeft.blp");
constexpr u64 kParchmentTopRight = assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-TopRight.blp");
constexpr u64 kParchmentBotLeft = assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-BotLeft.blp");
constexpr u64 kParchmentBotRight = assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-BotRight.blp");
constexpr u64 kHorizontalBreak = assetPathHash("Interface\\QuestFrame\\UI-HorizontalBreak.blp");
// UIPanelButtonTemplate
constexpr u64 kButtonUp = assetPathHash("Interface\\Buttons\\UI-Panel-Button-Up.blp");
constexpr u64 kButtonDown = assetPathHash("Interface\\Buttons\\UI-Panel-Button-Down.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\UI-Panel-Button-Highlight.blp");
constexpr u64 kButtonDisabled = assetPathHash("Interface\\Buttons\\UI-Panel-Button-Disabled.blp");
// UIPanelCloseButton
constexpr u64 kCloseUp = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Up.blp");
constexpr u64 kCloseDown = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Down.blp");
constexpr u64 kCloseHighlight = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight.blp");
// UICheckButtonTemplate
constexpr u64 kCheckUp = assetPathHash("Interface\\Buttons\\UI-CheckBox-Up.blp");
constexpr u64 kCheckDown = assetPathHash("Interface\\Buttons\\UI-CheckBox-Down.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\UI-CheckBox-Highlight.blp");
constexpr u64 kCheckMark = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
// OptionsSliderTemplate
constexpr u64 kSliderBackground = assetPathHash("Interface\\Buttons\\UI-SliderBar-Background.blp");
constexpr u64 kSliderBorder = assetPathHash("Interface\\Buttons\\UI-SliderBar-Border.blp");
constexpr u64 kSliderThumb = assetPathHash("Interface\\Buttons\\UI-SliderBar-Button-Horizontal.blp");
// QuestLogTitleButtonTemplate's expand / collapse
constexpr u64 kPlusUp = assetPathHash("Interface\\Buttons\\UI-PlusButton-Up.blp");
constexpr u64 kPlusDown = assetPathHash("Interface\\Buttons\\UI-PlusButton-Down.blp");
constexpr u64 kPlusDisabled = assetPathHash("Interface\\Buttons\\UI-PlusButton-Disabled.blp");
constexpr u64 kMinusUp = assetPathHash("Interface\\Buttons\\UI-MinusButton-Up.blp");
constexpr u64 kMinusDown = assetPathHash("Interface\\Buttons\\UI-MinusButton-Down.blp");
constexpr u64 kMinusDisabled = assetPathHash("Interface\\Buttons\\UI-MinusButton-Disabled.blp");
constexpr u64 kPlusMinusHighlight = assetPathHash("Interface\\Buttons\\UI-PlusButton-Hilight.blp");
// Row highlights
constexpr u64 kQuestTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr u64 kQuestLogTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
// ItemButtonTemplate
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kQuickslotDown = assetPathHash("Interface\\Buttons\\UI-Quickslot-Depress.blp");
constexpr u64 kSlotHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kEmptySlot = assetPathHash("Interface\\Buttons\\UI-EmptySlot.blp");
// Retouches (windows): the icon the client shows for one it does not know (in the atlas: always at hand)
constexpr u64 kQuestionMark = assetPathHash("Interface\\Icons\\INV_Misc_QuestionMark.blp");
// Two vertex colours one over the other (SetVertexColor on a texture already tinted): channel by channel.
constexpr u32 multiplyColor(u32 a, u32 b) {
    u32 out = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        out |= (((a >> shift) & 255u) * ((b >> shift) & 255u) + 127u) / 255u << shift;
    }
    return out;
}
// UIPanelScrollBarTemplate
constexpr u64 kScrollUpUp = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Up.blp");
constexpr u64 kScrollUpDown = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Down.blp");
constexpr u64 kScrollUpDisabled = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Disabled.blp");
constexpr u64 kScrollUpHighlight = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Highlight.blp");
constexpr u64 kScrollDownUp = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Up.blp");
constexpr u64 kScrollDownDown = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Down.blp");
constexpr u64 kScrollDownDisabled = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Disabled.blp");
constexpr u64 kScrollDownHighlight =
    assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Highlight.blp");
constexpr u64 kScrollKnob = assetPathHash("Interface\\Buttons\\UI-ScrollBar-Knob.blp");
// MoneyFrameTemplate, InputBoxTemplate
constexpr u64 kMoneyIcons = assetPathHash("Interface\\MoneyFrame\\UI-MoneyIcons.blp");
constexpr u64 kInputBorder = assetPathHash("Interface\\Common\\Common-Input-Border.blp");

constexpr f32 kTapSlopDp = 10.0f;          // as Context: a finger that moves less is still a tap
constexpr f32 kFlingDecayPerSecond = 3.0f; // velocity *= e^(-3 t): a fling travels velocity / 3
constexpr f32 kMinFlingDp = 40.0f;         // slower releases just stop
constexpr f32 kCaretBlinkMs = 530.0f;
constexpr u64 kForgetScrollMs = 500;          // a view not drawn for this long starts over

void addSample(ScrollState& s, u64 ms, f32 y) {
    s.samples[s.sampleCount % ScrollState::kSamples] = {ms, y};
    ++s.sampleCount;
}

// The dragging finger's speed (pixels per second, content moving up = positive) over its last samples.
f32 fingerVelocity(const ScrollState& s, u64 nowMs) {
    const u32 count = std::min(s.sampleCount, ScrollState::kSamples);
    if (count < 2) {
        return 0.0f;
    }
    const ScrollState::Sample& newest = s.samples[(s.sampleCount - 1) % ScrollState::kSamples];
    const ScrollState::Sample* oldest = &newest;
    for (u32 i = 2; i <= count; ++i) {
        const ScrollState::Sample& sample = s.samples[(s.sampleCount - i) % ScrollState::kSamples];
        if (nowMs - sample.ms > ScrollState::kVelocityWindowMs) {
            break;
        }
        oldest = &sample;
    }
    const u64 span = newest.ms - oldest->ms;
    return span > 0 ? -(newest.y - oldest->y) * 1000.0f / static_cast<f32>(span) : 0.0f;
}

// Scroll views drawn recently, least recently used replaced (game thread only).
constexpr u32 kMaxScrollViews = 32;
std::array<ScrollState, kMaxScrollViews> g_scrolls{};

ScrollState& scrollState(WidgetId id) {
    ScrollState* oldest = &g_scrolls[0];
    for (ScrollState& s : g_scrolls) {
        if (s.id == id) {
            return s;
        }
        if (s.lastMs < oldest->lastMs) {
            oldest = &s; // free entries (lastMs 0) first
        }
    }
    *oldest = ScrollState{};
    oldest->id = id;
    return *oldest;
}

Rect intersect(const Rect& a, const Rect& b) {
    const f32 x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const f32 x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

// `r` grown (around its center) to at least a finger's size.
Rect touchRect(const Gui& g, const Rect& r) {
    const f32 min = g.px(kTouchDp);
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

} // namespace

void threeSlice(DrawList& draw, const Rect& r, u64 hash, TexCoords uv, f32 capU, f32 capWidth, u32 color) {
    const std::optional<Sprite> s = draw.assets()->sprite(hash);
    if (!s) {
        return;
    }
    capWidth = std::min(capWidth, r.w * 0.5f);
    draw.sprite({r.x, r.y, capWidth, r.h}, *s, color, {uv.u0, uv.v0, uv.u0 + capU, uv.v1});
    draw.sprite({r.x + capWidth, r.y, r.w - 2.0f * capWidth, r.h}, *s, color,
                {uv.u0 + capU, uv.v0, uv.u1 - capU, uv.v1});
    draw.sprite({r.right() - capWidth, r.y, capWidth, r.h}, *s, color, {uv.u1 - capU, uv.v0, uv.u1, uv.v1});
}

u32 qualityColor(u32 quality) {
    constexpr u32 kColors[] = {rgba(157, 157, 157), rgba(255, 255, 255), rgba(30, 255, 0),
                               rgba(0, 112, 221),   rgba(163, 53, 238),  rgba(255, 128, 0),
                               rgba(230, 204, 128)};
    return kColors[std::min<u32>(quality, 6)];
}

u32 questDifficultyColor(u32 questLevel, u32 playerLevel) {
    if (playerLevel == 0) {
        return kGold;
    }
    const i32 diff = static_cast<i32>(questLevel) - static_cast<i32>(playerLevel);
    if (diff >= 5) {
        return rgba(255, 26, 26); // impossible
    }
    if (diff >= 3) {
        return rgba(255, 128, 64); // very difficult
    }
    if (diff >= -2) {
        return rgba(255, 255, 0); // difficult
    }
    // Below the gray level (no experience) quests are trivial.
    const i32 level = static_cast<i32>(playerLevel);
    const i32 gray = level <= 5    ? 0
                     : level <= 39 ? level - level / 10 - 5
                     : level <= 59 ? level - level / 5 - 1
                                   : level - 9;
    return static_cast<i32>(questLevel) > gray ? rgba(64, 191, 64) : rgba(128, 128, 128);
}

// --- Text --------------------------------------------------------------------------------------------------

TextStyle gameFont(f32 size, u32 color) {
    TextStyle style;
    style.font = FontId::Friz;
    style.size = size;
    style.color = color;
    style.shadow = std::max(1.0f, size / 12.0f); // MasterFont: 1 unit at 12
    return style;
}

TextStyle numberFont(f32 size, u32 color) {
    TextStyle style;
    style.font = FontId::Arial;
    style.size = size;
    style.color = color;
    style.outline = std::max(1.0f, size / 14.0f);
    return style;
}

TextStyle chatFont(f32 size, u32 color) {
    TextStyle style;
    style.font = FontId::Arial;
    style.size = size;
    style.color = color;
    style.shadow = std::max(1.0f, size / 14.0f);
    return style;
}

TextStyle questFont(f32 size, u32 color) {
    TextStyle style;
    style.font = FontId::Friz;
    style.size = size;
    style.color = color;
    return style;
}

f32 centeredBaseline(const DrawList& draw, const TextStyle& style, f32 centerY) {
    return centerY + draw.ascent(style.font, style.size) - style.size * 0.5f;
}

f32 fontStringBaseline(const DrawList& draw, FontId font, f32 fontHeight, f32 centerY) {
    // DrawList::ascent is the font's ascender over ascender + |descender| of a line that tall.
    return centerY - fontHeight * 0.5f + draw.ascent(font, fontHeight);
}

void label(Gui& g, const Rect& r, std::string_view text, const TextStyle& style) {
    if (text.empty() || r.w <= 0.0f) {
        return;
    }
    const f32 baseline = centeredBaseline(g.draw, style, r.centerY());
    const f32 width = g.draw.measure(text, style.font, style.size);
    if (width <= r.w) {
        const f32 x = style.align == Align::Left     ? r.x
                      : style.align == Align::Center ? r.centerX()
                                                     : r.right();
        g.draw.text(x, baseline, text, style);
        return;
    }
    // Too wide: the longest prefix (whole UTF-8 characters) that fits with "...".
    const f32 dots = g.draw.measure("...", style.font, style.size);
    usize low = 0, high = text.size();
    while (low < high) {
        usize mid = (low + high + 1) / 2;
        while (mid > 0 && mid < text.size() && (static_cast<u8>(text[mid]) & 0xC0) == 0x80) {
            --mid; // not in the middle of a character
        }
        if (mid <= low) {
            break;
        }
        if (g.draw.measure(text.substr(0, mid), style.font, style.size) + dots <= r.w) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    TextStyle left = style;
    left.align = Align::Left;
    const f32 drawn = g.draw.text(r.x, baseline, text.substr(0, low), left);
    g.draw.text(r.x + drawn, baseline, "...", left);
}

f32 parchmentTitle(Gui& g, const Rect& r, std::string_view text, f32 size) {
    TextStyle style;
    style.font = FontId::Morpheus;
    style.size = size;
    const f32 shadow = std::max(1.0f, size / 18.0f);
    style.color = kParchmentTitleShadow;
    g.draw.wrappedText({r.x + shadow, r.y + shadow, r.w, r.h}, text, style);
    style.color = kParchmentTitle;
    return g.draw.wrappedText(r, text, style);
}

// --- Panels ------------------------------------------------------------------------------------------------

void dialogPanel(Gui& g, const Rect& r) {
    // Backdrop of StaticPopupTemplate / GameMenuFrame: background insets 11/12/12/11, edge 32. The client
    // tiles the background, a uniform black at 60%: one stretched quad looks the same.
    const Rect inside{r.x + g.unit(11.0f), r.y + g.unit(12.0f), r.w - g.unit(23.0f), r.h - g.unit(23.0f)};
    g.draw.sprite(inside, kDialogBackground);
    g.draw.backdropBorder(r, kDialogBorder, g.unit(32.0f));
}

Rect dialogContent(const Gui& g, const Rect& panel) {
    return panel.inset(g.unit(16.0f));
}

f32 dialogHeader(Gui& g, const Rect& panel, std::string_view title) {
    // UI-DialogBox-Header: the plate is texels 58..197 x 0..40 of 256 x 64, placed 12 units above the panel's
    // top (GameMenuFrameHeader). Its ornamented ends are kept; the middle stretches for long titles.
    TextStyle style = gameFont(g.px(15.0f));
    style.align = Align::Center;
    const f32 s = g.unit(1.0f);
    const f32 textWidth = g.draw.measure(title, style.font, style.size);
    const f32 width = std::min(panel.w - 2.0f * g.px(46.0f), std::max(139.0f * s, textWidth + 44.0f * s));
    const Rect plate{panel.centerX() - width * 0.5f, panel.y - 12.0f * s, width, 40.0f * s};
    threeSlice(g.draw, plate, kDialogHeader, {58.0f / 256.0f, 0.0f, 197.0f / 256.0f, 40.0f / 64.0f},
               24.0f / 256.0f, 24.0f * s);
    label(g, {plate.x + 18.0f * s, plate.y, plate.w - 36.0f * s, 39.0f * s}, title, style);
    return plate.bottom();
}

void tooltipPanel(Gui& g, const Rect& r, u32 background) {
    // GameTooltipTemplate: background inset 5 (a uniform gray, tinted), edge 16.
    g.draw.sprite(r.inset(g.unit(5.0f)), kTooltipBackground, background);
    g.draw.backdropBorder(r, kTooltipBorder, g.unit(16.0f));
}

void parchment(Gui& g, const Rect& r) {
    // The parchment of QuestFramePanelTemplate is texels (24, 84)..(322, 414) of the 384 x 512 frame its four
    // textures make (split at 256). Its torn right and bottom edges keep their size; the rest stretches.
    const f32 s = g.unit(1.0f);
    const f32 rightW = std::min(66.0f * s, r.w * 0.4f);
    const f32 bottomH = std::min(158.0f * s, r.h * 0.5f);
    const f32 splitX = r.right() - rightW, splitY = r.bottom() - bottomH;
    constexpr f32 kLeftU = 24.0f / 256.0f, kTopV = 84.0f / 256.0f, kRightU = 66.0f / 128.0f,
                  kBottomV = 158.0f / 256.0f;
    g.draw.sprite({r.x, r.y, splitX - r.x, splitY - r.y}, kParchmentTopLeft, kWhite,
                  {kLeftU, kTopV, 1.0f, 1.0f});
    g.draw.sprite({splitX, r.y, rightW, splitY - r.y}, kParchmentTopRight, kWhite,
                  {0.0f, kTopV, kRightU, 1.0f});
    g.draw.sprite({r.x, splitY, splitX - r.x, bottomH}, kParchmentBotLeft, kWhite,
                  {kLeftU, 0.0f, 1.0f, kBottomV});
    g.draw.sprite({splitX, splitY, rightW, bottomH}, kParchmentBotRight, kWhite,
                  {0.0f, 0.0f, kRightU, kBottomV});
}

void horizontalBreak(Gui& g, f32 x, f32 centerY, f32 width) {
    // UI-HorizontalBreak: the ornament is texels 23..232 x 7..25; its curled ends keep their size.
    const f32 s = g.unit(1.0f);
    const Rect r{x, centerY - 9.0f * s, width, 18.0f * s};
    threeSlice(g.draw, r, kHorizontalBreak, {23.0f / 256.0f, 7.0f / 32.0f, 232.0f / 256.0f, 25.0f / 32.0f},
               60.0f / 256.0f, 60.0f * s);
}

bool modalBlocker(Gui& g, WidgetId id, f32 dim) {
    const Rect screen{0.0f, 0.0f, g.ui.width(), g.ui.height()};
    const WidgetState state = g.ui.widget(id, screen);
    if (dim > 0.0f) {
        g.draw.solid(screen, rgba(0, 0, 0, static_cast<u8>(std::clamp(dim, 0.0f, 1.0f) * 255.0f)));
    }
    return state.clicked;
}

// --- Buttons and controls ----------------------------------------------------------------------------------

WidgetState hit(Gui& g, WidgetId id, const Rect& r) {
    const Rect clip = g.draw.clip();
    return g.ui.widget(id, clip.w > 0.0f ? intersect(r, clip) : r);
}

bool tapped(const WidgetState& state) {
    return state.clicked && !scrollClaimed(state.touch);
}

bool held(const WidgetState& state, const Rect& r) {
    return state.pressed && state.touch && !scrollClaimed(state.touch) &&
           r.contains(state.touch->x, state.touch->y);
}

bool button(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool enabled) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hit(g, id, target);
    const bool down = enabled && held(state, target);
    // The art is texels 0..80 x 0..22 of 128 x 32, with 12-texel caps (UIPanelButtonTemplate2).
    constexpr TexCoords kUv{0.0f, 0.0f, 0.625f, 0.6875f};
    const f32 cap = r.h * 12.0f / 22.0f;
    threeSlice(g.draw, r, !enabled ? kButtonDisabled : down ? kButtonDown : kButtonUp, kUv, 0.09375f, cap);
    if (down) {
        threeSlice(g.draw, r, kButtonHighlight, kUv, 0.09375f, cap);
    }
    TextStyle style = gameFont(buttonFontSize(g, r.h), !enabled ? kDisabledText
                                                                   : down   ? kHighlight
                                                                            : kGold);
    style.align = Align::Center;
    const f32 push = down ? std::max(1.0f, g.px(1.0f)) : 0.0f; // PushedTextOffset
    label(g, {r.x + cap * 0.5f + push, r.y + push, r.w - cap, r.h}, text, style);
    return enabled && tapped(state);
}

f32 buttonFontSize(const Gui& g, f32 height) {
    return std::min(g.px(15.0f), height * (12.0f / 22.0f));
}

bool closeButton(Gui& g, WidgetId id, const Rect& r) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hit(g, id, target);
    const bool down = held(state, target);
    g.draw.sprite(r, down ? kCloseDown : kCloseUp);
    if (down) {
        g.draw.sprite(r, kCloseHighlight);
    }
    return tapped(state);
}

bool plusMinusButton(Gui& g, WidgetId id, const Rect& r, bool plus, bool enabled) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hit(g, id, target);
    const bool down = enabled && held(state, target);
    const u64 art = !enabled ? (plus ? kPlusDisabled : kMinusDisabled)
                    : down   ? (plus ? kPlusDown : kMinusDown)
                             : (plus ? kPlusUp : kMinusUp);
    g.draw.sprite(r, art);
    if (down) {
        g.draw.sprite(r, kPlusMinusHighlight);
    }
    return enabled && tapped(state);
}

bool checkbox(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool& checked) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hit(g, id, target);
    const bool down = held(state, target);
    // The box is texels 4..27 of the 32-texel art: sized so it shows ~26 dp.
    const f32 art = std::min(r.h, g.px(26.0f)) * 32.0f / 23.0f;
    const Rect box{r.x - art * 4.0f / 32.0f, r.centerY() - art * 0.5f, art, art};
    g.draw.sprite(box, down ? kCheckDown : kCheckUp);
    if (checked) {
        g.draw.sprite(box, kCheckMark);
    }
    if (down) {
        g.draw.sprite(box, kCheckHighlight);
    }
    const f32 textX = box.x + art * 30.0f / 32.0f;
    label(g, {textX, r.y, r.right() - textX, r.h}, text, gameFont(g.px(15.0f), down ? kHighlight : kGold));
    if (tapped(state)) {
        checked = !checked;
        return true;
    }
    return false;
}

bool slider(Gui& g, WidgetId id, const Rect& r, f32& value, f32 min, f32 max, f32 step) {
    const Rect target = touchRect(g, r);
    const WidgetState state = hit(g, id, target);
    // OptionsSliderTemplate: a 17-unit bar (background inset 3/6, 8-unit border) and a 32-unit thumb whose
    // knob is texels 9..21 wide; the bar a third larger than the client, the thumb twice, for a finger.
    const f32 k = g.unit(1.0f) * 1.35f;
    const f32 thumbHalf = 16.0f * k * 1.5f, knobHalf = 6.0f * k * 1.5f;
    const Rect bar{r.x, r.centerY() - 8.5f * k, r.w, 17.0f * k};
    g.draw.sprite({bar.x + 3.0f * k, bar.y + 6.0f * k, bar.w - 6.0f * k, bar.h - 12.0f * k},
                  kSliderBackground);
    g.draw.backdropBorder(bar, kSliderBorder, 8.0f * k);
    const f32 range = max - min;
    const f32 left = r.x + knobHalf, travel = std::max(1.0f, r.w - 2.0f * knobHalf);
    bool changed = false;
    if (state.pressed && state.touch && !scrollClaimed(state.touch) && range > 0.0f) {
        f32 t = std::clamp((state.touch->x - left) / travel, 0.0f, 1.0f);
        f32 next = min + t * range;
        if (step > 0.0f) {
            next = std::clamp(min + std::round((next - min) / step) * step, min, max);
        }
        changed = next != value;
        value = next;
    }
    const f32 t = range > 0.0f ? std::clamp((value - min) / range, 0.0f, 1.0f) : 0.0f;
    const f32 cx = left + t * travel;
    g.draw.sprite({cx - thumbHalf, r.centerY() - thumbHalf, thumbHalf * 2.0f, thumbHalf * 2.0f}, kSliderThumb,
                  state.pressed ? kWhite : rgba(230, 230, 230));
    return changed;
}

bool textField(Gui& g, WidgetId id, const Rect& r, std::string_view text, std::string_view placeholder,
               bool focused) {
    const WidgetState state = hit(g, id, touchRect(g, r));
    // Common-Input-Border: texels 0..128 x 0..20 of 128 x 32, 8-texel caps (InputBoxTemplate).
    const f32 cap = r.h * 8.0f / 20.0f;
    threeSlice(g.draw, r, kInputBorder, {0.0f, 0.0f, 1.0f, 0.625f}, 0.0625f, cap);
    const TextStyle style = chatFont(std::min(g.px(16.0f), r.h * 0.5f));
    const Rect inner{r.x + cap * 0.75f, r.y, r.w - cap * 1.5f, r.h};
    const f32 caretWidth = std::max(1.0f, g.px(1.5f));
    const f32 baseline = centeredBaseline(g.draw, style, r.centerY());
    g.draw.pushClip(inner);
    f32 end = inner.x;
    if (text.empty()) {
        TextStyle hint = style;
        hint.color = kDisabledText;
        hint.shadow = 0.0f;
        g.draw.text(inner.x, baseline, placeholder, hint);
    } else {
        const f32 width = g.draw.measure(text, style.font, style.size);
        const f32 x =
            width > inner.w - caretWidth * 2.0f ? inner.right() - caretWidth * 2.0f - width : inner.x;
        g.draw.text(x, baseline, text, style);
        end = x + width;
    }
    if (focused && static_cast<u64>(static_cast<f32>(g.ui.nowMs()) / kCaretBlinkMs) % 2 == 0) {
        g.draw.solid({end + caretWidth * 0.5f, r.centerY() - style.size * 0.5f, caretWidth, style.size},
                     kWhite);
    }
    g.draw.popClip();
    return tapped(state);
}

// --- Scroll view -------------------------------------------------------------------------------------------

void scrollStep(ScrollState& s, std::span<const Touch> touches, const Rect& viewport, f32 dp, u64 nowMs) {
    const f32 dt = s.lastMs == 0 || nowMs <= s.lastMs
                       ? 1.0f / 60.0f
                       : std::min(0.1f, static_cast<f32>(nowMs - s.lastMs) / 1000.0f);
    s.lastMs = nowMs;
    s.viewport = viewport.h;

    // The finger that has it, while it lasts (kept through the frame it lifts, so that it taps nothing).
    const Touch* finger = nullptr;
    if (s.touch >= 0) {
        for (const Touch& t : touches) {
            if (t.id == s.touch && t.downMs == s.touchDownMs) {
                finger = &t;
            }
        }
        if (!finger) {
            s.touch = -1;
        }
    }
    if (!finger) {
        const f32 slop = kTapSlopDp * dp;
        for (const Touch& t : touches) {
            // A finger held still until its long press is picking something up (an item to drag), not scrolling.
            if (t.released || (t.longPressed && !s.longPressScrolls) || !viewport.contains(t.startX, t.startY) ||
                scrollClaimed(&t)) {
                continue;
            }
            const f32 dx = t.x - t.startX, dy = t.y - t.startY;
            const bool catchFling = t.began && std::abs(s.velocity) > kMinFlingDp * dp;
            if (catchFling || (std::abs(dy) > slop && std::abs(dy) > std::abs(dx))) {
                s.touch = t.id;
                s.touchDownMs = t.downMs;
                s.velocity = 0.0f;
                s.sampleCount = 0;
                addSample(s, t.downMs, t.startY); // a quick flick is measured from where it began
                finger = &t;
                break;
            }
        }
    }

    const bool dragging = finger && !finger->released;
    if (finger) {
        s.offset -= finger->dy;
        addSample(s, nowMs, finger->y);
        // Its speed over the last 100 ms (a finger held still before lifting leaves nothing to fling).
        s.velocity = fingerVelocity(s, nowMs);
        if (!dragging && std::abs(s.velocity) < kMinFlingDp * dp) {
            s.velocity = 0.0f;
        }
    } else if (s.velocity != 0.0f) {
        s.offset += s.velocity * dt;
        s.velocity *= std::exp(-kFlingDecayPerSecond * dt);
        if (std::abs(s.velocity) < kMinFlingDp * dp * 0.25f) {
            s.velocity = 0.0f;
        }
    }
    const f32 maxOffset = s.maxOffset();
    if (s.offset <= 0.0f || s.offset >= maxOffset) {
        s.offset = std::clamp(s.offset, 0.0f, maxOffset);
        if (!dragging) {
            s.velocity = 0.0f; // a fling stops at the edge
        }
    }
    s.atEnd = s.offset >= maxOffset - 0.5f;
}

void scrollSetContent(ScrollState& s, f32 contentHeight, f32 viewportHeight) {
    const bool follow = s.stickToEnd && s.atEnd && s.touch < 0;
    s.content = std::max(0.0f, contentHeight);
    s.viewport = viewportHeight;
    if (follow) {
        s.offset = s.maxOffset();
        s.velocity = 0.0f;
    }
    s.offset = std::clamp(s.offset, 0.0f, s.maxOffset());
    s.atEnd = s.offset >= s.maxOffset() - 0.5f;
}

ScrollView beginScroll(Gui& g, WidgetId id, const Rect& viewport, bool stickToEnd) {
    ScrollState& s = scrollState(id);
    if (s.lastMs != 0 && g.ui.nowMs() > s.lastMs + kForgetScrollMs) {
        s = ScrollState{}; // not drawn for a while (closed): it opens again at its start (or its end)
        s.id = id;
    }
    s.stickToEnd = stickToEnd;
    hit(g, id, viewport); // fingers on its empty parts are its own
    scrollStep(s, g.ui.touches(), viewport, g.ui.dp(), g.ui.nowMs());
    g.draw.pushClip(viewport);
    ScrollView view;
    view.id = id;
    view.viewport = viewport;
    view.offset = s.offset;
    view.top = viewport.y - s.offset;
    view.scrolling = s.touch >= 0 || s.velocity != 0.0f;
    return view;
}

void endScroll(Gui& g, const ScrollView& view, f32 contentHeight, bool indicator) {
    g.draw.popClip();
    ScrollState& s = scrollState(view.id);
    scrollSetContent(s, contentHeight, view.viewport.h);
    if (!indicator || s.content <= s.viewport + 1.0f) {
        return;
    }
    // A thin indicator along the right edge: brighter while it moves.
    const f32 width = std::max(2.0f, g.px(3.0f)), margin = g.px(2.0f);
    const f32 track = view.viewport.h - 2.0f * margin;
    const f32 bar = std::max(g.px(20.0f), track * s.viewport / s.content);
    const f32 y =
        view.viewport.y + margin + (track - bar) * (s.maxOffset() > 0.0f ? s.offset / s.maxOffset() : 0.0f);
    const Rect r{view.viewport.right() - width - margin, y, width, bar};
    g.draw.solid(r.inset(-1.0f), rgba(0, 0, 0, view.scrolling ? 150 : 90));
    g.draw.solid(r, rgba(255, 209, 0, view.scrolling ? 230 : 130));
}

bool panelScrollBar(Gui& g, const ScrollView& view, const Rect& bar, f32 rightLimit) {
    const f32 w = bar.w;
    if (w <= 0.0f || bar.h < 2.0f * w) {
        return false;
    }
    ScrollState& s = scrollState(view.id);
    const f32 maxOffset = s.maxOffset();
    const bool range = maxOffset >= 1.0f; // ScrollFrame_OnScrollRangeChanged: floor(scrollrange) > 0
    // A finger's size, grown to the right of the bar only (the view's content is at its left), to the left where that
    // would cross `rightLimit`.
    const auto target = [&](const Rect& r) {
        const f32 min = g.px(kTouchDp);
        Rect t = r;
        t.w = std::max(t.w, min);
        t.x = std::min(t.x, std::max(rightLimit - t.w, r.right() - t.w));
        if (t.h < min) {
            t.y -= (min - t.h) * 0.5f;
            t.h = min;
        }
        return t;
    };
    const Rect up{bar.x, bar.y, w, w}, down{bar.x, bar.bottom() - w, w, w};
    const f32 top = up.bottom(), bottom = down.y; // the slider, between the buttons
    const f32 track = std::max(0.0f, bottom - top - w);
    if (range) {
        // The slider under the buttons' targets: a finger on it takes the knob there.
        const WidgetState knob =
            hit(g, widgetId("panelScrollBar.slider", view.id), target({bar.x, top, w, bottom - top}));
        if (knob.pressed && knob.touch && track > 0.0f) {
            s.offset = std::clamp((knob.touch->y - top - 0.5f * w) / track, 0.0f, 1.0f) * maxOffset;
            s.velocity = 0.0f;
            s.atEnd = s.offset >= maxOffset - 0.5f;
        }
    }
    const Rect upTarget = target(up), downTarget = target(down);
    const WidgetState upState = hit(g, widgetId("panelScrollBar.up", view.id), upTarget);
    const WidgetState downState = hit(g, widgetId("panelScrollBar.down", view.id), downTarget);
    const bool atTop = !range || s.offset <= 0.5f, atEnd = !range || s.offset >= maxOffset - 0.5f;
    const f32 half = (bottom - top) * 0.5f; // parent:GetHeight() / 2
    bool scrolled = false;
    if (!atTop && tapped(upState)) {
        s.offset = std::max(0.0f, s.offset - half);
        scrolled = true;
    }
    if (!atEnd && tapped(downState)) {
        s.offset = std::min(maxOffset, s.offset + half);
        scrolled = true;
    }
    if (scrolled) {
        s.velocity = 0.0f;
        s.atEnd = s.offset >= maxOffset - 0.5f;
    }
    // The art: UIPanelScrollBarButton's middle of each texture (0.25..0.75), the knob where the offset now is.
    constexpr TexCoords kUv{0.25f, 0.25f, 0.75f, 0.75f};
    const bool upDown = !atTop && held(upState, upTarget), downDown = !atEnd && held(downState, downTarget);
    g.draw.sprite(up, atTop ? kScrollUpDisabled : upDown ? kScrollUpDown : kScrollUpUp, kWhite, kUv);
    if (upDown) {
        g.draw.sprite(up, kScrollUpHighlight, kWhite, kUv);
    }
    g.draw.sprite(down, atEnd ? kScrollDownDisabled : downDown ? kScrollDownDown : kScrollDownUp, kWhite, kUv);
    if (downDown) {
        g.draw.sprite(down, kScrollDownHighlight, kWhite, kUv);
    }
    if (range) {
        const f32 knobY = top + track * std::clamp(s.offset / maxOffset, 0.0f, 1.0f);
        g.draw.sprite({bar.x, knobY, w, w}, kScrollKnob, kWhite, kUv);
    }
    return scrolled;
}

bool scrollClaimed(const Touch* touch) {
    if (!touch) {
        return false;
    }
    for (const ScrollState& s : g_scrolls) {
        if (s.id != 0 && s.touch == touch->id && s.touchDownMs == touch->downMs) {
            return true;
        }
    }
    return false;
}

void scrollTo(WidgetId id, f32 offset) {
    ScrollState& s = scrollState(id);
    s.offset = std::clamp(offset, 0.0f, s.maxOffset());
    s.velocity = 0.0f;
    s.atEnd = s.offset >= s.maxOffset() - 0.5f;
}

// --- Lists, items, money -----------------------------------------------------------------------------------

bool listRow(Gui& g, WidgetId id, const Rect& r, std::string_view text, const RowStyle& style) {
    const WidgetState state = hit(g, id, r);
    const bool down = held(state, r);
    if (down || style.selected) {
        switch (style.highlight) {
        case RowHighlight::Quest:
            g.draw.sprite(r, kQuestTitleHighlight, rgba(255, 255, 255, down ? 255 : 200));
            break;
        case RowHighlight::QuestLog: g.draw.sprite(r, kQuestLogTitleHighlight, style.text.color); break;
        case RowHighlight::Listbox:
            g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, down ? 255 : 200));
            break;
        }
    }
    f32 x = r.x + g.px(4.0f);
    if (style.icon != 0) {
        const f32 icon = style.iconSize > 0.0f ? style.iconSize : style.text.size * 1.15f;
        g.draw.sprite({x, r.centerY() - icon * 0.5f, icon, icon}, style.icon);
        x += icon + g.px(6.0f);
    }
    label(g, {x, r.y, r.right() - x - g.px(4.0f), r.h}, text, style.text);
    return tapped(state);
}

WidgetState itemSlot(Gui& g, WidgetId id, const Rect& r, const SlotView& slot) {
    WidgetState state = hit(g, id, r);
    const bool down = held(state, r);
    state.clicked = tapped(state);
    // ItemButtonTemplate: a 37-unit icon inside the 64-unit UI-Quickslot2 frame (centered, one unit lower).
    const f32 scale = r.w / 37.0f;
    const Rect frame{r.centerX() - 32.0f * scale, r.centerY() - 32.0f * scale + scale, 64.0f * scale,
                     64.0f * scale};
    if (slot.empty) {
        if (slot.emptyArt) {
            g.draw.sprite(frame, kEmptySlot);
        }
    } else {
        // Retouches (windows): the slot's own tint (SetItemButtonTextureVertexColor) under the usable /
        // pressed one; the question mark while the icon loads; no quality border (1.12 shows the quality in
        // names only).
        const u32 tint = multiplyColor(!slot.usable ? rgba(230, 60, 60)
                                       : down       ? rgba(170, 170, 170)
                                                    : kWhite,
                                       slot.tint);
        iconOrPlaceholder(g.draw, r, slot.icon, tint, {0.0f, 0.0f, 1.0f, 1.0f}, slot.desaturated);
        g.draw.models().cooldown(g.draw, id, r, scale, slot.cooldown, -1.0f, g.ui.nowMs()); // retouches (ui models)
    }
    g.draw.sprite(frame, kQuickslot, slot.empty ? kWhite : slot.tint);
    if (down) {
        g.draw.sprite(r, kQuickslotDown);
    }
    if (down || slot.selected) {
        g.draw.sprite(r, kSlotHighlight);
    }
    if (slot.count > 1 && !slot.empty) {
        char text[12];
        std::snprintf(text, sizeof(text), "%u", slot.count);
        TextStyle style = numberFont(std::max(g.px(12.0f), r.h * 0.34f));
        style.align = Align::Right;
        g.draw.text(r.right() - r.w * 0.08f, r.bottom() - r.h * 0.08f, text, style);
    }
    return state;
}

void iconOrPlaceholder(DrawList& draw, const Rect& r, std::optional<u32> icon, u32 color, TexCoords uv,
                       bool desaturated) {
    if (icon) {
        if (desaturated) {
            draw.iconDesaturated(r, *icon, color, uv);
        } else {
            draw.icon(r, *icon, color, uv);
        }
    } else if (desaturated) {
        draw.spriteDesaturated(r, kQuestionMark, color, uv);
    } else {
        draw.sprite(r, kQuestionMark, color, uv);
    }
}

CoinsShown coinsShown(u32 copper) {
    const Coins c = splitMoney(copper);
    CoinsShown shown;
    shown.gold = c.gold > 0;
    shown.silver = c.silver > 0 || (c.gold > 0 && c.copper > 0);
    shown.copper = c.copper > 0 || copper == 0;
    return shown;
}

usize formatMoney(char* out, usize capacity, u32 copper) {
    if (capacity == 0) {
        return 0;
    }
    const Coins c = splitMoney(copper);
    const CoinsShown shown = coinsShown(copper);
    usize length = 0;
    // Each denomination as "12g" (VD_MONEY_GOLD / _SILVER / _COPPER: the letter is the language's).
    auto put = [&](bool show, u32 amount, text::Key key) {
        if (!show || length + 1 >= capacity) {
            return;
        }
        if (length > 0) {
            out[length++] = ' ';
        }
        length += text::format({out + length, capacity - length}, key, amount).size();
    };
    out[0] = '\0';
    put(shown.gold, c.gold, "VD_MONEY_GOLD");
    put(shown.silver, c.silver, "VD_MONEY_SILVER");
    put(shown.copper, c.copper, "VD_MONEY_COPPER");
    out[std::min(length, capacity - 1)] = '\0';
    return std::min(length, capacity - 1);
}

namespace {

// Walks the shown denominations of `copper`: (amount text, coin index 0 gold / 1 silver / 2 copper).
template <typename F> void forEachCoin(u32 copper, F&& f) {
    const Coins c = splitMoney(copper);
    const CoinsShown shown = coinsShown(copper);
    const bool show[3] = {shown.gold, shown.silver, shown.copper};
    const u32 amount[3] = {c.gold, c.silver, c.copper};
    for (u32 i = 0; i < 3; ++i) {
        if (show[i]) {
            char text[12];
            std::snprintf(text, sizeof(text), "%u", amount[i]);
            f(std::string_view(text), i);
        }
    }
}

// MoneyFrame proportions: a 13-unit coin after a 14-unit number, 4 units between denominations.
constexpr f32 kCoinScale = 13.0f / 14.0f;
constexpr f32 kCoinGapScale = 4.0f / 14.0f;

} // namespace

f32 moneyWidth(const DrawList& draw, u32 copper, f32 size) {
    f32 width = 0.0f;
    bool first = true;
    forEachCoin(copper, [&](std::string_view text, u32) {
        width += (first ? 0.0f : size * kCoinGapScale) + draw.measure(text, FontId::Arial, size) +
                 size * 0.1f + size * kCoinScale;
        first = false;
    });
    return width;
}

f32 money(Gui& g, f32 x, f32 centerY, u32 copper, f32 size, Align align, f32 alpha, u32 color) {
    const f32 width = moneyWidth(g.draw, copper, size);
    f32 pen = align == Align::Left ? x : align == Align::Center ? x - width * 0.5f : x - width;
    const u32 white = withAlpha(kWhite, alpha); // phase 18 (extras)
    // Retouches (windows): the numbers in SetMoneyFrameColor's colour, the coins as they are.
    const TextStyle style = numberFont(size, withAlpha(color, alpha));
    const f32 baseline = centeredBaseline(g.draw, style, centerY);
    const f32 coin = size * kCoinScale;
    bool first = true;
    forEachCoin(copper, [&](std::string_view text, u32 index) {
        pen += first ? 0.0f : size * kCoinGapScale;
        first = false;
        pen += g.draw.text(pen, baseline, text, style) + size * 0.1f;
        // UI-MoneyIcons: gold, silver, copper in quarters of its width.
        const f32 u0 = static_cast<f32>(index) * 0.25f;
        g.draw.sprite({pen, centerY - coin * 0.5f, coin, coin}, kMoneyIcons, white, {u0, 0.0f, u0 + 0.25f, 1.0f});
        pen += coin;
    });
    return width;
}

} // namespace vd::ui
