#include "app/ChatFrame.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/ChatDock.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vd {
namespace {

using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;

constexpr u64 kBackground = assetPathHash("Interface\\ChatFrame\\ChatFrameBackground.blp");
constexpr u64 kTab = assetPathHash("Interface\\ChatFrame\\ChatFrameTab.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
constexpr u64 kInputLeft = assetPathHash("Interface\\ChatFrame\\UI-ChatInputBorder-Left.blp");
constexpr u64 kInputRight = assetPathHash("Interface\\ChatFrame\\UI-ChatInputBorder-Right.blp");
constexpr u64 kLinkHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");

constexpr f32 kDefaultAlpha =
    0.25f; // DEFAULT_CHATFRAME_ALPHA: the background while the mouse is over the frame
constexpr f32 kExpandedAlpha = 0.5f; // ours: the open chat is read over the world
constexpr u64 kWorldTapMs = 350;     // a tap that goes through to the world (app/Hud's)
constexpr usize kMaxExpandedLines = 256;
constexpr u32 kMaxLinkRects = 4;
// A double tap (Android's ViewConfiguration): the second finger down at most DOUBLE_TAP_TIMEOUT after the
// first one lifted, within DOUBLE_TAP_SLOP of where it went down.
constexpr u64 kDoubleTapMs = 300;
constexpr f32 kDoubleTapSlopDp = 100.0f;

// A tap on the chat (`t`, lifting now): whether it is the second of a double tap (else it may be the first).
bool doubleTap(const Gui& g, ChatFrameState& state, const ui::Touch& t) {
    const f32 slop = g.px(kDoubleTapSlopDp);
    const bool second = state.tapMs != 0 &&
                        static_cast<i64>(t.downMs - state.tapMs) <= static_cast<i64>(kDoubleTapMs) &&
                        std::abs(t.startX - state.tapX) <= slop && std::abs(t.startY - state.tapY) <= slop;
    state.tapMs = second ? 0 : g.ui.nowMs();
    state.tapX = t.startX;
    state.tapY = t.startY;
    return second;
}

// A rectangle grown to a finger's size.
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

// The edit box (ChatFrameEditBoxTemplate): UI-ChatInputBorder-Left's left end, stretched,
// UI-ChatInputBorder-Right's right end; the header in the chat type's colour 13 units in, the text after it
// in the same colour.
void editBox(Gui& g, const ChatFrameMetrics& m, const Rect& r, const ChatFrameView& v,
             ChatFrameResult& result) {
    const f32 cap = r.h * 0.5f; // 16 of the textures' 32 texels
    const u32 tint = ui::kWhite;
    g.draw.sprite({r.x, r.y, cap, r.h}, kInputLeft, tint, {0.0f, 0.0f, 0.0625f, 1.0f});
    g.draw.sprite({r.x + cap, r.y, r.w - 2.0f * cap, r.h}, kInputLeft, tint, {0.0625f, 0.0f, 0.9375f, 1.0f});
    g.draw.sprite({r.right() - cap, r.y, cap, r.h}, kInputRight, tint, {0.9375f, 0.0f, 1.0f, 1.0f});
    TextStyle style = ui::chatFont(std::min(g.px(16.0f), r.h * 0.42f), v.headerColor);
    const f32 baseline = ui::centeredBaseline(g.draw, style, r.centerY());
    const f32 left = r.x + 13.0f * m.unit + g.px(2.0f);
    const f32 headerW = g.draw.measure(v.header, style.font, style.size);
    const Rect inner{left + headerW, r.y, std::max(0.0f, r.right() - cap - left - headerW), r.h};
    // The header picks where the text goes; the rest types.
    const Rect headerHit{r.x, r.y, inner.x - r.x, r.h};
    const ui::WidgetState headerState = ui::hit(g, widgetId("chat.header"), headerHit);
    const ui::WidgetState textState = ui::hit(g, widgetId("chat.input"), inner);
    if (ui::held(headerState, headerHit)) {
        g.draw.spriteAdditive(headerHit.inset(cap * 0.3f, r.h * 0.18f), kLinkHighlight,
                              rgba(255, 255, 255, 140));
    }
    result.pickChannel = ui::tapped(headerState);
    result.tappedInput = ui::tapped(textState);
    g.draw.text(left, baseline, v.header, style);
    const f32 caretWidth = std::max(1.0f, g.px(1.5f));
    g.draw.pushClip(inner);
    f32 end = inner.x;
    if (v.input.empty()) {
        TextStyle hint = style;
        hint.color = ui::kDisabledText;
        hint.shadow = 0.0f;
        g.draw.text(inner.x + g.px(2.0f), baseline, v.hint, hint);
    } else {
        // The end of a long text stays in sight.
        const f32 width = g.draw.measure(v.input, style.font, style.size);
        const f32 x =
            width > inner.w - caretWidth * 3.0f ? inner.right() - caretWidth * 3.0f - width : inner.x;
        g.draw.text(x, baseline, v.input, style);
        end = x + width;
    }
    if (v.editing && (g.ui.nowMs() / 500) % 2 == 0) {
        g.draw.solid({end + caretWidth, r.centerY() - style.size * 0.5f, caretWidth, style.size}, ui::kWhite);
    }
    g.draw.popClip();
}

} // namespace

ChatFrameMetrics chatFrameMetrics(const DialogContext& c, f32 fontScale) {
    const bool wide = c.profile == ScreenProfile::Wide;
    ChatFrameMetrics m;
    m.unit = c.ui.px(ui::kUnitDp);
    m.font = c.ui.px(wide ? 13.5f : 14.5f) * fontScale;
    m.lineHeight = c.draw.lineHeight(ui::FontId::Arial, m.font);
    m.tabHeight = std::max(32.0f * m.unit, c.ui.px(30.0f));
    m.tabFont = c.ui.px(wide ? 12.0f : 12.5f);
    m.editHeight = c.ui.px(40.0f);
    return m;
}

Rect chatFrameRect(const DialogContext& c, bool expanded, const Rect& actionBar, const Rect& avoid,
                   bool keyboard, f32 fontScale) {
    const ChatFrameMetrics m = chatFrameMetrics(c, fontScale);
    const Rect& safe = c.safeArea;
    const bool wide = c.profile == ScreenProfile::Wide;
    const f32 u = m.unit;
    const f32 margin = c.ui.px(8.0f);
    // The background (2 units out left of the lines) a hair from the screen's left edge, where the 1.12's
    // chat-menu and scroll buttons were (phase 20: not shown); the action buttons'
    // cluster on the right is never covered. Phase 20 (look): the edge is the screen's where no camera hole
    // reaches the chat between `top` and `bottom` (c.edges), not the safe area's whole side.
    const f32 gap = c.ui.px(2.0f);
    const auto frameLeft = [&](f32 top, f32 bottom) { return c.edges.left(top, bottom) + gap + 2.0f * u; };
    const f32 right = actionBar.w > 0.0f ? actionBar.x - margin : safe.right() - margin;
    if (!expanded) {
        // ChatFrame1: 430 units wide, its bottom 95 units over the screen's (the main menu bar is there in
        // the client). Here there is no menu bar down there: the frame sits in the bottom left corner, its
        // background (6 units under the lines) on the experience bar and the reputation watch bar over it
        // (13 + 8 units, app/Hud). Phase 20: only the last kChatCollapsedLines lines (not the 120 units',
        // about seven), no tabs over them. The movement stick, which floats where the thumb lands, still
        // starts over it: at rest only its links take a finger.
        const f32 h = static_cast<f32>(kChatCollapsedLines) * m.lineHeight;
        const f32 bottom = safe.bottom() - (13.0f + 8.0f + 6.0f + 2.0f) * u;
        const f32 top = bottom - h - 3.0f * u;
        f32 x = frameLeft(top, bottom + 6.0f * u);
        if (avoid.w > 0.0f && avoid.h > 0.0f && avoid.bottom() > top && avoid.y < bottom &&
            avoid.x < x + 430.0f * u) {
            x = std::max(x, avoid.right() + 2.0f * u + c.ui.px(6.0f)); // the party frames stay in sight
        }
        const f32 w = std::max(0.0f, std::min(430.0f * u, right - x));
        return {x, bottom - h, w, h};
    }
    // Open: as wide as fits left of the action buttons (a readable measure), from above the edit box up to
    // under the unit frames (100 units), or over them while the keyboard takes the bottom of the screen.
    const f32 editRow = m.editHeight + 4.0f * u;
    const f32 bottom = safe.bottom() - c.ui.px(6.0f) - editRow;
    const f32 limit =
        (keyboard ? safe.y + c.ui.px(4.0f) : safe.y + 100.0f * u + c.ui.px(4.0f)) + m.tabHeight + 3.0f * u;
    const f32 tall = wide ? c.ui.px(300.0f) : std::min(safe.h * 0.5f, c.ui.px(420.0f));
    const f32 top = std::min(std::max(limit, bottom - tall), bottom - 2.0f * m.lineHeight);
    const f32 x = frameLeft(top - 3.0f * u - m.tabHeight, bottom + editRow);
    const f32 w = std::max(
        0.0f, std::min({right - x, c.ui.px(wide ? 540.0f : 480.0f), safe.w * (wide ? 0.62f : 0.66f)}));
    return {x, top, w, bottom - top};
}

Rect chatFrameBounds(const DialogContext& c, const Rect& frame, bool expanded) {
    const ChatFrameMetrics m = chatFrameMetrics(c);
    const f32 u = m.unit;
    const f32 left = frame.x - 2.0f * u;
    const f32 top = expanded ? frame.y - 3.0f * u - m.tabHeight : frame.y - 3.0f * u;
    const f32 bottom = expanded ? frame.bottom() + 4.0f * u + m.editHeight : frame.bottom() + 6.0f * u;
    return {left, top, frame.right() + 3.0f * u - left, bottom - top};
}

ChatFrameResult chatFrame(DialogContext& c, const Rect& frame, const ChatFrameView& v,
                          ChatFrameState& state) {
    VD_PROFILE_ZONE("ChatFrame::draw");
    Gui g{c.ui, c.draw};
    const ChatFrameMetrics m = chatFrameMetrics(c, v.fontScale); // phase 18 (p3): FCF_SetChatWindowFontSize
    const f32 u = m.unit;
    const u64 now = g.ui.nowMs();
    ChatFrameResult result;
    if (frame.w <= 0.0f || frame.h <= 0.0f) {
        return result;
    }
    TextStyle style = ui::chatFont(m.font);
    // FloatingChatFrameTemplate's background: 2 units out left, 3 up, 3 right, 6 down.
    const Rect background{frame.x - 2.0f * u, frame.y - 3.0f * u, frame.w + 5.0f * u, frame.h + 9.0f * u};
    // A tap on the frame (its lines, their links, its empty parts; not the tabs): the second of a double tap
    // opens or closes the chat. A line's or a link's own tap (`kind`) waits until no second one can come
    // (`due`).
    const auto frameTap = [&](const ui::Touch& t) {
        if (doubleTap(g, state, t)) {
            result.toggle = true;
            state.pending = ChatPendingTap::None;
        }
    };
    const auto pend = [&](const ui::Touch& t, ChatPendingTap kind, u32 key, u16 begin, u32 span) {
        state.pending = kind;
        state.pendingKey = key;
        state.pendingBegin = begin;
        state.pendingSpan = span;
        state.pendingMs = now;
        frameTap(t);
    };
    const auto due = [&](ChatPendingTap kind, u32 key, u16 begin, u32 span) {
        const bool act = state.pending == kind && state.pendingKey == key && state.pendingBegin == begin &&
                         state.pendingSpan == span && now - state.pendingMs > kDoubleTapMs;
        if (act) {
            state.pending = ChatPendingTap::None;
        }
        return act;
    };
    if (v.expanded) {
        // Open, no finger reaches the world.
        const ui::WidgetState area = ui::hit(g, widgetId("chat.area"), chatFrameBounds(c, frame, true));
        if (ui::tapped(area) && background.contains(area.touch->startX, area.touch->startY)) {
            frameTap(*area.touch);
        }
    }
    // Phase 18 (p3): the window's own colour and alpha (FCF_SetWindowColor / Alpha); a touch fades it in to
    // DEFAULT_CHATFRAME_ALPHA when it is lower (FCF_OnUpdate's mouse-over).
    const f32 backgroundAlpha =
        v.expanded ? std::max(kExpandedAlpha, v.restAlpha) : std::max(v.restAlpha, kDefaultAlpha * v.backgroundAlpha);
    if (backgroundAlpha > 0.004f) {
        g.draw.sprite(background, kBackground, ui::withAlpha(v.backgroundColor, backgroundAlpha));
    }

    // The tabs of the docked frames over the background's top left (FCF_SetTabPosition: 2 units in), each as
    // wide as its name (FCF_DockUpdate); the selected one at full alpha, the others at half. Phase 20: only
    // with the chat open.
    if (v.expanded && !v.tabs.empty()) {
        constexpr usize kMaxTabs = 8;
        std::array<f32, kMaxTabs> widths{};
        const u32 count = static_cast<u32>(std::min(v.tabs.size(), kMaxTabs));
        const f32 cap = m.tabHeight * 0.5f; // the 16-texel ends of the 32-texel-high ChatFrameTab
        for (u32 i = 0; i < count; ++i) {
            widths[i] = g.draw.measure(v.tabs[i].name, ui::FontId::Friz, m.tabFont) + 5.0f * u + 2.0f * cap;
        }
        ui::dockTabWidths({widths.data(), count}, background.w - 2.0f * u);
        f32 x = background.x + 2.0f * u;
        const f32 y = background.y - m.tabHeight;
        for (u32 i = 0; i < count; ++i) {
            const Rect r{x, y, widths[i], m.tabHeight};
            x += widths[i];
            const f32 alpha = i == v.selected ? 1.0f : 0.5f;
            ui::threeSlice(g.draw, r, kTab, {}, 0.25f, cap, ui::withAlpha(ui::kWhite, alpha));
            // The tab's art is the texture's lower part: its name sits 5 units under the middle, its flash
            // and highlight 7 units down.
            const Rect lit{r.x, r.y + m.tabHeight * 7.0f / 32.0f, r.w, m.tabHeight};
            if (v.tabs[i].flash > 0.0f) {
                g.draw.spriteAdditive(lit, kTabHighlight, ui::withAlpha(ui::kWhite, v.tabs[i].flash));
            }
            const Rect target = touchTarget(g, r);
            const ui::WidgetState s = ui::hit(g, widgetId("chat.tab", i), target);
            if (ui::held(s, target)) {
                g.draw.spriteAdditive(lit, kTabHighlight, rgba(255, 255, 255, 200));
            }
            if (ui::tapped(s)) {
                result.tab = static_cast<i32>(i);
            }
            if (s.longPress) {
                result.tabMenu = static_cast<i32>(i);
            }
            result.touched = result.touched || s.pressed;
            TextStyle name = ui::gameFont(m.tabFont, ui::withAlpha(ui::kGold, alpha));
            name.align = ui::Align::Center;
            ui::label(g, {r.x + cap * 0.75f, r.y + m.tabHeight * 5.0f / 32.0f, r.w - cap * 1.5f, m.tabHeight},
                      v.tabs[i].name, name);
        }
    }

    // The lines. A line's links are widgets of their own (over the line's), so a finger on one opens it.
    std::array<Rect, kMaxLinkRects> spans;
    const auto links = [&](const ChatFrameLine& line, u32 index, f32 x, f32 y, f32 width) {
        for (u32 k = 0; k < line.links.size(); ++k) {
            const ChatFrameLink& link = line.links[k];
            const u32 n =
                g.draw.wrappedSpan(width, line.text, style.font, style.size, link.begin, link.end, spans);
            for (u32 s = 0; s < n; ++s) {
                // A link's height a finger can find (it may reach the next line's: the newer one wins).
                const Rect r{x + spans[s].x, y + spans[s].y, spans[s].w, spans[s].h};
                const Rect target = r.inset(-g.px(3.0f), -std::max(0.0f, (g.px(28.0f) - r.h) * 0.5f));
                const u64 id = (static_cast<u64>(line.key) << 8) | (static_cast<u64>(k) << 2) | s;
                const ui::WidgetState ws = ui::hit(g, widgetId("chat.link", id), target);
                if (ui::held(ws, target)) {
                    g.draw.sprite(r, kLinkHighlight, rgba(255, 255, 255, 110));
                }
                if (ui::tapped(ws)) {
                    pend(*ws.touch, ChatPendingTap::Link, line.key, link.begin, s);
                }
                if (due(ChatPendingTap::Link, line.key, link.begin, s)) {
                    result.linkLine = static_cast<i32>(index);
                    result.linkBegin = link.begin;
                    result.linkRect = r;
                }
                if (ws.longPress) { // phase 13 (social): the right click
                    result.linkHeldLine = static_cast<i32>(index);
                    result.linkHeldBegin = link.begin;
                    result.linkHeldRect = r;
                }
                result.touched = result.touched || ws.pressed;
            }
        }
    };
    if (v.expanded) {
        // Open: every line, scrolled with a finger, the view staying on the newest while it is there.
        const WidgetId linesId = widgetId("chat.lines");
        const usize first = v.lines.size() > kMaxExpandedLines ? v.lines.size() - kMaxExpandedLines : 0;
        const usize count = v.lines.size() - first;
        const f32 width = frame.w - g.px(6.0f);
        std::array<f32, kMaxExpandedLines> heights;
        f32 total = 0.0f;
        for (usize i = 0; i < count; ++i) {
            heights[i] = g.draw.measureWrapped(width, v.lines[first + i].text, style.font, style.size);
            total += heights[i];
        }
        const ui::ScrollView view = ui::beginScroll(g, linesId, frame, true);
        for (const ui::Touch& t : g.ui.touches()) { // the frame's empty part, above the oldest line
            if (t.owner == linesId && t.released && !t.cancelled && !t.moved && !ui::scrollClaimed(&t)) {
                frameTap(t);
            }
        }
        f32 y = view.top + std::max(0.0f, frame.h - total);
        for (usize i = 0; i < count; ++i) {
            const ChatFrameLine& line = v.lines[first + i];
            if (y + heights[i] >= frame.y && y <= frame.bottom()) {
                const Rect r{frame.x, y, width, heights[i]};
                const ui::WidgetState ws = ui::hit(g, widgetId("chat.line", line.key), r);
                if (ui::held(ws, r)) {
                    g.draw.sprite(r, kLinkHighlight, rgba(255, 255, 255, 70));
                }
                if (ui::tapped(ws)) {
                    pend(*ws.touch, ChatPendingTap::Line, line.key, 0, 0);
                }
                if (due(ChatPendingTap::Line, line.key, 0, 0)) {
                    result.line = static_cast<i32>(first + i);
                    result.lineRect = r;
                }
                links(line, static_cast<u32>(first + i), r.x, r.y, width);
                style.color = line.color;
                g.draw.wrappedText({r.x, r.y, width, heights[i] + 1.0f}, line.text, style);
            }
            y += heights[i];
        }
        ui::endScroll(g, view, y - view.top);
        f32 offset = view.offset;
        total = std::max(total, frame.h);
        if (state.atEnd && frame.h != state.viewport) {
            // Resized (the keyboard came up): it stays on the newest line, as the scroll view would not (it
            // measures the new height against the old content).
            ui::scrollTo(linesId, total);
            offset = total - frame.h;
        }
        state.viewport = frame.h;
        state.atEnd = offset + frame.h >= total - 1.0f;
    } else {
        // At rest: the newest lines from the frame's bottom up, as faded as they are; the rest of the frame
        // lets fingers through to the world (the stick, the camera).
        g.draw.pushClip(frame);
        f32 y = frame.bottom();
        for (usize i = v.lines.size(); i-- > 0 && y > frame.y;) {
            const ChatFrameLine& line = v.lines[i];
            if ((line.color >> 24) == 0) {
                continue; // faded out
            }
            const f32 h = g.draw.measureWrapped(frame.w, line.text, style.font, style.size);
            // Whole lines only: a message that does not fit shows the lines of it that do.
            const f32 room = std::floor((y - frame.y) / m.lineHeight + 0.01f) * m.lineHeight;
            const bool cut = h > room;
            if (room <= 0.0f) {
                break;
            }
            y -= h;
            if (cut) {
                g.draw.pushClip({frame.x, y + h - room, frame.w, room});
            }
            links(line, static_cast<u32>(i), frame.x, y, frame.w);
            style.color = line.color;
            g.draw.wrappedText({frame.x, y, frame.w, h + 1.0f}, line.text, style);
            if (cut) {
                g.draw.popClip();
                break;
            }
        }
        g.draw.popClip();
        // A tap on the frame that no widget took (it went on to the world): the faded lines show; the second
        // of a double tap opens the chat.
        for (const ui::Touch& t : g.ui.touches()) {
            if (t.owner == 0 && t.released && !t.cancelled && !t.moved && now - t.downMs < kWorldTapMs &&
                background.contains(t.startX, t.startY)) {
                result.touched = true;
                frameTap(t);
            }
        }
    }
    // A tap still waiting whose line was not drawn (scrolled away, faded, the chat toggled) is dropped.
    if (state.pending != ChatPendingTap::None && now - state.pendingMs > kDoubleTapMs) {
        state.pending = ChatPendingTap::None;
    }

    // Open: the edit box under the frame (ChatFrameEditBox: 2 units under it, as wide as it), here from the
    // background's left edge, with the emote and send buttons at its right end.
    if (v.expanded) {
        const f32 h = m.editHeight;
        const f32 y = frame.bottom() + 4.0f * u;
        const f32 left = background.x;
        const f32 gap = g.px(4.0f);
        const std::string_view emote = text::get("EMOTE_MESSAGE");
        const std::string_view send = text::get("SEND_LABEL");
        const f32 emoteW = std::max(g.px(64.0f), kit::buttonWidth(g, emote, h) * 0.7f);
        const f32 sendW = std::max(g.px(72.0f), kit::buttonWidth(g, send, h) * 0.7f);
        const Rect box{left, y, frame.right() - left - emoteW - sendW - 2.0f * gap, h};
        editBox(g, m, box, v, result);
        result.emotes = ui::button(g, widgetId("chat.emotes"), {box.right() + gap, y, emoteW, h}, emote);
        result.send = ui::button(g, widgetId("chat.send"), {frame.right() - sendW, y, sendW, h}, send,
                                 !v.input.empty());
    }
    return result;
}

} // namespace vd
