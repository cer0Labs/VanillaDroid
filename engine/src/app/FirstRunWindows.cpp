#include "app/FirstRunWindows.h"

#include "core/Hash.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using ui::rgba;

// The colours of the PC's page (tools/setup/PhoneServer.cpp): the same look on both ends.
constexpr u32 kBackground = rgba(22, 18, 13);
constexpr u32 kText = rgba(242, 230, 201);
constexpr u32 kDim = rgba(201, 185, 143);
constexpr u32 kName = rgba(255, 209, 0);
constexpr u32 kRule = rgba(84, 70, 44);
constexpr u32 kPrimary = rgba(140, 29, 14);
constexpr u32 kPrimaryDown = rgba(178, 46, 26);
constexpr u32 kPrimaryBorder = rgba(212, 160, 23);
constexpr u32 kSecondary = rgba(46, 39, 29);
constexpr u32 kSecondaryDown = rgba(72, 61, 45);
constexpr u32 kSecondaryBorder = rgba(124, 107, 74);
constexpr u32 kDisabledFill = rgba(30, 26, 20);
constexpr u32 kDisabledBorder = rgba(62, 55, 44);
constexpr u32 kDisabledText = rgba(118, 107, 88);
constexpr u32 kField = rgba(10, 8, 6);
constexpr u32 kError = rgba(255, 118, 96);
constexpr u32 kTrack = rgba(46, 39, 29);
constexpr u32 kBar = rgba(212, 160, 23);

// Sizes in dp.
constexpr f32 kNameDp = 26.0f;
constexpr f32 kTitleDp = 21.0f;
constexpr f32 kBodyDp = 17.0f;
constexpr f32 kSmallDp = 14.5f;
constexpr f32 kLabelDp = 18.0f;
constexpr f32 kNoteDp = 13.0f;
constexpr f32 kFieldDp = 19.0f;
constexpr f32 kFieldHeightDp = 50.0f;
constexpr f32 kGapDp = 10.0f;
constexpr f32 kMinButtonDp = 48.0f;  // above ui::kTouchDp with room for the gaps
constexpr f32 kNoteButtonDp = 56.0f; // tall enough for the second line

ui::TextStyle style(f32 size, u32 color, ui::Align align = ui::Align::Left) {
    ui::TextStyle s;
    s.font = ui::FontId::App;
    s.size = size;
    s.color = color;
    s.align = align;
    return s;
}

// A rectangle with round corners, in an opaque colour (the corners' discs overlap the sides: invisible when
// opaque).
void roundedRect(ui::DrawList& draw, const ui::Rect& r, f32 radius, u32 color) {
    radius = std::max(0.0f, std::min(radius, std::min(r.w, r.h) * 0.5f));
    if (radius < 1.0f) {
        draw.solid(r, color);
        return;
    }
    draw.solid({r.x + radius, r.y, r.w - 2.0f * radius, r.h}, color);
    draw.solid({r.x, r.y + radius, radius, r.h - 2.0f * radius}, color);
    draw.solid({r.right() - radius, r.y + radius, radius, r.h - 2.0f * radius}, color);
    draw.ring(r.x + radius, r.y + radius, radius, 0.0f, color);
    draw.ring(r.right() - radius, r.y + radius, radius, 0.0f, color);
    draw.ring(r.x + radius, r.bottom() - radius, radius, 0.0f, color);
    draw.ring(r.right() - radius, r.bottom() - radius, radius, 0.0f, color);
}

void framedRect(ui::DrawList& draw, const ui::Rect& r, f32 radius, f32 border, u32 borderColor, u32 fill) {
    roundedRect(draw, r, radius, borderColor);
    roundedRect(draw, r.inset(border), radius - border, fill);
}

bool drawButton(ui::Gui& g, ui::WidgetId id, const ui::Rect& r, const FirstRunButton& button, bool withNote) {
    const f32 dp = g.px(1.0f);
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool down = button.enabled && ui::held(state, r);
    u32 fill = button.primary ? (down ? kPrimaryDown : kPrimary) : (down ? kSecondaryDown : kSecondary);
    u32 border = button.primary ? kPrimaryBorder : kSecondaryBorder;
    u32 color = kText;
    if (!button.enabled) {
        fill = kDisabledFill;
        border = kDisabledBorder;
        color = kDisabledText;
    }
    framedRect(g.draw, r, 10.0f * dp, std::max(1.0f, 1.5f * dp), border, fill);
    const bool note = withNote && !button.note.empty();
    const f32 labelSize = kLabelDp * dp;
    const f32 noteSize = kNoteDp * dp;
    const f32 gap = 3.0f * dp;
    const f32 height = note ? labelSize + gap + noteSize : labelSize;
    const f32 top = r.centerY() - height * 0.5f;
    const f32 inset = 14.0f * dp;
    ui::label(g, {r.x + inset, top, r.w - 2.0f * inset, labelSize}, button.label,
              style(labelSize, button.primary && button.enabled ? ui::kWhite : color, ui::Align::Center));
    if (note) {
        ui::label(g, {r.x + inset, top + labelSize + gap, r.w - 2.0f * inset, noteSize}, button.note,
                  style(noteSize, button.enabled ? kDim : kDisabledText, ui::Align::Center));
    }
    return button.enabled && ui::tapped(state);
}

