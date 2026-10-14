#include "app/WorldStateWindows.h"

#include "app/UnitMenuWindows.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using ui::Gui;
using ui::Rect;
using ui::rgba;

// --- Art
// ---------------------------------------------------------------------------------------------------------

constexpr u64 kCaptureBar = assetPathHash("Interface\\WorldStateFrame\\WorldState-CaptureBar.blp");
constexpr u64 kScoreTopLeft =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-TopLeft.blp");
constexpr u64 kScoreTop = assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-Top.blp");
constexpr u64 kScoreTopRight =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-TopRight.blp");
constexpr u64 kScoreBotLeft =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-BotLeft.blp");
constexpr u64 kScoreBot = assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-Bot.blp");
constexpr u64 kScoreBotRight =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-BotRight.blp");
constexpr u64 kScoreTopBackground =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-TopBackground.blp");
constexpr u64 kScoreHighlight =
    assetPathHash("Interface\\WorldStateFrame\\WorldStateFinalScore-Highlight.blp");
constexpr u64 kTabActive = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ActiveTab.blp");
constexpr u64 kTabInactive = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-InActiveTab.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
constexpr u64 kScrollBarArt = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ScrollBar.blp");
constexpr u64 kScrollKnob = assetPathHash("Interface\\Buttons\\UI-ScrollBar-Knob.blp");
constexpr u64 kScrollUp = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Up.blp");
constexpr u64 kScrollUpDisabled =
    assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Disabled.blp");
constexpr u64 kScrollDown = assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Up.blp");
constexpr u64 kScrollDownDisabled =
    assetPathHash("Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Disabled.blp");
constexpr u64 kMapBorder = assetPathHash("Interface\\BattlefieldFrame\\UI-BattlefieldMinimap-Border.blp");
constexpr u64 kDialogCorner = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Corner.blp");
constexpr u64 kChatTab = assetPathHash("Interface\\ChatFrame\\ChatFrameTab.blp");
constexpr u64 kMinimapArrow = assetPathHash("Interface\\Minimap\\MinimapArrow.blp");
constexpr u64 kPoiIcons = assetPathHash("Interface\\Minimap\\POIIcons.blp");
constexpr u64 kDialogBorder = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Border.blp");
constexpr u64 kDialogBackground = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Background.blp");
constexpr u64 kSliderBorder = assetPathHash("Interface\\Buttons\\UI-SliderBar-Border.blp");
constexpr u64 kSliderThumb = assetPathHash("Interface\\Buttons\\UI-SliderBar-Button-Vertical.blp");

// Colours of WorldStateFrame.lua: the Horde's (1, 0.1, 0.1), the Alliance's (0, 0.68, 0.94).
constexpr u32 kHordeColor = rgba(255, 26, 26);
constexpr u32 kAllianceColor = rgba(0, 173, 240);

// Fonts.xml sizes: GameFontNormal / GameFontHighlight 12, the Small ones 10.
constexpr f32 kFontNormal = 12.0f;
constexpr f32 kFontSmall = 10.0f;

// UIFrameFlash(frame, 0.5, 0.5, -1): the flash's alpha, in 0.5 s and out 0.5 s, for ever.
f32 flashAlpha(u64 nowMs) {
    const f32 t = static_cast<f32>(nowMs % 1000) / 1000.0f;
    return t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.w > 0.0f && b.w > 0.0f && a.x < b.right() && b.x < a.right() && a.y < b.bottom() &&
           b.y < a.bottom();
}

// A text's centre-line baseline for a box (the FontStrings' vertical centring).
f32 baseline(const ui::DrawList& draw, const ui::TextStyle& style, f32 centerY) {
    return ui::centeredBaseline(draw, style, centerY);
}

// --- WorldStateAlwaysUpFrame
// ---------------------------------------------------------------------------------

constexpr f32 kLineHeight = 24.0f; // WorldStateAlwaysUpTemplate
constexpr f32 kLineWidth = 45.0f;
constexpr f32 kBarWidth = 173.0f, kBarHeight = 26.0f; // WorldStateCaptureBarTemplate

f32 linesWidth(const ui::DrawList& draw, f32 unit, std::span<const AlwaysUpLineView> lines) {
    // From the icon's left (LEFT - 6) to the end of the widest text, or of a dynamic icon (RIGHT + 40).
    f32 widest = 0.0f;
    for (const AlwaysUpLineView& l : lines) {
        widest = std::max(widest, draw.measure(l.text, ui::FontId::Friz, kFontSmall * unit) / unit);
    }
    const f32 textRight = -6.0f + 42.0f - 12.0f + widest;
    return std::max(textRight, kLineWidth + 40.0f) + 6.0f;
}

} // namespace

WorldStateHudPlaces worldStateHudPlaces(const ui::DrawList& draw, const Rect& safe, f32 unit,
                                        const WorldStateHudAvoid& avoid,
                                        std::span<const AlwaysUpLineView> lines) {
    WorldStateHudPlaces p;
    p.unit = unit;
    // WorldStateAlwaysUpFrame TOP (-5, -15) of UIParent; AlwaysUpFrame1 TOP (-23, -20) of it.
    p.linesCenterX = safe.centerX() - 28.0f * unit;
    p.linesTop = safe.y + 35.0f * unit;
    const f32 width = linesWidth(draw, unit, lines) * unit;
    const f32 height = static_cast<f32>(std::max<usize>(lines.size(), 1)) * kLineHeight * unit;
    const f32 gap = 4.0f * unit;
    const auto block = [&] {
        // The icon overhangs the 24-unit line by 9 units above and below.
        return Rect{p.linesCenterX - (kLineWidth * 0.5f + 6.0f) * unit, p.linesTop - 9.0f * unit, width,
                    height + 18.0f * unit};
    };
    if (overlaps(block(), avoid.targetFrame)) { // right of the target frame (its portrait)
        p.linesCenterX = avoid.targetFrame.right() + gap + (kLineWidth * 0.5f + 6.0f) * unit;
    }
    for (const Rect& r : {avoid.buffs, avoid.extraBar}) { // under the buff rows and the top action bar
        if (overlaps(block(), r)) {
            p.linesTop = r.bottom() + gap + 9.0f * unit;
        }
    }
    if (avoid.cluster.w > 0.0f && block().right() > avoid.cluster.x - gap &&
        block().y < avoid.cluster.bottom()) {
        p.linesCenterX -= block().right() - (avoid.cluster.x - gap); // left of the minimap
    }
    p.lines = lines.empty() ? Rect{} : block();
    // MinimapCluster's BOTTOMRIGHT (-CONTAINER_OFFSET_X, 0): the bars hang under it.
    p.barsRight = avoid.cluster.w > 0.0f ? avoid.cluster.right() : safe.right();
    p.barsTop = avoid.cluster.w > 0.0f ? avoid.cluster.bottom() : safe.y + 192.0f * unit;
    return p;
}

f32 captureBarsHeight(f32 unit, u32 bars) {
    return static_cast<f32>(bars) * kBarHeight * unit;
}

