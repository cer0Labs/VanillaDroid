#include "app/SortHeaders.h"

#include "core/Hash.h"
#include "core/Profile.h"

#include <algorithm>
#include <array>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr u64 kColumnTabs = assetPathHash("Interface\\FriendsFrame\\WhoFrame-ColumnTabs.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
constexpr u64 kSortArrow = assetPathHash("Interface\\Buttons\\UI-SortArrow.blp");
constexpr u64 kDropDownUp = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");
constexpr u64 kDropDownDown = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Down.blp");

} // namespace

void sortColumns(const Rect& r, std::span<const SortHeaderView> headers, std::span<f32> edges) {
    if (edges.empty()) {
        return;
    }
    f32 total = 0.0f;
    for (const SortHeaderView& h : headers) {
        total += std::max(h.weight, 0.0f);
    }
    const usize n = std::min(headers.size(), edges.size() - 1);
    edges[0] = r.x;
    for (usize i = 0; i < n; ++i) {
        const f32 share = total > 0.0f ? std::max(headers[i].weight, 0.0f) / total : 0.0f;
        edges[i + 1] = edges[i] + r.w * share;
    }
    if (n > 0) {
        edges[n] = r.right(); // no rounding gap at the end
    }
}

SortHeaderResult sortHeaders(ui::Gui& g, std::string_view id, const Rect& r, std::span<const SortHeaderView> headers,
                             f32 textSize) {
    VD_PROFILE_ZONE("SortHeaders::draw");
    SortHeaderResult result;
    constexpr usize kMax = 12;
    std::array<f32, kMax + 1> edges{};
    const usize n = std::min(headers.size(), kMax);
    sortColumns(r, headers.first(n), edges);
    for (usize i = 0; i < n; ++i) {
        const SortHeaderView& h = headers[i];
        // The tabs overlap by two of their units (the FrameXML anchors them at x = -2).
        const Rect cell{edges[i], r.y, edges[i + 1] - edges[i] + (i + 1 < n ? g.px(2.0f) : 0.0f), r.h};
        const f32 menuW = h.menu ? std::min(r.h, cell.w * 0.4f) : 0.0f;
        const Rect sortArea{cell.x, cell.y, cell.w - menuW, cell.h};
        const ui::WidgetId wid = ui::widgetId(id, i * 2);
        const ui::WidgetState state = ui::hit(g, wid, sortArea);
        ui::threeSlice(g.draw, cell, kColumnTabs, {0.0f, 0.0f, 0.96875f, 0.75f}, 0.078125f / 0.96875f, g.px(5.0f));
        if (ui::held(state, sortArea)) {
            g.draw.sprite(sortArea, kTabHighlight, rgba(255, 255, 255, 200));
        }
        if (ui::tapped(state)) {
            result.tapped = static_cast<i32>(i);
        }
        if (state.longPress && h.menu) {
            result.menu = static_cast<i32>(i);
        }
        // The label (clipped to its column), the arrow right of it.
        const f32 pad = g.px(8.0f);
        const ui::TextStyle style = ui::gameFont(textSize, ui::kWhite);
        const f32 arrowW = h.arrow ? g.px(9.0f) + g.px(3.0f) : 0.0f;
        const f32 room = std::max(0.0f, sortArea.w - pad - g.px(3.0f) - arrowW);
        const f32 textW = std::min(g.draw.measure(h.text, style.font, style.size), room);
        g.draw.pushClip({sortArea.x, sortArea.y, sortArea.w - arrowW, sortArea.h});
        ui::label(g, {sortArea.x + pad, sortArea.y, room, sortArea.h}, h.text, style);
        g.draw.popClip();
        if (h.arrow) {
            const f32 aw = g.px(9.0f), ah = g.px(8.0f);
            const Rect a{sortArea.x + pad + textW + g.px(3.0f), sortArea.centerY() - ah * 0.5f + g.px(2.0f), aw, ah};
            g.draw.sprite(a, kSortArrow, ui::kWhite,
                          h.reversed ? ui::TexCoords{0.0f, 1.0f, 0.5625f, 0.0f} : ui::TexCoords{0.0f, 0.0f, 0.5625f, 1.0f});
        }
        if (h.menu) {
            const Rect button{cell.right() - menuW, cell.y, menuW, cell.h};
            const ui::WidgetState ms = ui::hit(g, ui::widgetId(id, i * 2 + 1), button);
            const f32 s = std::min(button.w, button.h);
            g.draw.sprite({button.centerX() - s * 0.5f, button.centerY() - s * 0.5f, s, s},
                          ui::held(ms, button) ? kDropDownDown : kDropDownUp);
            if (ui::tapped(ms) || ms.longPress) {
                result.menu = static_cast<i32>(i);
            }
        }
    }
    return result;
}

} // namespace vd