// A text field: its label over a framed box with the text (its end in view when long) and a blinking caret.
f32 drawField(ui::Gui& g, ui::WidgetId id, f32 x, f32 y, f32 width, const FirstRunField& field,
              bool& tapped) {
    const f32 dp = g.px(1.0f);
    ui::DrawList& draw = g.draw;
    const ui::TextStyle label = style(kSmallDp * dp, kDim);
    draw.text(x, y + draw.ascent(label.font, label.size), field.label, label);
    const f32 labelHeight = draw.lineHeight(label.font, label.size) + 6.0f * dp;
    const ui::Rect box{x, y + labelHeight, std::min(width, 520.0f * dp), kFieldHeightDp * dp};
    const ui::WidgetState state = ui::hit(g, id, box);
    tapped = ui::tapped(state);
    framedRect(draw, box, 8.0f * dp, std::max(1.0f, (field.focused ? 2.0f : 1.5f) * dp),
               field.focused ? kPrimaryBorder : kSecondaryBorder, kField);
    const bool empty = field.text.empty();
    const ui::TextStyle text = style(kFieldDp * dp, empty ? kDisabledText : kText);
    const std::string_view shown = empty ? field.placeholder : field.text;
    const f32 inner = 14.0f * dp;
    const f32 room = box.w - 2.0f * inner - 4.0f * dp;
    const f32 width0 = draw.measure(shown, text.font, text.size);
    const f32 tx = box.x + inner - std::max(0.0f, width0 - room);
    const f32 baseline = ui::centeredBaseline(draw, text, box.centerY());
    draw.pushClip(box.inset(inner * 0.5f, 0.0f));
    draw.text(tx, baseline, shown, text);
    if (field.focused && (g.ui.nowMs() / 530) % 2 == 0) {
        const f32 caretX = empty ? box.x + inner : tx + width0 + 1.0f * dp;
        draw.solid({caretX, box.y + 12.0f * dp, std::max(1.0f, 2.0f * dp), box.h - 24.0f * dp}, kName);
    }
    draw.popClip();
    return labelHeight + box.h;
}

f32 drawProgress(ui::Gui& g, f32 x, f32 y, f32 width, const FirstRunPageView& v) {
    const f32 dp = g.px(1.0f);
    ui::DrawList& draw = g.draw;
    const f32 start = y;
    if (!v.progressTop.empty()) {
        const ui::TextStyle top = style(kBodyDp * dp, kText);
        ui::label(g, {x, y, width, top.size}, v.progressTop, top);
        y += top.size + 8.0f * dp;
    }
    const ui::Rect bar{x, y, width, 14.0f * dp};
    roundedRect(draw, bar, bar.h * 0.5f, kTrack);
    if (v.indeterminate) {
        const f32 t = static_cast<f32>(g.ui.nowMs() % 1600) / 1600.0f;
        const f32 segment = width * 0.3f;
        draw.pushClip(bar);
        roundedRect(draw, {x - segment + (width + segment) * t, bar.y, segment, bar.h}, bar.h * 0.5f, kBar);
        draw.popClip();
    } else if (v.fraction > 0.0f) {
        roundedRect(draw, {x, bar.y, std::max(bar.h, width * std::clamp(v.fraction, 0.0f, 1.0f)), bar.h},
                    bar.h * 0.5f, kBar);
    }
    y += bar.h + 8.0f * dp;
    if (!v.progressLeft.empty() || !v.progressRight.empty()) {
        const ui::TextStyle left = style(kSmallDp * dp, kDim);
        const ui::TextStyle right = style(kSmallDp * dp, kDim, ui::Align::Right);
        const f32 leftWidth = draw.measure(v.progressLeft, left.font, left.size);
        const f32 rightWidth = draw.measure(v.progressRight, right.font, right.size);
        const f32 line = draw.lineHeight(left.font, left.size);
        draw.text(x, y + draw.ascent(left.font, left.size), v.progressLeft, left);
        if (leftWidth + rightWidth + 16.0f * dp > width) {
            y += line; // no room side by side: under it
        }
        draw.text(x + width, y + draw.ascent(right.font, right.size), v.progressRight, right);
        y += line;
    }
    return y - start;
}