void worldStateTooltip(DialogContext& c, const Rect& anchor, std::string_view title, std::string_view line,
                       u32 titleColor) {
    if (title.empty() && line.empty()) {
        return;
    }
    Gui g{c.ui, c.draw};
    const f32 size = c.ui.px(13.0f);
    const f32 lineSize = c.ui.px(12.0f);
    const f32 pad = c.ui.px(8.0f);
    const f32 maxText = c.ui.px(260.0f);
    const f32 titleWidth = std::min(maxText, g.draw.measure(title, ui::FontId::Friz, size));
    const f32 lineWidth =
        line.empty() ? 0.0f : std::min(maxText, g.draw.measure(line, ui::FontId::Friz, lineSize));
    const f32 textWidth = std::max(titleWidth, lineWidth);
    const f32 titleHeight =
        title.empty() ? 0.0f : g.draw.measureWrapped(textWidth + 1.0f, title, ui::FontId::Friz, size);
    const f32 lineHeight =
        line.empty() ? 0.0f : g.draw.measureWrapped(textWidth + 1.0f, line, ui::FontId::Friz, lineSize);
    const f32 w = textWidth + pad * 2.0f;
    const f32 h =
        titleHeight + lineHeight + pad * 2.0f + (title.empty() || line.empty() ? 0.0f : c.ui.px(2.0f));
    // ANCHOR_BOTTOMLEFT: under the frame, its right edge at the frame's left... here under it, inside the
    // screen.
    const Rect& safe = c.safeArea;
    const f32 x = std::clamp(anchor.x, safe.x, std::max(safe.x, safe.right() - w));
    f32 y = anchor.bottom() + c.ui.px(4.0f);
    if (y + h > safe.bottom()) {
        y = std::max(safe.y, anchor.y - h - c.ui.px(4.0f));
    }
    ui::tooltipPanel(g, {x, y, w, h});
    f32 top = y + pad;
    if (!title.empty()) {
        top += g.draw.wrappedText({x + pad, top, textWidth + 1.0f, titleHeight}, title,
                                  ui::gameFont(size, titleColor));
        top += c.ui.px(2.0f);
    }
    if (!line.empty()) {
        g.draw.wrappedText({x + pad, top, textWidth + 1.0f, lineHeight}, line,
                           ui::gameFont(lineSize, ui::kWhite));
    }
}

WorldStateHudResult worldStateHud(DialogContext& c, const WorldStateHudPlaces& p,
                                  std::span<const AlwaysUpLineView> lines,
                                  std::span<const CaptureBarView> bars, u64 nowMs) {
    VD_PROFILE_ZONE("WorldStateHud::draw");
    WorldStateHudResult result;
    Gui g{c.ui, c.draw};
    const f32 u = p.unit;
    const f32 flash = flashAlpha(nowMs);
    for (usize i = 0; i < lines.size(); ++i) {
        const AlwaysUpLineView& l = lines[i];
        const Rect frame{p.linesCenterX - kLineWidth * 0.5f * u,
                         p.linesTop + static_cast<f32>(i) * kLineHeight * u, kLineWidth * u, kLineHeight * u};
        const Rect icon{frame.x - 6.0f * u, frame.centerY() - 21.0f * u, 42.0f * u, 42.0f * u};
        if (l.icon != 0) {
            g.draw.sprite(icon, l.icon);
        }
        ui::TextStyle style = ui::gameFont(kFontSmall * u);
        const f32 textX = icon.right() - 12.0f * u;
        const f32 textWidth =
            g.draw.text(textX, baseline(g.draw, style, frame.centerY() - 10.0f * u), l.text, style);
        // The line's tooltip (frame.tooltip) on its whole width; the dynamic icon's own on it.
        const Rect lineHit{icon.x, frame.y, std::max(frame.right(), textX + textWidth) - icon.x, frame.h};
        const ui::WidgetState held =
            c.ui.widget(ui::widgetId("worldstate.line", static_cast<u32>(i)), lineHit);
        if (held.pressed && !l.tooltip.empty()) {
            result.tooltip = l.tooltip;
            result.tooltipAnchor = lineHit;
        }
        if (l.flashing && l.dynamicIcon != 0) {
            const Rect dynamic{frame.right() + 8.0f * u, frame.centerY() - 9.0f * u - 16.0f * u, 32.0f * u,
                               32.0f * u};
            g.draw.sprite(dynamic, l.dynamicIcon);
            if (l.dynamicFlash != 0) {
                g.draw.sprite(dynamic, l.dynamicFlash, ui::withAlpha(ui::kWhite, flash));
            }
            const ui::WidgetState dyn =
                c.ui.widget(ui::widgetId("worldstate.dynamic", static_cast<u32>(i)), dynamic);
            if (dyn.pressed && !l.dynamicTooltip.empty()) {
                result.tooltip = l.dynamicTooltip;
                result.tooltipAnchor = dynamic;
            }
        }
    }
    // WorldStateCaptureBar<n>, stacked under the minimap cluster.
    for (usize i = 0; i < bars.size(); ++i) {
        const CaptureBarView& b = bars[i];
        const Rect r{p.barsRight - kBarWidth * u, p.barsTop + static_cast<f32>(i) * kBarHeight * u,
                     kBarWidth * u, kBarHeight * u};
        const auto at = [&](f32 x, f32 y, f32 w, f32 h) {
            return Rect{r.x + x * u, r.y + y * u, w * u, h * u};
        };
        const f32 midY = kBarHeight * 0.5f;
        // BACKGROUND: the Alliance's (blue), the Horde's (red) and the neutral middle.
        const Rect left = at(26.0f, midY - 4.5f, 48.0f, 9.0f);
        const Rect right = at(kBarWidth - 26.0f - 48.0f, midY - 4.5f, 48.0f, 9.0f);
        g.draw.sprite(left, kCaptureBar, ui::kWhite, {0.8203125f, 0.0f, 1.0f, 0.140625f});
        g.draw.sprite(right, kCaptureBar, ui::kWhite, {0.8203125f, 0.171875f, 1.0f, 0.3125f});
        g.draw.sprite({left.right(), left.y, right.x - left.right(), left.h}, kCaptureBar, ui::kWhite,
                      {0.8203125f, 0.34375f, 1.0f, 0.484375f});
        // ARTWORK: the frame and the zones' two ticks.
        g.draw.sprite(r, kCaptureBar, ui::kWhite, {0.0f, 0.0f, 0.67578125f, 0.40625f});
        g.draw.sprite({left.right() - 1.0f * u, r.y + (midY - 4.0f) * u, 3.0f * u, 8.0f * u}, kCaptureBar,
                      ui::kWhite, {0.74609375f, 0.0f, 0.7578125f, 0.125f});
        g.draw.sprite({right.x + 1.0f * u - 3.0f * u, r.y + (midY - 4.0f) * u, 3.0f * u, 8.0f * u},
                      kCaptureBar, ui::kWhite, {0.74609375f, 0.0f, 0.7578125f, 0.125f});
        // The side that holds the point lit (beyond 60 the Alliance's, under 40 the Horde's).
        if (b.value > 60.0f) {
            g.draw.sprite(at(-1.0f, midY - 14.0f, 27.0f, 28.0f), kCaptureBar, ui::kWhite,
                          {0.0f, 0.4375f, 0.10546875f, 0.875f});
        } else if (b.value < 40.0f) {
            g.draw.sprite(at(kBarWidth + 1.0f - 27.0f, midY - 14.0f, 27.0f, 28.0f), kCaptureBar, ui::kWhite,
                          {0.0f, 0.4375f, 0.10546875f, 0.875f});
        }
        // The indicator and its arrow (CaptureBar_Update: 25 + 124 * (1 - value / 100)).
        const f32 position = 25.0f + 124.0f * (1.0f - std::clamp(b.value, 0.0f, 100.0f) / 100.0f);
        const Rect indicator = at(position - 2.5f, midY - 9.0f, 5.0f, 18.0f);
        if (b.moving < 0) {
            g.draw.sprite(
                {indicator.x + 1.0f * u - 8.0f * u, indicator.centerY() - 7.5f * u, 8.0f * u, 15.0f * u},
                kCaptureBar, ui::kWhite, {0.7265625f, 0.140625f, 0.76171875f, 0.375f});
        } else if (b.moving > 0) {
            g.draw.sprite({indicator.right() - 1.0f * u, indicator.centerY() - 7.5f * u, 8.0f * u, 15.0f * u},
                          kCaptureBar, ui::kWhite, {0.76171875f, 0.140625f, 0.7265625f, 0.375f});
        }
        g.draw.sprite(indicator, kCaptureBar, ui::kWhite, {0.77734375f, 0.0f, 0.796875f, 0.28125f});
    }
    return result;
}

