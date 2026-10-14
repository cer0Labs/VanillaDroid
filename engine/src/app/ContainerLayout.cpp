#include "app/ContainerLayout.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

// ContainerFrame.lua's background arithmetic.
constexpr u32 kRowsInBgTexture = 6;      // ROWS_IN_BG_TEXTURE
constexpr u32 kMaxBgTextures = 2;        // MAX_BG_TEXTURES
constexpr f32 kBgTextureHeight = 512.0f; // BG_TEXTURE_HEIGHT
constexpr f32 kRowHeight = 41.0f;
constexpr f32 kFirstRowPixelOffset = 9.0f;
constexpr f32 kFirstRowTexCoordOffset = 0.353515625f;
constexpr f32 kItemSize = 37.0f;
constexpr f32 kItemStepX = 42.0f; // 37 + 5 (BOTTOMLEFT -5)
constexpr f32 kItemStepY = 41.0f; // 37 + 4 (TOPRIGHT 0, 4)
constexpr f32 kItemRight = 12.0f; // the first button's BOTTOMRIGHT -12

} // namespace

u32 keyringSize(u32 level) {
    return level > 60 ? 16u : level >= 50 ? 12u : level >= 40 ? 8u : 4u;
}

// --- The open frames
// -----------------------------------------------------------------------------------------

bool ContainerStack::isOpen(i32 id) const {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_order[i] == id) {
            return true;
        }
    }
    return false;
}

void ContainerStack::open(i32 id) {
    if (isOpen(id)) {
        return;
    }
    if (m_count == m_order.size()) {
        --m_count; // ContainerFrame_GetOpenFrame: every frame is shown, the last one is hidden and reused
    }
    m_order[m_count++] = id;
}

void ContainerStack::closeBag(i32 id) {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_order[i] == id) {
            // ContainerFrame_OnHide: the closed bag leaves the list and the rest close up.
            for (u32 j = i + 1; j < m_count; ++j) {
                m_order[j - 1] = m_order[j];
            }
            --m_count;
            return;
        }
    }
}

void ContainerStack::toggleBag(i32 id, const ContainerSizes& sizes) {
    if (sizes.size(id) == 0 && id != kKeyringContainer) {
        return;
    }
    if (isOpen(id)) {
        closeBag(id);
    } else {
        open(id);
    }
}

void ContainerStack::toggleBackpack(const ContainerSizes& sizes) {
    if (isOpen(0)) {
        m_count = 0; // every shown frame hides
    } else {
        toggleBag(0, sizes);
    }
}

void ContainerStack::openAllBags(const ContainerSizes& sizes, bool force) {
    u32 bagsOpen = 0;
    u32 totalBags = 1;
    for (u32 i = 1; i < 5; ++i) {
        totalBags += sizes.bags[i] > 0 ? 1u : 0u;
    }
    for (u32 i = 0; i < m_count; ++i) {
        bagsOpen += m_order[i] != kKeyringContainer ? 1u : 0u;
    }
    m_count = 0; // every shown frame hides, the keyring too
    if (bagsOpen >= totalBags && !force) {
        return;
    }
    toggleBackpack(sizes);
    for (i32 id = 1; id < 5; ++id) {
        toggleBag(id, sizes);
    }
}

void ContainerStack::openBag(i32 id, const ContainerSizes& sizes) {
    if (sizes.size(id) > 0) {
        open(id);
    }
}

void ContainerStack::closeAllBags() {
    // CloseBackpack (only when OpenBackpack did not find it open: here it always closes) and CloseBag(1..12).
    for (u32 i = 0; i < m_count;) {
        if (m_order[i] != kKeyringContainer) {
            closeBag(m_order[i]);
        } else {
            ++i;
        }
    }
}

void ContainerStack::raise(i32 id) {
    if (isOpen(id)) {
        closeBag(id);
        m_order[m_count++] = id;
    }
}

// --- One frame
// -------------------------------------------------------------------------------------------------