// Three dots that pulse one after the other: the PC is being asked.
f32 drawBusy(ui::Gui& g, f32 x, f32 y) {
    const f32 dp = g.px(1.0f);
    const f32 t = static_cast<f32>(g.ui.nowMs() % 1200) / 1200.0f;
    for (u32 i = 0; i < 3; ++i) {
        const f32 phase = t * 6.2831853f - static_cast<f32>(i) * 0.9f;
        const f32 a = 0.3f + 0.7f * std::max(0.0f, std::sin(phase));
        g.draw.ring(x + (6.0f + 18.0f * static_cast<f32>(i)) * dp, y + 8.0f * dp, 5.0f * dp, 0.0f,
                    ui::withAlpha(kName, a));
    }
    return 16.0f * dp;
}

} // namespace

FirstRunInput drawFirstRunPage(DialogContext& c, const FirstRunPageView& v) {
    FirstRunInput input;
    ui::Gui g{c.ui, c.draw};
    ui::DrawList& draw = c.draw;
    const f32 dp = c.ui.px(1.0f);
    draw.solid({0.0f, 0.0f, c.ui.width(), c.ui.height()}, kBackground); // nothing of an (empty) world shows

    const ui::Rect& safe = c.safeArea;
    // A short area (the cover screen with the keyboard up): a smaller header, less margin.
    const bool compact = safe.h < 260.0f * dp;
    const f32 pad = compact ? 8.0f * dp : std::clamp(safe.h * 0.045f, 12.0f * dp, 26.0f * dp);
    // The header: the app's name, the page's title, the version.
    const ui::Rect header{safe.x + pad, safe.y + pad, safe.w - 2.0f * pad, (compact ? 26.0f : 34.0f) * dp};
    const ui::TextStyle name = style((compact ? 20.0f : kNameDp) * dp, kName);
    const f32 nameWidth = draw.text(header.x, ui::centeredBaseline(draw, name, header.centerY()),
                                    text::get("VD_APP_NAME"), name);
    f32 right = header.right();
    if (!v.version.empty()) {
        const ui::TextStyle version = style(kSmallDp * dp, kDim, ui::Align::Right);
        right -= draw.text(header.right(), ui::centeredBaseline(draw, version, header.centerY()), v.version,
                           version) +
                 12.0f * dp;
    }
    if (!v.title.empty()) {
        const f32 x = header.x + nameWidth + 12.0f * dp;
        draw.ring(x + 3.0f * dp, header.centerY(), 3.0f * dp, 0.0f, kPrimaryBorder);
        ui::label(g, {x + 14.0f * dp, header.y, std::max(0.0f, right - x - 14.0f * dp), header.h}, v.title,
                  style((compact ? 17.0f : kTitleDp) * dp, kText));
    }
    draw.solid({header.x, header.bottom() + (compact ? 4.0f : 8.0f) * dp, header.w, std::max(1.0f, dp)}, kRule);

    // The explanation and the buttons: side by side on wide screens, the buttons under it on tall ones.
    const f32 contentTop = header.bottom() + (compact ? 10.0f : 20.0f) * dp;
    const ui::Rect content{header.x, contentTop, header.w, std::max(0.0f, safe.bottom() - pad - contentTop)};
    const usize count = v.buttons.size();
    const f32 gap = kGapDp * dp;
    const bool notes = std::any_of(v.buttons.begin(), v.buttons.end(),
                                   [](const FirstRunButton& b) { return !b.note.empty(); });
    const bool wide = content.w >= content.h * 1.15f && content.w >= 520.0f * dp;
    ui::Rect textPane;
    ui::Rect actions;
    f32 buttonHeight = 0.0f;
    if (wide) {
        const f32 actionsWidth = std::clamp(content.w * 0.38f, 240.0f * dp, 420.0f * dp);
        actions = {content.right() - actionsWidth, content.y, actionsWidth, content.h};
        textPane = {content.x, content.y, content.w - actionsWidth - 28.0f * dp, content.h};
        const f32 fit =
            count > 0 ? (actions.h - gap * static_cast<f32>(count - 1)) / static_cast<f32>(count) : 0.0f;
        buttonHeight = std::clamp(fit, kMinButtonDp * dp, (notes ? 76.0f : 62.0f) * dp);
    } else {
        buttonHeight = (notes ? 68.0f : 58.0f) * dp;
        const f32 height =
            count > 0 ? buttonHeight * static_cast<f32>(count) + gap * static_cast<f32>(count - 1) : 0.0f;
        const f32 width = std::min(content.w, 560.0f * dp);
        actions = {content.x + (content.w - width) * 0.5f, std::max(content.y, content.bottom() - height),
                   width, std::min(height, content.h)};
        const f32 textWidth = std::min(content.w, 680.0f * dp);
        textPane = {content.x + (content.w - textWidth) * 0.5f, content.y, textWidth,
                    std::max(0.0f, actions.y - 18.0f * dp - content.y)};
    }

    // The buttons (they scroll too in the rare page with more than fit).
    const f32 buttonsHeight =
        count > 0 ? buttonHeight * static_cast<f32>(count) + gap * static_cast<f32>(count - 1) : 0.0f;
    const bool scrollButtons = buttonsHeight > actions.h + 0.5f;
    ui::ScrollView buttonView;
    f32 by = actions.y;
    if (scrollButtons) {
        buttonView = ui::beginScroll(g, ui::widgetId("firstrun.buttons"), actions);
        by = buttonView.top;
    }
    for (usize i = 0; i < count; ++i) {
        const ui::Rect r{actions.x, by, actions.w, buttonHeight};
        if (drawButton(g, ui::widgetId("firstrun.button", i), r, v.buttons[i],
                       buttonHeight >= kNoteButtonDp * dp)) {
            input.button = static_cast<i32>(i);
        }
        by += buttonHeight + gap;
    }
    if (scrollButtons) {
        ui::endScroll(g, buttonView, buttonsHeight);
    }

    // The explanation's column, scrolling when it does not fit.
    const ui::WidgetId viewId = ui::widgetId("firstrun.text", stringHash(v.title));
    const ui::ScrollView view = ui::beginScroll(g, viewId, textPane);
    const f32 x = textPane.x;
    const f32 width = std::max(0.0f, textPane.w - 12.0f * dp); // room for the scroll indicator
    f32 y = view.top;
    i32 focused = -1;
    f32 focusedTop = 0.0f, focusedBottom = 0.0f; // in the content, from its top
    for (usize i = 0; i < v.fields.size(); ++i) {
        bool tapped = false;
        const f32 top = y;
        y += drawField(g, ui::widgetId("firstrun.field", i), x, y, width, v.fields[i], tapped);
        if (v.fields[i].focused) {
            focused = static_cast<i32>(i);
            focusedTop = top - view.top;
            focusedBottom = y - view.top;
        }
        y += 14.0f * dp;
        if (tapped) {
            input.field = static_cast<i32>(i);
        }
    }
    if (v.progress) {
        y += drawProgress(g, x, y, width, v) + 14.0f * dp;
    }
    if (v.busy) {
        y += drawBusy(g, x, y) + 12.0f * dp;
    }
    if (!v.body.empty()) {
        y += draw.wrappedText({x, y, width, 1.0e6f}, v.body, style(kBodyDp * dp, kText)) + 14.0f * dp;
    }
    if (v.hasCheck) {
        const f32 box = 26.0f * dp;
        const ui::TextStyle label = style(kBodyDp * dp, kText);
        const f32 labelWidth = std::max(0.0f, width - box - 14.0f * dp);
        const f32 labelHeight = draw.measureWrapped(labelWidth, v.checkLabel, label.font, label.size);
        const ui::Rect row{x, y, width, std::max(box, labelHeight)};
        const ui::WidgetState state = ui::hit(g, ui::widgetId("firstrun.check"), row);
        if (ui::tapped(state)) {
            input.toggled = true;
        }
        const ui::Rect square{x, y + std::max(0.0f, (std::min(row.h, 2.0f * box) - box) * 0.5f), box, box};
        framedRect(draw, square, 5.0f * dp, std::max(1.0f, 1.5f * dp),
                   v.checked ? kPrimaryBorder : kSecondaryBorder, kField);
        if (v.checked) {
            roundedRect(draw, square.inset(6.0f * dp), 3.0f * dp, kPrimaryBorder);
        }
        draw.wrappedText({x + box + 14.0f * dp, y, labelWidth, 1.0e6f}, v.checkLabel, label);
        y += row.h + 14.0f * dp;
    }
    if (!v.status.empty()) {
        y += draw.wrappedText({x, y, width, 1.0e6f}, v.status,
                              style(kBodyDp * dp, v.statusError ? kError : kDim)) +
             14.0f * dp;
    }
    if (!v.body2.empty()) {
        y += draw.wrappedText({x, y, width, 1.0e6f}, v.body2, style(kSmallDp * dp, kDim)) + 8.0f * dp;
    }
    ui::endScroll(g, view, y - view.top);
    // The field being typed into comes into view when it gets the keyboard or the keyboard takes the room (once:
    // then the finger may scroll elsewhere). After endScroll, which knows this frame's sizes.
    static i32 s_focused = -1;
    static f32 s_viewHeight = 0.0f;
    if (focused >= 0 && (focused != s_focused || textPane.h != s_viewHeight)) {
        if (focusedBottom - view.offset > textPane.h) {
            ui::scrollTo(viewId, focusedBottom - textPane.h + 6.0f * dp);
        } else if (focusedTop < view.offset) {
            ui::scrollTo(viewId, focusedTop);
        }
    }
    s_focused = focused;
    s_viewHeight = textPane.h;
    return input;
}

} // namespace vd