// --- WorldStateScoreFrame
// ----------------------------------------------------------------------------------------

namespace {

constexpr u32 kScoreButtons = 22; // MAX_WORLDSTATE_SCORE_BUTTONS
constexpr f32 kScoreFrameHeight = 512.0f;
constexpr f32 kRowTop = 78.0f, kRowStep = 15.0f, kRowHeight = 16.0f;

// The frame's columns' centres (units from its left), from WorldStateFrame.xml's anchors: the Name header's
// right edge + 160 for Killing Blows, + 58 each for Deaths and Honorable Kills, the stat columns from HK's
// right edge (+ 38 + 30 each, the last two + 40), Bonus Honor 58 after the last of them.
struct ScoreColumns {
    f32 kb = 0.0f, deaths = 0.0f, hk = 0.0f, honor = 0.0f;
    std::array<f32, 7> stat{};
    f32 headerY = 0.0f; // the headers' vertical centre
};

ScoreColumns scoreColumns(f32 nameRight, u32 stats) {
    ScoreColumns c;
    c.headerY = 48.0f + kFontSmall * 0.5f;
    c.kb = nameRight + 160.0f;
    c.deaths = c.kb + 58.0f;
    c.hk = c.deaths + 58.0f;
    f32 last = c.hk;
    f32 right = c.hk + 30.0f;
    for (u32 j = 0; j < 7; ++j) {
        const f32 offset = j >= 5 ? 40.0f : 38.0f;
        c.stat[j] = right + offset; // CENTER at the previous one's RIGHT + 38 (40)
        right = c.stat[j] + 30.0f;
        if (j < stats) {
            last = c.stat[j];
        }
    }
    c.honor = last + 58.0f;
    return c;
}

} // namespace

f32 worldStateScoreWidth(u32 columns, bool scrollBar) {
    return 530.0f + 77.0f * static_cast<f32>(std::min<u32>(columns, 7)) + (scrollBar ? 37.0f : 0.0f);
}

