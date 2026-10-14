#pragma once

#include "core/Types.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 18 (p3): a row of column headers that sort their list, as WhoFrameColumnHeaderTemplate (FriendsFrame.xml) and
// AuctionSortButtonTemplate (Blizzard_AuctionUITemplates.xml) draw them: WhoFrame-ColumnTabs in three slices, the
// label in GameFontHighlightSmall, UI-Character-Tab-Highlight while pressed; the auction house's with UI-SortArrow
// right of the label (down, or up while that key is reversed: SortButton_UpdateArrow). A column may carry a dropdown
// (WhoFrameDropDown: which of zone, guild and race the column shows): its arrow button at the right end opens it, as
// does a long press anywhere on the header. Pure view: tap results out.
struct SortHeaderView {
    std::string_view text;
    f32 weight = 1.0f;     // the column's share of the row (the FrameXML widths)
    bool arrow = false;    // UI-SortArrow (the auction house)
    bool reversed = false; // ...pointing up
    bool menu = false;     // a dropdown button at the right end
};
struct SortHeaderResult {
    i32 tapped = -1; // the header tapped: sort by it
    i32 menu = -1;   // its dropdown asked for
};

// The columns' edges across `r`: edges[i]..edges[i + 1] is column i (`edges` holds headers.size() + 1).
void sortColumns(const ui::Rect& r, std::span<const SortHeaderView> headers, std::span<f32> edges);
// The headers across `r` (its height: the headers'), laid out by sortColumns.
SortHeaderResult sortHeaders(ui::Gui& g, std::string_view id, const ui::Rect& r, std::span<const SortHeaderView> headers,
                             f32 textSize);

} // namespace vd