ContainerGeometry containerGeometry(i32 id, u32 size) {
    ContainerGeometry g;
    g.size = size;
    if (id == 0) {
        // The backpack: UI-BackpackBackground's 256 x 256 over a 240-unit frame, the money frame in its lower
        // part.
        g.art = ContainerArt::Backpack;
        g.height = 240.0f;
        g.pieces[0] = {0.0f, 256.0f, 0.0f, 1.0f};
        g.pieceCount = 1;
        g.firstItemBottom = 30.0f;
        return g;
    }
    g.art = id == kKeyringContainer ? ContainerArt::Keyring : id > 4 ? ContainerArt::Bank : ContainerArt::Bag;
    const u32 rows = std::max(1u, (size + kContainerColumns - 1) / kContainerColumns);
    u32 remaining = rows - 1; // the top piece holds a row already
    ContainerPiece top;
    if (size % kContainerColumns == 2) { // the top with two slots on the right
        top = {0.0f, 72.0f, 0.189453125f, 0.330078125f};
    } else if (rows == 1) { // a single row: the bottom of the top piece cut off
        top = {0.0f, 86.0f, 0.00390625f, 0.16796875f};
    } else {
        top = {0.0f, 94.0f, 0.00390625f, 0.18359375f};
    }
    g.pieces[g.pieceCount++] = top;
    f32 y = top.h;
    const u32 middles = (remaining + kRowsInBgTexture - 1) / kRowsInBgTexture;
    for (u32 i = 0; i < middles && i < kMaxBgTextures; ++i) {
        f32 h;
        if (remaining > kRowsInBgTexture) {
            // As the original: the texture coordinate offset ends up added to the height (a fraction of a
            // unit).
            h = static_cast<f32>(kRowsInBgTexture) * kRowHeight + kFirstRowTexCoordOffset;
            remaining -= kRowsInBgTexture;
        } else {
            h = static_cast<f32>(remaining) * kRowHeight - kFirstRowPixelOffset;
            remaining = 0;
        }
        g.pieces[g.pieceCount++] = {y, h, kFirstRowTexCoordOffset,
                                    h / kBgTextureHeight + kFirstRowTexCoordOffset};
        y += h;
    }
    g.pieces[g.pieceCount++] = {y, 10.0f, 0.330078125f, 0.349609375f};
    g.height = y + 10.0f;
    return g;
}

ui::Rect containerItemRect(const ContainerGeometry& g, u32 slot) {
    // Button j (1-based) shows slot size - j + 1; j = 1 sits at the bottom right, the next ones go left, a
    // new row starts above every fourth.
    const u32 k = g.size > slot ? g.size - 1 - slot : 0;
    const f32 right = kContainerWidth - kItemRight - static_cast<f32>(k % kContainerColumns) * kItemStepX;
    const f32 bottom = g.height - g.firstItemBottom - static_cast<f32>(k / kContainerColumns) * kItemStepY;
    return {right - kItemSize, bottom - kItemSize, kItemSize, kItemSize};
}

// --- Stacking
// --------------------------------------------------------------------------------------------------

u32 layoutContainers(std::span<const f32> heights, const ui::Rect& area, f32 scale, std::span<ui::Rect> out) {
    const f32 width = kContainerWidth * scale;
    f32 free = area.h;
    u32 column = 0;
    u32 fitting = 0;
    bool fits = true;
    for (u32 i = 0; i < heights.size() && i < out.size(); ++i) {
        const f32 h = heights[i] * scale;
        ui::Rect r{area.right() - width, area.bottom() - h, width, h};
        if (i > 0 && free < h) { // a new column, at the bottom again
            ++column;
            free = area.h;
        } else if (i > 0) { // on top of the previous one
            r.y = out[i - 1].y - kContainerSpacing * scale - h;
        }
        r.x = area.right() - static_cast<f32>(column + 1) * width;
        out[i] = r;
        fits = fits && r.x >= area.x - 0.5f && r.y >= area.y - 0.5f;
        fitting += fits ? 1u : 0u;
        free -= h + kVisibleContainerSpacing * scale;
    }
    return fitting;
}

f32 containerScale(std::span<const f32> heights, const ui::Rect& area, f32 largest, f32 smallest) {
    std::array<ui::Rect, kContainerFrames> rects{};
    const u32 count = static_cast<u32>(std::min<usize>(heights.size(), rects.size()));
    f32 scale = largest;
    while (scale > smallest && layoutContainers(heights.first(count), area, scale, rects) < count) {
        scale = std::max(smallest, scale * kContainerScale);
    }
    return std::max(scale, smallest);
}

u32 shownContainers(std::span<const f32> heights, const ui::Rect& area, f32 smallest, std::span<u32> shown) {
    std::array<ui::Rect, kContainerFrames> rects{};
    std::array<f32, kContainerFrames> kept{};
    const u32 count = static_cast<u32>(std::min({heights.size(), rects.size(), shown.size()}));
    if (count == 0) {
        return 0;
    }
    // The oldest `n` and the newest, with n from all of them down to none.
    u32 n = count - 1;
    for (;; --n) {
        for (u32 i = 0; i < n; ++i) {
            kept[i] = heights[i];
        }
        kept[n] = heights[count - 1];
        if (n == 0 || layoutContainers({kept.data(), n + 1}, area, smallest, rects) == n + 1) {
            break;
        }
    }
    for (u32 i = 0; i < n; ++i) {
        shown[i] = i;
    }
    shown[n] = count - 1;
    return n + 1;
}

} // namespace vd