WorldStateScoreResult worldStateScoreFrame(DialogContext& c, const WorldStateScoreView& v,
                                           WorldStateScoreState& st) {
    VD_PROFILE_ZONE("WorldStateScoreFrame::draw");
    WorldStateScoreResult result;
    Gui g{c.ui, c.draw};
    const u32 stats = static_cast<u32>(std::min<usize>(v.columns.size(), 7));
    const u32 count = static_cast<u32>(v.rows.size());
    const bool scrollBar = count > kScoreButtons;
    const u32 maxOffset = scrollBar ? count - kScoreButtons : 0;
    st.offset = std::min(st.offset, maxOffset);
    const f32 width = worldStateScoreWidth(stats, scrollBar);
    // What shows of the 640-wide art: from 8 to 116 short of its right edge, from 16 down to the tabs.
    const f32 visibleW = width - 124.0f;
    const f32 visibleH = 475.0f - 16.0f;
    const Rect place = c.panel.w > 0.0f ? c.panel : c.safeArea.inset(c.ui.px(6.0f));
    const f32 s = std::min({place.w / visibleW, place.h / visibleH, c.ui.px(1.4f)});
    const f32 ox = place.centerX() - (8.0f + visibleW * 0.5f) * s;
    const f32 oy = (c.panel.w > 0.0f ? place.y : place.centerY() - visibleH * 0.5f * s) - 16.0f * s;
    const auto at = [&](f32 x, f32 y, f32 w, f32 h) { return Rect{ox + x * s, oy + y * s, w * s, h * s}; };
    const Rect visible = at(8.0f, 16.0f, visibleW, 437.0f - 16.0f);
    result.panel = visible;
    ui::hit(g, ui::widgetId("score.frame"), visible); // the panel takes its fingers (not the world's)

    // BACKGROUND: TopBackground, tiled (TexCoords 0..width/256).
    {
        const f32 bgW = width - 129.0f;
        for (f32 x = 0.0f; x < bgW; x += 256.0f) {
            const f32 w = std::min(256.0f, bgW - x);
            g.draw.sprite(at(13.0f + x, 20.0f, w, 64.0f), kScoreTopBackground, ui::kWhite,
                          {0.0f, 0.0f, w / 256.0f, 1.0f});
        }
    }
    // ARTWORK: the frame.
    g.draw.sprite(at(8.0f, 16.0f, 128.0f, 256.0f), kScoreTopLeft);
    g.draw.sprite(at(136.0f, 16.0f, width - 392.0f, 256.0f), kScoreTop);
    g.draw.sprite(at(width - 256.0f, 16.0f, 256.0f, 256.0f), kScoreTopRight);
    g.draw.sprite(at(8.0f, 272.0f, 128.0f, 256.0f), kScoreBotLeft);
    g.draw.sprite(at(136.0f, 272.0f, width - 392.0f, 256.0f), kScoreBot);
    g.draw.sprite(at(width - 256.0f, 272.0f, 256.0f, 256.0f), kScoreBotRight);

    // The label (STAT_TEMPLATE) or the winner's banner over it.
    if (v.winner >= 0) {
        const Rect banner = at(17.0f, 20.0f, width - 141.0f - 17.0f, 16.0f);
        const bool horde = v.winner == 0;
        const ui::TexCoords leftUv =
            horde ? ui::TexCoords{0.0f, 0.5f, 1.0f, 0.75f} : ui::TexCoords{0.0f, 0.0f, 1.0f, 0.25f};
        const ui::TexCoords rightUv = horde ? ui::TexCoords{0.0f, 0.75f, 0.97265625f, 1.0f}
                                            : ui::TexCoords{0.0f, 0.25f, 0.97265625f, 0.5f};
        const f32 leftW = std::min(256.0f * s, banner.w);
        g.draw.sprite({banner.x, banner.y, leftW, banner.h}, kScoreHighlight, ui::kWhite, leftUv);
        if (banner.w > leftW) {
            g.draw.sprite({banner.x + leftW, banner.y, banner.w - leftW, banner.h}, kScoreHighlight,
                          ui::kWhite, rightUv);
        }
        ui::TextStyle style = ui::gameFont(kFontNormal * s, horde ? kHordeColor : kAllianceColor);
        style.align = ui::Align::Center;
        g.draw.text(banner.centerX(), baseline(g.draw, style, banner.centerY()), v.winnerText, style);
    } else {
        ui::TextStyle style = ui::gameFont(kFontNormal * s);
        style.align = ui::Align::Center;
        g.draw.text(ox + (width * 0.5f - 57.0f) * s, baseline(g.draw, style, oy + (21.0f + 6.0f) * s),
                    v.label, style);
    }

    // Headers.
    const ui::TextStyle headerStyle = ui::gameFont(kFontSmall * s, ui::kWhite);
    const std::string_view nameHeader = text::get("NAME");
    g.draw.text(ox + 20.0f * s, baseline(g.draw, headerStyle, oy + (48.0f + kFontSmall * 0.5f) * s),
                nameHeader, headerStyle);
    const f32 nameRight = 20.0f + g.draw.measure(nameHeader, ui::FontId::Friz, kFontSmall * s) / s;
    const ScoreColumns cols = scoreColumns(nameRight, stats);
    std::string_view tooltip;
    std::string_view tooltipLine;
    u32 tooltipColor = kTooltipTextColor;
    Rect tooltipAnchor;
    const auto header = [&](u32 id, f32 cx, f32 w, std::string_view label, std::string_view tip) {
        const Rect box = at(cx - w * 0.5f, cols.headerY - 15.0f, w, 30.0f);
        ui::TextStyle style = headerStyle;
        style.align = ui::Align::Center;
        const f32 textH = g.draw.measureWrapped(65.0f * s, label, ui::FontId::Friz, kFontSmall * s);
        g.draw.wrappedText({box.centerX() - 32.5f * s, box.centerY() - textH * 0.5f, 65.0f * s, textH + 1.0f},
                           label, style);
        const ui::WidgetState state = c.ui.widget(ui::widgetId("score.header", id), box);
        if (state.pressed && !tip.empty()) {
            tooltip = tip;
            tooltipColor = ui::kWhite; // GameTooltip:AddLine(this.tooltip, 1.0, 1.0, 1.0, 1)
            tooltipAnchor = box;
        }
    };
    header(0, cols.kb, 60.0f, text::get("SCORE_KILLING_BLOWS"), text::get("KILLING_BLOW_TOOLTIP"));
    header(1, cols.deaths, 60.0f, text::get("DEATHS"), text::get("DEATHS_TOOLTIP"));
    header(2, cols.hk, 60.0f, text::get("SCORE_HONORABLE_KILLS"), text::get("HONORABLE_KILLS_TOOLTIP"));
    for (u32 j = 0; j < stats; ++j) {
        header(3 + j, cols.stat[j], 60.0f, v.columns[j].text, v.columns[j].tooltip);
    }
    header(10, cols.honor, 90.0f, text::get("SCORE_HONOR_GAINED"), text::get("HONOR_GAINED_TOOLTIP"));

    // The lines: 22 buttons from 78 units down, 15 apart, as wide as the frame minus 137 (165 with the bar).
    const f32 rowW = scrollBar ? width - 165.0f : width - 137.0f;
    const u32 shown = std::min(count - std::min(count, st.offset), kScoreButtons);
    const Rect rowsArea = at(17.0f, kRowTop, rowW, kRowStep * static_cast<f32>(kScoreButtons) + 1.0f);
    const ui::WidgetState rowsState = c.ui.widget(ui::widgetId("score.rows"), rowsArea);
    const ui::TextStyle numberStyle = [&] {
        ui::TextStyle t = ui::gameFont(kFontNormal * s);
        t.align = ui::Align::Center;
        return t;
    }();
    ui::TextStyle smallStyle = ui::gameFont(kFontSmall * s);
    smallStyle.align = ui::Align::Center;
    char number[16];
    const auto num = [&](i64 value) {
        const int n = std::snprintf(number, sizeof(number), "%lld", static_cast<long long>(value));
        return std::string_view(number, static_cast<usize>(std::max(n, 0)));
    };
    for (u32 i = 0; i < shown; ++i) {
        const ScoreRowView& row = v.rows[st.offset + i];
        const f32 top = kRowTop + static_cast<f32>(i) * kRowStep;
        const Rect r = at(17.0f, top, rowW, kRowHeight);
        if (row.faction >= 0) {
            const bool horde = row.faction == 0;
            const ui::TexCoords leftUv =
                horde ? ui::TexCoords{0.0f, 0.5f, 1.0f, 0.75f} : ui::TexCoords{0.0f, 0.0f, 1.0f, 0.25f};
            const ui::TexCoords rightUv = horde ? ui::TexCoords{0.0f, 0.75f, 0.97265625f, 1.0f}
                                                : ui::TexCoords{0.0f, 0.25f, 0.97265625f, 0.5f};
            const f32 leftW = std::min(256.0f * s, r.w);
            g.draw.sprite({r.x, r.y, leftW, r.h}, kScoreHighlight, ui::kWhite, leftUv);
            if (r.w > leftW) {
                g.draw.sprite({r.x + leftW, r.y, r.w - leftW, r.h}, kScoreHighlight, ui::kWhite, rightUv);
            }
        }
        if (row.rankBadge != 0) {
            g.draw.sprite(at(17.0f, top, 16.0f, 16.0f), row.rankBadge);
        }
        ui::TextStyle nameStyle = ui::gameFont(kFontNormal * s, row.nameColor);
        g.draw.pushClip(at(32.0f, top - 1.0f, 160.0f, kRowHeight));
        g.draw.text(ox + 32.0f * s, baseline(g.draw, nameStyle, oy + (top + 8.0f - 1.0f) * s), row.name,
                    nameStyle);
        g.draw.popClip();
        const f32 numberY = oy + (cols.headerY + 32.0f + static_cast<f32>(i) * kRowStep) * s;
        g.draw.text(ox + cols.kb * s, baseline(g.draw, numberStyle, numberY), num(row.killingBlows),
                    numberStyle);
        g.draw.text(ox + cols.deaths * s, baseline(g.draw, numberStyle, numberY), num(row.deaths),
                    numberStyle);
        g.draw.text(ox + cols.hk * s, baseline(g.draw, numberStyle, numberY), num(row.honorableKills),
                    numberStyle);
        g.draw.text(ox + cols.honor * s, baseline(g.draw, smallStyle, numberY), num(row.bonusHonor),
                    smallStyle);
        const f32 columnY = oy + (cols.headerY + 33.0f + static_cast<f32>(i) * kRowStep) * s;
        for (u32 j = 0; j < stats; ++j) {
            const ScoreColumnView& col = v.columns[j];
            const i32 value = row.stats[j];
            if (col.hasIcon) {
                // FLAG_COUNT_TEMPLATE ("x %d") with the side's icon left of it, nothing for 0.
                if (value > 0) {
                    char flagCount[24];
                    const std::string_view t = text::format(flagCount, "FLAG_COUNT_TEMPLATE", value);
                    const f32 cx = ox + (cols.stat[j] + 6.0f) * s;
                    g.draw.text(cx, baseline(g.draw, smallStyle, columnY), t, smallStyle);
                    const f32 textLeft = cx - g.draw.measure(t, ui::FontId::Friz, kFontSmall * s) * 0.5f;
                    const u64 icon = col.icons[row.faction == 0 ? 0 : 1];
                    if (icon != 0 && row.faction >= 0) {
                        g.draw.sprite(
                            {textLeft - 16.0f * s, columnY - 8.0f * s + 1.0f * s, 16.0f * s, 16.0f * s},
                            icon);
                    }
                }
            } else {
                g.draw.text(ox + (cols.stat[j] - 1.0f) * s, baseline(g.draw, smallStyle, columnY), num(value),
                            smallStyle);
            }
        }
    }
    // Long presses on a line: the name's tooltip (its race and class), the rank badge's (the rank's name).
    if (rowsState.touch && rowsState.pressed && rowsState.touch->longPressed && !rowsState.touch->moved) {
        const f32 ux = (rowsState.touch->x - ox) / s;
        const f32 uy = (rowsState.touch->y - oy) / s;
        const i32 i = static_cast<i32>(std::floor((uy - kRowTop) / kRowStep));
        if (i >= 0 && static_cast<u32>(i) < shown) {
            const ScoreRowView& row = v.rows[st.offset + static_cast<u32>(i)];
            const Rect rowRect = at(17.0f, kRowTop + static_cast<f32>(i) * kRowStep, rowW, kRowHeight);
            if (ux < 33.0f && row.rankBadge != 0 && !row.rankName.empty()) {
                tooltip = row.rankName;
                tooltipAnchor = rowRect;
            } else if (ux >= 32.0f && ux < 192.0f && !row.raceClass.empty()) {
                tooltip = row.name;
                tooltipLine = row.raceClass;
                tooltipAnchor = rowRect;
            }
        }
    }
    // FauxScrollFrame: dragging the lines moves them a line at a time.
    if (scrollBar && rowsState.touch && rowsState.touch->moved) {
        st.dragRest -= rowsState.touch->dy;
        const f32 step = kRowStep * s;
        while (st.dragRest >= step && st.offset < maxOffset) {
            ++st.offset;
            st.dragRest -= step;
        }
        while (st.dragRest <= -step && st.offset > 0) {
            --st.offset;
            st.dragRest += step;
        }
        if ((st.offset == 0 && st.dragRest < 0.0f) || (st.offset == maxOffset && st.dragRest > 0.0f)) {
            st.dragRest = 0.0f;
        }
    } else {
        st.dragRest = 0.0f;
    }
    if (scrollBar) {
        // WorldStateScoreScrollFrame TOPRIGHT (-148, -76), 296 x 361: its bar right of it
        // (UI-Character-ScrollBar's top and bottom pieces), the arrows and the thumb.
        const f32 barX = width - 148.0f - 2.0f;
        g.draw.sprite(at(barX, 76.0f - 3.0f, 31.0f, 254.0f), kScrollBarArt, ui::kWhite,
                      {0.0f, 0.0078125f, 0.484375f, 1.0f});
        g.draw.sprite(at(barX, 76.0f - 3.0f + 254.0f, 31.0f, 56.0f), kScrollBarArt, ui::kWhite,
                      {0.0f, 0.78125f, 0.484375f, 1.0f});
        g.draw.sprite(at(barX, 76.0f + 361.0f - 105.0f + 1.0f, 31.0f, 105.0f), kScrollBarArt, ui::kWhite,
                      {0.515625f, 0.0f, 1.0f, 0.41015625f});
        const Rect up = at(barX + 6.0f, 76.0f - 1.0f, 16.0f, 16.0f);
        const Rect down = at(barX + 6.0f, 76.0f + 361.0f - 15.0f, 16.0f, 16.0f);
        if (ui::tapped(ui::hit(g, ui::widgetId("score.up"), up.inset(-4.0f * s))) && st.offset > 0) {
            st.offset =
                st.offset > kScoreButtons ? st.offset - kScoreButtons : 0; // a page, as the bar's arrows step
        }
        if (ui::tapped(ui::hit(g, ui::widgetId("score.down"), down.inset(-4.0f * s))) &&
            st.offset < maxOffset) {
            st.offset = std::min(maxOffset, st.offset + kScoreButtons);
        }
        g.draw.sprite(up, st.offset > 0 ? kScrollUp : kScrollUpDisabled);
        g.draw.sprite(down, st.offset < maxOffset ? kScrollDown : kScrollDownDisabled);
        const f32 track = 361.0f - 32.0f - 16.0f;
        const f32 knobY =
            76.0f + 15.0f + track * (maxOffset > 0 ? static_cast<f32>(st.offset) / maxOffset : 0.0f);
        g.draw.sprite(at(barX + 6.0f, knobY, 16.0f, 16.0f), kScrollKnob, ui::kWhite,
                      {0.25f, 0.25f, 0.75f, 0.75f});
    }

    // Under the last line shown: the players on each side, the time elapsed.
    const u32 lastShown = std::max(shown, 1u);
    const f32 lastBottom = kRowTop + static_cast<f32>(lastShown - 1) * kRowStep + kRowHeight;
    const ui::TextStyle footStyle = ui::gameFont(kFontSmall * s);
    if (!v.playerCount.empty()) {
        g.draw.text(ox + 32.0f * s,
                    oy + (lastBottom + 6.0f) * s + g.draw.ascent(ui::FontId::Friz, kFontSmall * s),
                    v.playerCount, footStyle);
    }
    {
        ui::TextStyle t = footStyle;
        t.align = ui::Align::Right;
        g.draw.text(ox + (17.0f + rowW - 20.0f) * s,
                    oy + (lastBottom + 7.0f) * s + g.draw.ascent(ui::FontId::Friz, kFontSmall * s), v.runTime,
                    t);
    }
    // The end: "Battleground closing in" and its timer, Leave Battleground.
    if (v.winner >= 0) {
        const ui::TextStyle labelStyle = ui::gameFont(kFontSmall * s, ui::kWhite);
        const f32 labelBottom = oy + (kScoreFrameHeight - 77.0f) * s;
        const std::string_view label = text::get("TIME_TO_PORT");
        const f32 labelW = g.draw.text(ox + 40.0f * s,
                                       labelBottom - g.draw.lineHeight(ui::FontId::Friz, kFontSmall * s) +
                                           g.draw.ascent(ui::FontId::Friz, kFontSmall * s),
                                       label, labelStyle);
        g.draw.text(ox + (40.0f + 2.0f) * s + labelW,
                    labelBottom - g.draw.lineHeight(ui::FontId::Friz, kFontSmall * s) +
                        g.draw.ascent(ui::FontId::Friz, kFontSmall * s),
                    v.timer, footStyle);
        const Rect leave = at(width * 0.5f - 30.0f - 76.5f, kScoreFrameHeight - 76.0f - 21.0f, 153.0f, 21.0f);
        result.leave = ui::button(g, ui::widgetId("score.leave"), leave, text::get("RETURN_TO_WORLD"));
    }

    // WorldStateScoreFrameCloseButton (TOPRIGHT -111, -12).
    result.closed =
        ui::closeButton(g, ui::widgetId("score.close"), at(width - 111.0f - 32.0f, 12.0f, 32.0f, 32.0f));

    // The tabs under the frame: All, Alliance, Horde (PanelTemplates_TabResize: the text + 40 units).
    {
        const std::string_view names[3] = {text::get("ALL"), text::get("FACTION_ALLIANCE"),
                                           text::get("FACTION_HORDE")};
        f32 x = 10.0f;
        const f32 tabTop = kScoreFrameHeight - 69.0f;
        for (u32 t = 0; t < 3; ++t) {
            const f32 textW = g.draw.measure(names[t], ui::FontId::Friz, kFontSmall * s) / s;
            const f32 w = textW + 40.0f;
            const bool selected = v.tab == t;
            const f32 artTop = tabTop + (selected ? -5.0f : -4.0f);
            const u64 art = selected ? kTabActive : kTabInactive;
            const f32 middle = std::max(0.0f, w - 40.0f);
            g.draw.sprite(at(x, artTop, 20.0f, 29.0f), art, ui::kWhite, {0.0f, 0.09375f, 0.15625f, 1.0f});
            g.draw.sprite(at(x + 20.0f, artTop, middle, 29.0f), art, ui::kWhite,
                          {0.15625f, 0.09375f, 0.84375f, 1.0f});
            g.draw.sprite(at(x + 20.0f + middle, artTop, 20.0f, 29.0f), art, ui::kWhite,
                          {0.84375f, 0.09375f, 1.0f, 1.0f});
            ui::TextStyle style = ui::gameFont(kFontSmall * s, selected ? ui::kWhite : ui::kGold);
            style.align = ui::Align::Center;
            const Rect tab = at(x, tabTop, w, 32.0f);
            g.draw.text(tab.centerX(), baseline(g.draw, style, tab.centerY() - 9.0f * s), names[t], style);
            const ui::WidgetState state = ui::hit(g, ui::widgetId("score.tab", t), tab);
            if (state.pressed && !selected) {
                g.draw.sprite(at(x + 10.0f, tabTop - 9.0f + 16.0f - 8.0f, w - 20.0f, 16.0f), kTabHighlight);
            }
            if (ui::tapped(state) && !selected) {
                result.tab = static_cast<i32>(t);
            }
            x += w - 16.0f;
        }
    }
    if (!tooltip.empty()) {
        worldStateTooltip(c, tooltipAnchor, tooltip, tooltipLine, tooltipColor);
    }
    return result;
}

