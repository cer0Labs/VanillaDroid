#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>

namespace vd {

// Character frame: CharacterFrame with its 1.12 art and CharacterFrameTab1..5 turned to stand along its right edge,
// in two arrangements:
//  * tall (the inner screen's places): PaperDollFrame as the original, its 384 x 512 picture
//    (UI-Character-CharacterTab-L1 / -R1 / -BottomLeft / -BottomRight) with the slots, the model, the resistances and
//    CharacterAttributesFrame where PaperDollFrame.xml puts them;
//  * wide (the phones, the cover screen; not in 1.12): the plain frame (UI-Character-General-TopLeft / -TopRight /
//    -BottomLeft / -BottomRight) stretched across in nine pieces, the left column's slots two across (head, neck,
//    shoulders, back over the main hand; chest, shirt, tabard, wrists over the off hand) and the right column's too
//    (hands, waist, legs, feet over the ranged slot; the rings and trinkets over the ammo slot), each in the riveted
//    cell of the original's weapon slots; between them the attributes, melee and ranged boxes one under the other,
//    the model and the five resistances (the model in the middle).
// The other tabs (ReputationFrame, SkillFrame, HonorFrame: as the original's even where the paper doll is wide)
// always take the tall frame: at() places their parts in its units. Pure geometry (unit tested):
// app/CharacterWindows draws it. Rectangles in logical pixels.

// The tall frame: what its art shows of the 384 x 512 picture (the ring from 2, the border to 350 and 440).
constexpr f32 kCharacterTallWidth = 350.0f - 2.0f;
constexpr f32 kCharacterTallHeight = 440.0f;
// The wide frame: its border reaches 470 across and 300 down.
constexpr f32 kCharacterWideRight = 474.0f;
constexpr f32 kCharacterWideHeight = 300.0f;
// The turned tabs: an unselected tab's top 5 units in from the border, as wide as the tab is high.
constexpr f32 kCharacterTabsWidth = 32.0f;

// A part of one of the four textures (0 top left, 1 top right, 2 bottom left, 3 bottom right) drawn over `dst`.
struct CharacterArtPiece {
    u8 texture = 0;
    ui::Rect dst;
    ui::TexCoords uv;
};

// The riveted cell of the original's first weapon slot, which the wide arrangement puts under every slot: 52 x 52 units
// of the picture from (113, 376), in UI-Character-CharacterTab-BottomLeft.
constexpr ui::TexCoords kCharacterCellUv{113.0f / 256.0f, 120.0f / 256.0f, 165.0f / 256.0f, 172.0f / 256.0f};

// The equipment view's cells (PaperDollView::equipment): the inventory slots 0..18, then the ammo slot.
constexpr u32 kCharacterCells = 20;
constexpr u32 kCharacterAmmoCell = 19;
// The paper doll's numbers (game::PaperDollStat): the six attributes, then melee and ranged (three lines each).
constexpr u32 kCharacterStatLines = 12;

struct CharacterFrameLayout {
    bool wide = false;
    f32 k = 1.0f;   // logical pixels per unit of the art
    ui::Rect art;   // the 384 x 512 picture (wide: stretched to its frame)
    ui::Rect frame; // what the art shows
    // The frame's art: the tall paper doll's four textures as they are, or the plain frame's nine stretched pieces
    // (each split where it crosses from one texture to another).
    std::array<CharacterArtPiece, 36> pieces{};
    u32 pieceCount = 0;
    std::array<CharacterArtPiece, 4> plainPieces{}; // tall: the plain frame (UI-Character-General-*) for the other tabs
    u32 plainPieceCount = 0;
    ui::Rect portrait; // CharacterFramePortrait: 60 x 60 at (7, -6), under the ring
    ui::Rect name;     // CharacterNameText (300 x 12 in CharacterNameFrame, its middle at (198, -24))
    ui::Rect level;    // CharacterLevelText (GameFontNormalSmall), 6 under it
    ui::Rect guild;    // CharacterGuildText, 1 under that
    ui::Rect close;    // CharacterFrameCloseButton (32 x 32, its middle at TOPRIGHT (-44, -25))
    ui::Rect tabs;     // the turned tabs' column
    ui::Rect body;     // the other tabs' place, inside the border under the header
    // The paper doll.
    std::array<ui::Rect, kCharacterCells> slots{}; // PaperDollItemSlotButtonTemplate (37 x 37; the ammo slot 27 x 27)
    std::array<ui::Rect, kCharacterCells> cells{}; // wide: the riveted cell art under each slot (empty: none)
    ui::Rect ammoFrame; // UI-Character-AmmoSlot's frame (41 x 41 around the ammo slot)...
    ui::Rect ammoArrow; // ...and its arrow from the ranged slot (23 x 41, 22 left of the slot's middle)
    ui::Rect model;     // CharacterModelFrame
    ui::Rect inspectModel; // InspectModelFrame: taller in the tall frame (no numbers under it)
    std::array<ui::Rect, 5> resistances{}; // MagicResFrame1..5 (32 x 29)
    std::array<ui::Rect, 3> statBoxes{};   // the attributes', the melee and the ranged UI-Character-StatBackground
    std::array<ui::Rect, kCharacterStatLines> statLines{};

    // A rectangle in the picture's units (from its top left, y down: CharacterFrame's FrameXML anchors).
    ui::Rect at(f32 ux, f32 uy, f32 uw, f32 uh) const { return {art.x + ux * k, art.y + uy * k, uw * k, uh * k}; }
};

// The scale for a frame whose buttons are `buttonHeight` high (the quest frames', the friends frame's).
constexpr f32 characterFrameScale(f32 buttonHeight) {
    return buttonHeight / 21.0f;
}
// Whether `place` gets the wide arrangement: a place clearly wider than tall, or one where it shows bigger.
bool characterFrameWide(const ui::Rect& place, f32 k);
// The frame in `place` at `k` logical pixels per unit (or less when the place is smaller), at its top left or
// centered.
CharacterFrameLayout characterFrameLayout(const ui::Rect& place, f32 k, bool wide, bool center);
// Where a frame 1.12 hangs at CharacterFrame's right goes (ReputationDetailFrame: TOPRIGHT (-33, -28)): `top` units
// under the frame's top, right of the turned tabs, `w` x `h` logical pixels (at most down to the frame's bottom);
// where that does not fit `bounds` (the safe area), as far right as it fits, over the frame.
ui::Rect characterSideFrame(const CharacterFrameLayout& l, const ui::Rect& bounds, f32 top, f32 w, f32 h);

} // namespace vd
