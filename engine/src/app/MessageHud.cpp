#include "app/MessageHud.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/MirrorTimers.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>

namespace vd {
namespace {

using ui::rgba;

constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kBorder = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Border.blp");

// MirrorTimer.lua MirrorTimerColors, by type (EXHAUSTION 1, 0.9, 0; BREATH 0, 0.5, 1; FEIGNDEATH 1, 0.7, 0).
constexpr u32 kColors[] = {rgba(255, 230, 0), rgba(0, 128, 255), rgba(255, 178, 0)};

// The centre of a piece `halfWidth` wide spanning top..bottom, centred in `area` as the original places it,
// or right of `avoid` when they would overlap (the expanded chat of the outer screen, a chat pushed right by
// party frames).
f32 clearCenter(const ui::Rect& area, f32 halfWidth, f32 top, f32 bottom, const ui::Rect& avoid) {
    const f32 center = area.centerX();
    if (avoid.w <= 0.0f || avoid.y >= bottom || avoid.bottom() <= top ||
        avoid.right() <= center - halfWidth || avoid.x >= center + halfWidth) {
        return center;
    }
    return std::max(center, std::min(avoid.right() + halfWidth, area.right() - halfWidth));
}

} // namespace

void errorsFrame(DialogContext& c, const ui::Rect& area, const ui::MessageFrame& frame, u64 nowMs, f32 below,
                 const ui::Rect& avoid) {
    ui::Gui g{c.ui, c.draw};
    std::array<ui::MessageFrame::Shown, 4> shown;
    const u32 count = frame.visible(nowMs, shown);
    if (count == 0) {
        return;
    }
    const auto styleOf = [&](const ui::MessageFrame::Shown& s) {
        ui::TextStyle style =
            ui::gameFont(g.px(s.line->big ? 22.0f : 16.0f), ui::withAlpha(s.line->color, s.alpha));
        style.outline = std::max(1.0f, g.px(1.0f));
        style.align = ui::Align::Center;
        return style;
    };
    const f32 top = std::max(area.y + g.unit(122.0f), below); // UIErrorsFrame: TOP, 0, -122
    f32 height = 0.0f, width = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const ui::TextStyle style = styleOf(shown[i]);
        height += style.size * 1.2f;
        width = std::max(width, g.draw.measure(shown[i].line->text.data(), style.font, style.size));
    }
    const f32 centerX = clearCenter(area, width * 0.5f + g.px(4.0f), top, top + height + g.px(4.0f), avoid);
    f32 y = top;
    for (u32 i = 0; i < count; ++i) {
        const ui::TextStyle style = styleOf(shown[i]);
        y += style.size * 1.2f;
        g.draw.text(centerX, y, shown[i].line->text.data(), style);
    }
}

namespace {

// A raid warning frame's height: its 40 units hold two lines of its 20-unit font (here at 1 dp a unit, two lines
// as the font draws them).
f32 raidFrameHeight(const DialogContext& c) {
    return std::max(c.ui.px(40.0f), 2.0f * c.draw.lineHeight(ui::FontId::Friz, c.ui.px(20.0f)));
}

} // namespace

f32 raidWarningFramesBottom(const DialogContext& c, const ui::Rect& area, f32 below) {
    // Each frame 10 under the one above, the first 70 under the UIErrorsFrame's top.
    return std::max(area.y + c.ui.px(122.0f * ui::kUnitDp), below) + c.ui.px(70.0f + 10.0f) +
           2.0f * raidFrameHeight(c);
}