// --- Blizzard_BattlefieldMinimap
// -----------------------------------------------------------------------------

namespace {

constexpr f32 kMapW = 225.0f, kMapH = 150.0f; // BattlefieldMinimap
constexpr f32 kTabH = 32.0f;
constexpr u64 kTabShowMs = 3000;   // the tab stays this long after the last touch (the mouse leaving it)
constexpr f32 kTabAlpha = 0.75f;   // DEFAULT_BATTLEFIELD_TAB_ALPHA
constexpr f32 kTabFadeMs = 150.0f; // BATTLEFIELD_TAB_FADE_TIME

void arrow(Gui& g, f32 x, f32 y, f32 size, f32 facing) {
    const std::optional<ui::Sprite> sprite =
        g.draw.assets() ? g.draw.assets()->sprite(kMinimapArrow) : std::nullopt;
    if (!sprite) {
        g.draw.solid({x - size * 0.25f, y - size * 0.25f, size * 0.5f, size * 0.5f}, ui::kGold);
        return;
    }
    const f32 half = size * 0.5f;
    const f32 upX = -std::sin(facing), upY = -std::cos(facing);
    const f32 rightX = -upY, rightY = upX;
    const f32 corners[8] = {x - rightX * half + upX * half, y - rightY * half + upY * half,
                            x + rightX * half + upX * half, y + rightY * half + upY * half,
                            x + rightX * half - upX * half, y + rightY * half - upY * half,
                            x - rightX * half - upX * half, y - rightY * half - upY * half};
    g.draw.spriteQuad(corners, *sprite);
}

} // namespace

