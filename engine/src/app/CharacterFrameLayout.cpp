#include "app/CharacterFrameLayout.h"

#include <algorithm>

namespace vd {
namespace {

// What each arrangement takes, from the ring's left to the tabs' outer side, and down to the border's bottom.
constexpr f32 kTallTabsLeft = 345.0f;
constexpr f32 kWideTabsLeft = kCharacterWideRight - 5.0f;
constexpr f32 kTallWidth = kTallTabsLeft + kCharacterTabsWidth - 2.0f;
constexpr f32 kWideWidth = kWideTabsLeft + kCharacterTabsWidth - 2.0f;

// The plain frame's nine pieces: the picture's columns 0..150 (the ring and the title bar's start), 150..290 (the
// title bar, stretched) and 290..384 (its end and the close button's notch); its rows 0..90 (the title and the band
// under it), 90..400 (stretched or squeezed) and 400..512 (the bottom border).
constexpr f32 kSrcX[4] = {0.0f, 150.0f, 290.0f, 384.0f};
constexpr f32 kSrcY[4] = {0.0f, 90.0f, 400.0f, 512.0f};
constexpr f32 kWideExtraX = kCharacterWideRight - 350.0f;
constexpr f32 kWideExtraY = kCharacterTallHeight - kCharacterWideHeight;
constexpr f32 kDstX[4] = {0.0f, 150.0f, 290.0f + kWideExtraX, 384.0f + kWideExtraX};
constexpr f32 kDstY[4] = {0.0f, 90.0f, 400.0f - kWideExtraY, 512.0f - kWideExtraY};

// The paper doll's slots in the equipment view's order (inventory slots): the left column (head, neck, shoulders,
// back, chest, shirt, tabard, wrists), the right one (hands, waist, legs, feet, rings, trinkets), the weapons.
constexpr u8 kLeftSlots[8] = {0, 1, 2, 14, 4, 3, 18, 8};
constexpr u8 kRightSlots[8] = {9, 5, 6, 7, 10, 11, 12, 13};
constexpr u8 kWeaponSlots[3] = {15, 16, 17};

// The riveted cell (kCharacterCellUv): 52 x 52 units, its slot 9 in from its top left.
constexpr f32 kCellSize = 52.0f, kCellInset = 9.0f;

} // namespace

bool characterFrameWide(const ui::Rect& place, f32 k) {
    // A place wider than tall (the phones' and the cover screen's), or one where the tall frame would have to shrink
    // more than the wide one.
    const f32 tall = std::min({k, place.w / kTallWidth, place.h / kCharacterTallHeight});
    const f32 wide = std::min({k, place.w / kWideWidth, place.h / kCharacterWideHeight});
    return place.w > place.h * 1.3f || wide > tall * 1.05f;
}

CharacterFrameLayout characterFrameLayout(const ui::Rect& place, f32 k, bool wide, bool center) {
    CharacterFrameLayout l;
    l.wide = wide;
    if (place.w <= 0.0f || place.h <= 0.0f || k <= 0.0f) {
        return l;
    }
    const f32 width = wide ? kWideWidth : kTallWidth;
    const f32 height = wide ? kCharacterWideHeight : kCharacterTallHeight;
    k = std::min({k, place.w / width, place.h / height});
    l.k = k;
    const f32 x = center ? place.centerX() - width * k * 0.5f : place.x;
    const f32 y = center ? place.centerY() - height * k * 0.5f : place.y;
    const f32 ax = x - 2.0f * k, ay = y; // the picture's top left
    l.art = {ax, ay, (wide ? kDstX[3] : 384.0f) * k, (wide ? kDstY[3] : 512.0f) * k};
    const auto at = [&](f32 ux, f32 uy, f32 uw, f32 uh) { return ui::Rect{ax + ux * k, ay + uy * k, uw * k, uh * k}; };
    // A region of the 384 x 512 picture drawn over a region of the frame's: split where it crosses from one texture
    // to another (they meet at 256 across and down).
    const auto piece = [&](std::array<CharacterArtPiece, 36>& out, u32& count, f32 sx0, f32 sy0, f32 sx1, f32 sy1,
                           f32 dx0, f32 dy0, f32 dx1, f32 dy1) {
        const f32 xs[3] = {sx0, std::clamp(256.0f, sx0, sx1), sx1};
        const f32 ys[3] = {sy0, std::clamp(256.0f, sy0, sy1), sy1};
        for (u32 j = 0; j < 2; ++j) {
            for (u32 i = 0; i < 2; ++i) {
                const f32 u0 = xs[i], u1 = xs[i + 1], v0 = ys[j], v1 = ys[j + 1];
                if (u1 - u0 <= 0.0f || v1 - v0 <= 0.0f || count >= out.size()) {
                    continue;
                }
                const bool right = u0 >= 256.0f, bottom = v0 >= 256.0f;
                const f32 tw = right ? 128.0f : 256.0f;
                const f32 ox = right ? 256.0f : 0.0f, oy = bottom ? 256.0f : 0.0f;
                const auto mapX = [&](f32 s) { return dx0 + (s - sx0) / (sx1 - sx0) * (dx1 - dx0); };
                const auto mapY = [&](f32 s) { return dy0 + (s - sy0) / (sy1 - sy0) * (dy1 - dy0); };
                CharacterArtPiece& p = out[count++];
                p.texture = static_cast<u8>((bottom ? 2 : 0) + (right ? 1 : 0));
                p.dst = at(mapX(u0), mapY(v0), mapX(u1) - mapX(u0), mapY(v1) - mapY(v0));
                p.uv = {(u0 - ox) / tw, (v0 - oy) / 256.0f, (u1 - ox) / tw, (v1 - oy) / 256.0f};
            }
        }
    };

    l.portrait = at(7.0f, 6.0f, 60.0f, 60.0f);
    if (!wide) {
        // PaperDollFrame as the original: its four textures as they are.
        l.frame = at(2.0f, 0.0f, kCharacterTallWidth, kCharacterTallHeight);
        piece(l.pieces, l.pieceCount, 0.0f, 0.0f, 384.0f, 512.0f, 0.0f, 0.0f, 384.0f, 512.0f);
        std::array<CharacterArtPiece, 36> plain{};
        u32 plainCount = 0;
        piece(plain, plainCount, 0.0f, 0.0f, 384.0f, 512.0f, 0.0f, 0.0f, 384.0f, 512.0f);
        for (u32 i = 0; i < plainCount && i < l.plainPieces.size(); ++i) {
            l.plainPieces[l.plainPieceCount++] = plain[i];
        }
        l.name = at(198.0f - 150.0f, 18.0f, 300.0f, 12.0f);
        l.level = at(198.0f - 150.0f, 36.0f, 300.0f, 11.0f);
        l.guild = at(198.0f - 150.0f, 47.0f, 300.0f, 11.0f);
        l.close = at(340.0f - 16.0f, 25.0f - 16.0f, 32.0f, 32.0f);
        l.tabs = at(kTallTabsLeft, 75.0f, kCharacterTabsWidth, 437.0f - 75.0f);
        l.body = at(20.0f, 76.0f, 322.0f, 354.0f);
        for (u32 i = 0; i < 8; ++i) {
            l.slots[kLeftSlots[i]] = at(21.0f, 74.0f + 41.0f * static_cast<f32>(i), 37.0f, 37.0f);
            l.slots[kRightSlots[i]] = at(305.0f, 74.0f + 41.0f * static_cast<f32>(i), 37.0f, 37.0f);
        }
        for (u32 i = 0; i < 3; ++i) {
            l.slots[kWeaponSlots[i]] = at(122.0f + 42.0f * static_cast<f32>(i), 385.0f, 37.0f, 37.0f);
        }
        l.slots[kCharacterAmmoCell] = at(258.0f, 390.0f, 27.0f, 27.0f); // 15 right of the ranged slot
        l.model = at(65.0f, 78.0f, 233.0f, 224.0f);
        l.inspectModel = at(65.0f, 78.0f, 233.0f, 300.0f);
        for (u32 i = 0; i < 5; ++i) {
            l.resistances[i] = at(265.0f, 77.0f + 29.0f * static_cast<f32>(i), 32.0f, 29.0f);
        }
        l.statBoxes = {at(67.0f, 291.0f, 115.0f, 85.0f), at(182.0f, 291.0f, 115.0f, 44.0f),
                       at(182.0f, 337.0f, 115.0f, 43.0f)};
    } else {
        // The plain frame stretched in its nine pieces, the slots in riveted cells two across a side.
        l.frame = at(2.0f, 0.0f, kCharacterWideRight - 2.0f, kCharacterWideHeight);
        for (u32 j = 0; j < 3; ++j) {
            for (u32 i = 0; i < 3; ++i) {
                piece(l.pieces, l.pieceCount, kSrcX[i], kSrcY[j], kSrcX[i + 1], kSrcY[j + 1], kDstX[i], kDstY[j],
                      kDstX[i + 1], kDstY[j + 1]);
            }
        }
        const f32 mid = 198.0f + kWideExtraX * 0.5f; // the title bar's middle, moved by its stretch
        l.name = at(mid - 150.0f, 18.0f, 300.0f, 12.0f);
        l.level = at(mid - 150.0f, 36.0f, 300.0f, 11.0f);
        l.guild = at(mid - 150.0f, 47.0f, 300.0f, 11.0f);
        l.close = at(340.0f + kWideExtraX - 16.0f, 25.0f - 16.0f, 32.0f, 32.0f);
        // From under the close button: the short frame's column needs the room.
        l.tabs = at(kWideTabsLeft, 46.0f, kCharacterTabsWidth, kCharacterWideHeight - 3.0f - 46.0f);
        l.body = at(20.0f, 76.0f, kCharacterWideRight - 28.0f, kCharacterWideHeight - 84.0f);
        // Two columns a side, five rows 41 apart: the column's eight slots, then its weapons; between the sides the
        // numbers, the model and the resistances.
        constexpr f32 kColumns[4] = {24.0f, 66.0f, 381.0f, 423.0f};
        constexpr u8 kGrid[4][5] = {{0, 1, 2, 14, 15}, {4, 3, 18, 8, 16}, {9, 5, 6, 7, 17}, {10, 11, 12, 13, 255}};
        for (u32 c = 0; c < 4; ++c) {
            for (u32 r = 0; r < 5; ++r) {
                const u8 slot = kGrid[c][r];
                if (slot == 255) {
                    continue;
                }
                const f32 sx = kColumns[c], sy = 82.0f + 41.0f * static_cast<f32>(r);
                l.slots[slot] = at(sx, sy, 37.0f, 37.0f);
                l.cells[slot] = at(sx - kCellInset, sy - kCellInset, kCellSize, kCellSize);
            }
        }
        l.slots[kCharacterAmmoCell] = at(kColumns[2] + 37.0f + 15.0f, 82.0f + 41.0f * 4.0f + 5.0f, 27.0f, 27.0f);
        l.model = at(233.0f, 78.0f, 100.0f, 212.0f);
        l.inspectModel = l.model;
        for (u32 i = 0; i < 5; ++i) {
            l.resistances[i] = at(337.0f, 84.0f + 29.0f * static_cast<f32>(i), 32.0f, 29.0f);
        }
        l.statBoxes = {at(114.0f, 84.0f, 115.0f, 85.0f), at(114.0f, 171.0f, 115.0f, 44.0f),
                       at(114.0f, 217.0f, 115.0f, 43.0f)};
    }
    // The ammo slot's art around it and its arrow from the ranged slot (CENTER (-22, 0)).
    const ui::Rect& ammo = l.slots[kCharacterAmmoCell];
    const f32 acx = ammo.centerX(), acy = ammo.centerY();
    l.ammoFrame = {acx - 20.5f * k, acy - 20.5f * k, 41.0f * k, 41.0f * k};
    l.ammoArrow = {acx - 22.0f * k - 11.5f * k, acy - 20.5f * k, 23.0f * k, 41.0f * k};
    // CharacterStatFrame1..5 and CharacterArmorFrame (6, -3 in the attributes' box, 13 apart); CharacterAttackFrame,
    // its power and damage (7, 2 in the melee box, then 5 in, 12 apart); the ranged ones (7, -1 in theirs).
    const ui::Rect& attributes = l.statBoxes[0];
    const ui::Rect& melee = l.statBoxes[1];
    const ui::Rect& ranged = l.statBoxes[2];
    for (u32 i = 0; i < 6; ++i) {
        l.statLines[i] = {attributes.x + 6.0f * k, attributes.y + (3.0f + 13.0f * static_cast<f32>(i)) * k, 104.0f * k,
                          13.0f * k};
    }
    const auto block = [&](u32 first, const ui::Rect& box, f32 top) {
        l.statLines[first] = {box.x + 7.0f * k, box.y + top * k, 104.0f * k, 13.0f * k};
        l.statLines[first + 1] = {box.x + 12.0f * k, box.y + (top + 12.0f) * k, 99.0f * k, 13.0f * k};
        l.statLines[first + 2] = {box.x + 12.0f * k, box.y + (top + 24.0f) * k, 99.0f * k, 13.0f * k};
    };
    block(6, melee, 2.0f);
    block(9, ranged, -1.0f);
    return l;
}

ui::Rect characterSideFrame(const CharacterFrameLayout& l, const ui::Rect& bounds, f32 top, f32 w, f32 h) {
    const f32 y = l.art.y + top * l.k;
    h = std::min(h, l.frame.bottom() - y);
    w = std::min(w, bounds.w);
    f32 x = l.tabs.right() + 2.0f * l.k;
    if (x + w > bounds.right()) {
        x = std::max(bounds.x, bounds.right() - w);
    }
    return {x, y, w, std::max(0.0f, h)};
}

} // namespace vd