void raidWarningFrames(DialogContext& c, const ui::Rect& area, const ui::MessageFrame& warnings,
                       const ui::MessageFrame& emotes, u64 nowMs, f32 below, const ui::Rect& avoid) {
    VD_PROFILE_ZONE("raidWarningFrames");
    ui::Gui g{c.ui, c.draw};
    // Under the UIErrorsFrame as errorsFrame() places it; the stack keeps its 1 dp a unit (ErrorFont's 16 at
    // 16 dp).
    const f32 width = std::min(g.px(512.0f), area.w - g.px(16.0f));
    f32 top = std::max(area.y + g.unit(122.0f), below) + g.px(60.0f + 10.0f);
    const ui::MessageFrame* frames[2] = {&warnings, &emotes};
    for (const ui::MessageFrame* frame : frames) {
        const f32 bottom = top + raidFrameHeight(c);
        std::array<ui::MessageFrame::Shown, 4> shown;
        const u32 count = frame->visible(nowMs, shown);
        const f32 centerX =
            count > 0 ? clearCenter(area, width * 0.5f + g.px(4.0f), top, bottom, avoid) : area.centerX();
        f32 y = top;
        for (u32 i = 0; i < count; ++i) {
            ui::TextStyle style =
                ui::gameFont(g.px(20.0f), ui::withAlpha(shown[i].line->color, shown[i].alpha));
            style.outline = std::max(1.0f, g.px(1.0f));
            style.align = ui::Align::Center;
            const char* text = shown[i].line->text.data();
            const f32 h = g.draw.measureWrapped(width, text, style.font, style.size);
            if (i > 0 && y + h > bottom + g.px(1.0f)) {
                break; // the frame's two lines are taken
            }
            g.draw.wrappedText({centerX - width * 0.5f, y, width, h + 1.0f}, text, style);
            y += h;
        }
        top = bottom + g.px(10.0f); // RaidBossEmoteFrame: TOP at RaidWarningFrame's BOTTOM, 0, -10
    }
}

f32 mirrorTimerBars(DialogContext& c, std::span<const MirrorTimerBarView> bars, const ui::Rect& avoid) {
    VD_PROFILE_ZONE("mirrorTimerBars");
    ui::Gui g{c.ui, c.draw};
    f32 top = c.safeArea.y + g.unit(96.0f); // MirrorTimer1: TOP, 0, -96
    const f32 centerX = clearCenter(c.safeArea, g.unit(128.0f), top - g.unit(6.0f),
                                    top + g.unit(26.0f) * static_cast<f32>(bars.size()), avoid);
    f32 bottom = 0.0f;
    for (const MirrorTimerBarView& bar : bars) {
        if (bar.shown) {
            bottom = top + g.unit(20.0f); // the border's lower edge
            const ui::Rect area{centerX - g.unit(195.0f) * 0.5f, top + g.unit(2.0f), g.unit(195.0f),
                                g.unit(13.0f)};
            g.draw.solid(area, rgba(0, 0, 0, 128));
            const f32 fill = std::clamp(bar.fill, 0.0f, 1.0f);
            g.draw.sprite({area.x, area.y, area.w * fill, area.h}, kStatusBar, bar.color,
                          {0.0f, 0.0f, fill, 1.0f});
            g.draw.sprite({centerX - g.unit(128.0f), top - g.unit(25.0f), g.unit(256.0f), g.unit(64.0f)},
                          kBorder);
            ui::TextStyle style = ui::gameFont(g.unit(12.0f) * 1.2f, ui::kWhite); // GameFontHighlight
            style.align = ui::Align::Center;
            // $parentText: no size of its own, TOP at the frame's top: its one line hangs from there.
            g.draw.text(centerX,
                        ui::fontStringBaseline(g.draw, style.font, g.unit(12.0f), top + g.unit(6.0f)),
                        bar.label, style);
        }
        top += g.unit(26.0f); // the next frame hangs from this one's bottom, shown or not
    }
    return bottom;
}

u32 mirrorTimerColor(u32 type) {
    return type < std::size(kColors) ? kColors[type] : kColors[1];
}

void mirrorTimerViews(const game::MirrorTimers& timers, const game::GameData* data, u32 nowMs,
                      std::span<MirrorTimerBarView> out) {
    const u32 n = std::min(static_cast<u32>(out.size()), game::MirrorTimers::kFrames);
    for (u32 i = 0; i < n; ++i) {
        const game::MirrorTimer& t = timers.frames()[i];
        MirrorTimerBarView& view = out[i];
        view = MirrorTimerBarView{};
        if (!t.active) {
            continue;
        }
        view.shown = true;
        view.fill = t.fill(nowMs);
        view.color = mirrorTimerColor(t.type);
        // WoW.exe labels the bar "<TIMER>_LABEL"; 1.12 has none for feign death: its spell's name instead.
        const char* key = game::mirrorTimerLabelKey(t.type);
        if (key && text::has(key)) {
            view.label = text::get(key);
        } else if (const std::optional<game::SpellInfo> spell =
                       data && t.spellId ? data->spell(t.spellId) : std::nullopt) {
            view.label = spell->name; // a view of the mapped db pack: outlives the frame
        }
    }
}

} // namespace vd
