#include "app/TooltipLayout.h"

#include <algorithm>

namespace vd {

TooltipPlacement placeTooltips(const ui::Rect& safe, const ui::Rect& anchor, f32 width, f32 height,
                               std::span<const f32> compareHeights, f32 margin, f32 drop) {
    TooltipPlacement p;
    const u32 count = static_cast<u32>(std::min<usize>(compareHeights.size(), p.compare.size()));
    const f32 left = safe.x + margin, right = safe.right() - margin;
    // Side by side when the three fit, else the comparisons one under the other.
    const bool row = static_cast<f32>(count + 1) * width <= right - left;
    const u32 columns = count == 0 ? 0u : row ? count : 1u;
    const f32 blockWidth = width * static_cast<f32>(columns + 1);
    // The tooltip right of the cell when the block fits there, else left of it (the comparisons further
    // left).
    f32 x = 0.0f;
    bool mirrored = false;
    if (anchor.w <= 0.0f) {
        x = safe.centerX() - blockWidth * 0.5f;
    } else if (anchor.right() + margin + blockWidth <= right) {
        x = anchor.right() + margin;
    } else if (anchor.x - margin - blockWidth >= left) {
        x = anchor.x - margin - width;
        mirrored = true;
    } else {
        // No room on either side: right of the cell when the tooltip alone fits there (as before the
        // comparisons), else left of it; the block then moves into the screen over the cell.
        mirrored = anchor.right() + margin + width > right;
        x = mirrored ? anchor.x - margin - width : anchor.right() + margin;
    }
    // The whole block inside the safe area.
    const f32 blockLeft = mirrored ? x - static_cast<f32>(columns) * width : x;
    const f32 shift = std::clamp(blockLeft, left, std::max(left, right - blockWidth)) - blockLeft;
    x += shift;
    const auto clampY = [&](f32 y, f32 h) {
        return std::clamp(y, safe.y + margin, std::max(safe.y + margin, safe.bottom() - margin - h));
    };
    const f32 y = clampY(anchor.h > 0.0f ? anchor.y : safe.centerY() - height * 0.5f, height);
    p.main = {x, y, width, height};
    for (u32 i = 0; i < count; ++i) {
        const u32 column = row ? i : 0u;
        const f32 cx =
            mirrored ? x - static_cast<f32>(column + 1) * width : x + static_cast<f32>(column + 1) * width;
        const f32 cy = row || i == 0 ? y + drop : p.compare[i - 1].bottom();
        p.compare[i] = {cx, clampY(cy, compareHeights[i]), width, compareHeights[i]};
    }
    return p;
}

} // namespace vd