Rect battlefieldMinimapDefault(const DialogContext& c, const WorldStateHudAvoid& avoid) {
    const f32 u = c.ui.px(ui::kUnitDp);
    const Rect& safe = c.safeArea;
    const f32 gap = c.ui.px(6.0f);
    const f32 w = kMapW * u, h = (kTabH + 5.0f + kMapH) * u;
    const Rect obstacles[] = {avoid.targetFrame, avoid.buffs,    avoid.cluster, avoid.actions,
                              avoid.stance,      avoid.extraBar, avoid.lines};
    const auto covered = [&](const Rect& r) {
        f32 area = 0.0f;
        for (const Rect& o : obstacles) {
            if (overlaps(r, o)) {
                area += (std::min(r.right(), o.right()) - std::max(r.x, o.x)) *
                        (std::min(r.bottom(), o.bottom()) - std::max(r.y, o.y));
            }
        }
        return area;
    };
    const auto clampToSafe = [&](Rect r) {
        r.x = std::clamp(r.x, safe.x, std::max(safe.x, safe.right() - r.w));
        r.y = std::clamp(r.y, safe.y, std::max(safe.y, safe.bottom() - r.h));
        return r;
    };
    // Right-aligned above the action buttons (and whatever sits on them: the stance and extra bars).
    f32 bottom = safe.bottom() - gap;
    const f32 right = safe.right() - gap;
    for (const Rect& r : {avoid.actions, avoid.stance, avoid.extraBar}) {
        if (r.w > 0.0f && r.right() > right - w && r.x < right) {
            bottom = std::min(bottom, r.y - gap);
        }
    }
    const f32 topBars = std::max(
        {safe.y, avoid.buffs.w > 0.0f ? avoid.buffs.bottom() : safe.y,
         avoid.extraBar.w > 0.0f && avoid.extraBar.y < safe.centerY() ? avoid.extraBar.bottom() : safe.y});
    const Rect candidates[] = {
        clampToSafe({right - w, bottom - h, w, h}),
        clampToSafe({right - w, avoid.cluster.bottom() + gap, w, h}),
        clampToSafe({avoid.cluster.x - gap - w, topBars + gap, w, h}),
        clampToSafe({avoid.actions.x - gap - w, avoid.actions.bottom() - h, w, h}),
    };
    Rect best = candidates[0];
    f32 bestCovered = covered(best);
    for (const Rect& r : candidates) {
        const f32 area = covered(r);
        if (area < bestCovered - 0.5f) {
            best = r;
            bestCovered = area;
        }
        if (bestCovered <= 0.5f) {
            break;
        }
    }
    return {best.x, best.y, 64.0f * u, kTabH * u};
}

BattlefieldMinimapResult battlefieldMinimap(DialogContext& c, const BattlefieldMinimapView& v,
                                            BattlefieldMinimapState& s, const WorldStateHudAvoid& avoid,
                                            u64 nowMs) {
    VD_PROFILE_ZONE("BattlefieldMinimap::draw");
    BattlefieldMinimapResult result;
    Gui g{c.ui, c.draw};
    const f32 u = c.ui.px(ui::kUnitDp);
    if (!s.placed) {
        const Rect tab = battlefieldMinimapDefault(c, avoid);
        s.x = tab.x;
        s.y = tab.y;
    }
    // The tab, then the map 5 units under it (TOPLEFT to the tab's BOTTOMLEFT).
    const std::string_view tabText = text::get("BATTLEFIELD_MINIMAP");
    const f32 tabW = g.draw.measure(tabText, ui::FontId::Friz, kFontSmall * u) + 45.0f * u; // TabResize(5)
    Rect tab{s.x, s.y, tabW, kTabH * u};
    Rect map{s.x, s.y + (kTabH + 5.0f) * u, kMapW * u, kMapH * u};
    result.map = map;
    const f32 alpha = 1.0f - std::clamp(v.opacity, 0.0f, 1.0f);
    const f32 dimmed =
        alpha >= 0.15f ? alpha - 0.15f : alpha; // the overlays' (BattlefieldMinimap_SetOpacity)

    // BACKGROUND: the 4 x 3 tiles of the battleground's map (its 1002 x 668 part over the 225 x 150 units).
    const f32 tileW = kMapW * 256.0f / 1002.0f * u, tileH = kMapH * 256.0f / 668.0f * u;
    g.draw.pushClip(map);
    for (u32 i = 0; i < 12; ++i) {
        if (!v.tiles[i].valid()) {
            continue;
        }
        g.draw.texture(
            {map.x + static_cast<f32>(i % 4) * tileW, map.y + static_cast<f32>(i / 4) * tileH, tileW, tileH},
            v.tiles[i], ui::withAlpha(ui::kWhite, alpha));
    }
    g.draw.popClip();
    // BORDER / OVERLAY: the corner and the border (UI-BattlefieldMinimap-Border 256 x 256 at -12, +12).
    g.draw.sprite({map.right() - 34.0f * u, map.y - 3.0f * u, 32.0f * u, 32.0f * u}, kDialogCorner,
                  ui::withAlpha(ui::kWhite, dimmed));
    g.draw.sprite({map.x - 12.0f * u, map.y - 12.0f * u, 256.0f * u, 256.0f * u}, kMapBorder,
                  ui::withAlpha(ui::kWhite, alpha));
    // The touches: a finger on the map shows the tab (and its menu on a long press).
    const ui::WidgetState onMap = c.ui.widget(ui::widgetId("bfmap.map"), map);
    if (onMap.pressed) {
        s.touchedMs = nowMs;
    }
    const auto toMap = [&](f32 mu, f32 mv) { return std::pair{map.x + mu * map.w, map.y + mv * map.h}; };
    const auto inside = [](f32 mu, f32 mv) { return mu >= 0.0f && mv >= 0.0f && mu <= 1.0f && mv <= 1.0f; };
    // The corpse, the group, the team out of it, the flags, the player.
    if (v.hasCorpse && inside(v.corpseU, v.corpseV)) {
        const auto [x, y] = toMap(v.corpseU, v.corpseV);
        g.draw.sprite({x - 8.0f * u, y - 8.0f * u, 16.0f * u, 16.0f * u}, kPoiIcons, ui::kWhite,
                      {0.875f, 0.0f, 1.0f, 0.125f});
    }
    std::array<std::string_view, 12> names{};
    u32 nameCount = 0;
    const Rect* pressedAt = nullptr;
    f32 pressX = 0.0f, pressY = 0.0f;
    if (onMap.touch && onMap.pressed) {
        pressedAt = &map;
        pressX = onMap.touch->x;
        pressY = onMap.touch->y;
    }
    const auto dot = [&](const BattlefieldMapDot& d) {
        if (!inside(d.u, d.v)) {
            return;
        }
        const auto [x, y] = toMap(d.u, d.v);
        const Rect r{x - 6.0f * u, y - 6.0f * u, 12.0f * u, 12.0f * u};
        if (v.partyIcon.valid()) {
            g.draw.texture(r, v.partyIcon);
        } else {
            g.draw.solid(r.inset(3.0f * u), rgba(255, 210, 0));
        }
        if (pressedAt && r.inset(-4.0f * u).contains(pressX, pressY) && nameCount < names.size() &&
            !d.name.empty()) {
            names[nameCount++] = d.name;
        }
    };
    if (v.showPlayers) {
        for (const BattlefieldMapDot& d : v.party) {
            dot(d);
        }
        for (const BattlefieldMapDot& d : v.team) {
            dot(d);
        }
        for (const BattlefieldMapFlag& f : v.flags) {
            if (inside(f.u, f.v) && f.texture != 0) {
                const auto [x, y] = toMap(f.u, f.v);
                g.draw.sprite({x - 9.0f * u, y - 9.0f * u, 18.0f * u, 18.0f * u}, f.texture);
            }
        }
    }
    if (v.hasPlayer && inside(v.playerU, v.playerV)) {
        const auto [x, y] = toMap(v.playerU, v.playerV);
        arrow(g, x, y, 24.0f * u, v.facing);
    }
    // BattlefieldMinimapCloseButton (TOPRIGHT + 2, + 7).
    if (ui::closeButton(g, ui::widgetId("bfmap.close"),
                        {map.right() + 2.0f * u - 32.0f * u, map.y - 7.0f * u, 32.0f * u, 32.0f * u})) {
        result.closed = true;
    }
    // The tab: hidden (alpha 0) until the map is touched, then DEFAULT_BATTLEFIELD_TAB_ALPHA; faded in and
    // out.
    const ui::WidgetState onTab = c.ui.widget(ui::widgetId("bfmap.tab"), tab);
    if (onTab.pressed) {
        s.touchedMs = nowMs;
    }
    const u64 since = nowMs - s.touchedMs;
    f32 tabAlpha = 0.0f;
    if (s.touchedMs != 0) {
        if (since < kTabShowMs) {
            tabAlpha = kTabAlpha * std::min(1.0f, static_cast<f32>(since + 16) / kTabFadeMs);
        } else if (since < kTabShowMs + static_cast<u64>(kTabFadeMs)) {
            tabAlpha = kTabAlpha * (1.0f - static_cast<f32>(since - kTabShowMs) / kTabFadeMs);
        }
    }
    if (tabAlpha > 0.0f || s.menu || s.dragging) {
        const u32 tint =
            ui::withAlpha(ui::kWhite, std::max(tabAlpha, s.menu || s.dragging ? kTabAlpha : 0.0f));
        const f32 middle = tab.w - 32.0f * u;
        g.draw.sprite({tab.x, tab.y, 16.0f * u, tab.h}, kChatTab, tint, {0.0f, 0.0f, 0.25f, 1.0f});
        g.draw.sprite({tab.x + 16.0f * u, tab.y, middle, tab.h}, kChatTab, tint, {0.25f, 0.0f, 0.75f, 1.0f});
        g.draw.sprite({tab.right() - 16.0f * u, tab.y, 16.0f * u, tab.h}, kChatTab, tint,
                      {0.75f, 0.0f, 1.0f, 1.0f});
        ui::TextStyle style = ui::gameFont(kFontSmall * u, ui::withAlpha(ui::kGold, tabAlpha / kTabAlpha));
        g.draw.text(tab.x + 16.0f * u, baseline(g.draw, style, tab.centerY() + 5.0f * u), tabText, style);
    }
    // Dragging the tab moves the map (unless locked: BattlefieldMinimapOptions.locked).
    if (onTab.touch && onTab.pressed && onTab.touch->moved && !v.locked) {
        s.dragging = true;
        s.placed = true;
        s.x = std::clamp(s.x + onTab.touch->dx, c.safeArea.x, c.safeArea.right() - map.w);
        s.y = std::clamp(s.y + onTab.touch->dy, c.safeArea.y, c.safeArea.bottom() - map.bottom() + s.y);
        result.moved = true;
    } else if (!onTab.pressed) {
        s.dragging = false;
    }
    // The tab's right click (a long press on it or on the map): the options menu.
    if ((onTab.longPress && !(onTab.touch && onTab.touch->moved)) ||
        (onMap.longPress && !(onMap.touch && onMap.touch->moved) && nameCount == 0)) {
        s.menu = true;
        s.opacityFrame = false;
    }
    if (s.menu) {
        std::array<DropDownItemView, 3> items{};
        items[0].text = text::get("SHOW_BATTLEFIELDMINIMAP_PLAYERS");
        items[0].checkable = true;
        items[0].checked = v.showPlayers;
        items[1].text = text::get("LOCK_BATTLEFIELDMINIMAP");
        items[1].checkable = true;
        items[1].checked = v.locked;
        items[2].text = text::get("BATTLEFIELDMINIMAP_OPACITY_LABEL");
        DropDownMenuView menu;
        menu.level1 = items;
        menu.x = tab.x; // ToggleDropDownMenu(1, nil, BattlefieldMinimapTabDropDown, tab, 0, 0)
        menu.y = tab.bottom();
        const DropDownMenuResult r = dropDownMenu(c, "bfmap.menu", menu);
        if (r.picked == 0) {
            result.togglePlayers = true;
        } else if (r.picked == 1) {
            result.toggleLock = true;
        } else if (r.picked == 2) {
            s.opacityFrame = true; // BattlefieldMinimap_ShowOpacity
        }
        if (r.picked >= 0 || r.closed) {
            s.menu = false;
        }
    }
    if (s.opacityFrame) {
        // OpacityFrame (ColorPickerFrame.xml: 80 x 180, the vertical OpacityFrameSlider 16 x 128 at TOP - 10,
        // -35) with its TOPRIGHT at the map's TOPLEFT + 7 up.
        const Rect frame{std::max(c.safeArea.x, map.x - 80.0f * u * 1.4f),
                         std::max(c.safeArea.y, map.y - 7.0f * u), 80.0f * u * 1.4f, 180.0f * u * 1.4f};
        const f32 fu = u * 1.4f; // a finger's size: the slider grows
        const ui::WidgetState onFrame = c.ui.widget(ui::widgetId("bfmap.opacity"), frame);
        g.draw.sprite(frame.inset(11.0f * fu, 11.0f * fu), kDialogBackground);
        g.draw.backdropBorder(frame, kDialogBorder, 32.0f * fu);
        ui::TextStyle title = ui::gameFont(kFontSmall * fu);
        title.align = ui::Align::Center;
        g.draw.text(frame.centerX(), baseline(g.draw, title, frame.y + 20.0f * fu), text::get("OPACITY"),
                    title);
        const Rect track{frame.centerX() - 10.0f * fu - 8.0f * fu, frame.y + 35.0f * fu, 16.0f * fu,
                         128.0f * fu};
        g.draw.backdropBorder(track, kSliderBorder, 8.0f * fu);
        ui::TextStyle small = ui::gameFont(kFontSmall * fu, ui::kWhite);
        g.draw.text(track.right() + 2.0f * fu, track.bottom() - 3.0f * fu, "0%", small);
        g.draw.text(track.right() + 2.0f * fu, track.y + 10.0f * fu, "100%", small);
        f32 value = std::clamp(v.opacity, 0.0f, 1.0f);
        if (onFrame.touch && onFrame.pressed &&
            track.inset(-12.0f * fu).contains(onFrame.touch->startX, onFrame.touch->startY)) {
            // The slider's top is its maximum (100%), as a vertical Slider's.
            value = std::clamp(1.0f - (onFrame.touch->y - track.y - 8.0f * fu) / (track.h - 16.0f * fu), 0.0f,
                               1.0f);
            value = std::round(value * 100.0f) / 100.0f; // valueStep 0.01
            result.opacityChanged = true;
            result.opacity = value;
        }
        const f32 thumbY = track.y + 8.0f * fu + (1.0f - value) * (track.h - 16.0f * fu);
        g.draw.sprite({track.centerX() - 16.0f * fu, thumbY - 16.0f * fu, 32.0f * fu, 32.0f * fu},
                      kSliderThumb);
        // A tap away from it closes it (the original's OnShow / OnHide with the slider kept).
        for (const ui::Touch& t : c.ui.touches()) {
            if (t.began && !frame.contains(t.x, t.y)) {
                s.opacityFrame = false;
            }
        }
    }
    // BattlefieldMinimapUnit_OnEnter: the names under the finger.
    if (nameCount > 0) {
        char list[256];
        usize used = 0;
        for (u32 i = 0; i < nameCount; ++i) {
            const std::string_view n = names[i];
            if (used + n.size() + 2 >= sizeof(list)) {
                break;
            }
            if (used > 0) {
                list[used++] = '\n';
            }
            std::copy(n.begin(), n.end(), list + used);
            used += n.size();
        }
        worldStateTooltip(c, {pressX, pressY, 1.0f, 1.0f}, std::string_view(list, used));
        result.tooltip = true;
    }
    return result;
}

} // namespace vd
