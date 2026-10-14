#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>

namespace vd {

// Quest log: QuestLogFrame with its 1.12 art, UI-QuestLog-TopLeft / -TopRight / -BotLeft / -BotRight (one 384 x 512
// picture, QuestLogFrame.xml), cut into pieces that stretch, in two arrangements: the original's, the list over the
// parchment (tall places: the inner screen's slots), and ours, the list at the left and the parchment with its scroll
// bar at the right, under the title bar and the header and over the buttons' bar that run across both (wide places: the
// phones, the cover screen). Pure geometry (unit tested): app/QuestWindows draws it. Rectangles in logical pixels.

// The art, in the order of the drawing's table of paths.
enum class QuestLogTexture : u8 {
    TopLeft, // UI-QuestLog-TopLeft, 256 x 256
    TopRight, // 128 x 256
    BotLeft,
    BotRight,
    EmptyTopLeft, // EmptyQuestLogFrame (no quest): UI-QuestLog-Empty-TopLeft, 256 x 256, the cobwebs and the book
    EmptyTopRight, // 64 x 256
    EmptyBotLeft,  // 256 x 128
    EmptyBotRight, // 64 x 128
};
constexpr u32 kQuestLogTextures = 8;

// A part of one texture drawn over `dst`.
struct QuestLogPiece {
    QuestLogTexture texture = QuestLogTexture::TopLeft;
    ui::Rect dst;
    ui::TexCoords uv;
};

// The art's sizes in its units (QuestLogFrame is 384 x 512 with transparent margins at its right and bottom).
constexpr f32 kQuestLogMinWidth = 346.0f;  // the art's visible width: the original's arrangement unstretched
constexpr f32 kQuestLogMinHeight = 300.0f; // the title bar, the header, the buttons' bar and some content
constexpr f32 kQuestLogButtonHeight = 21.0f; // UIPanelButtonTemplate in the buttons' bar (its slots' inside)

struct QuestLogLayout {
    bool wide = false;
    f32 k = 1.0f;   // logical pixels per unit of the art
    ui::Rect frame; // what the art covers
    std::array<QuestLogPiece, 112> pieces{};
    u32 pieceCount = 0;
    std::array<QuestLogPiece, 8> emptyPieces{}; // EmptyQuestLogFrame: the cobwebs over the list, the book
    u32 emptyPieceCount = 0;
    ui::Rect portrait;     // UI-QuestLog-BookIcon, 64 x 64 (BACKGROUND: under the art's ring)
    f32 titleCenterX = 0.0f; // QuestLogTitleText (GameFontHighlight), its middle
    ui::Rect title;          // its line in the title bar
    ui::Rect close;          // QuestLogFrameCloseButton (UIPanelCloseButton, 32 x 32)
    ui::Rect expand;         // QuestLogCollapseAllButton (40 x 22; its tab reaches 6 left of it, 8 above)
    ui::Rect track;          // QuestLogTrack's radio (20 x 20; TRACK_QUEST right of it)
    f32 countRight = 0.0f;   // the right edge of the count's box (QuestLogCountRight)
    f32 countTop = 0.0f;     // its top (20 units high)
    ui::Rect list;           // QuestLogListScrollFrame: where the rows go
    ui::Rect listBar;        // its scroll bar (16 wide, the up and down buttons at its ends): shown when they overflow
    ui::Rect detail;         // QuestLogDetailScrollFrame
    ui::Rect detailBar;      // its scroll bar, in the art's column
    ui::Rect noQuests;       // QuestLogNoQuestsText (an empty log), over the list's cobwebs
    ui::Rect abandon;        // QuestLogFrameAbandonButton (125 x 21)
    ui::Rect share;          // QuestFramePushQuestButton (123 x 21)
    ui::Rect exit;           // QuestFrameExitButton (77 x 21)
};

// The art scale (logical pixels per unit) for a window whose buttons are `buttonHeight` high: they fill the slots of
// the buttons' bar.
constexpr f32 questLogScale(f32 buttonHeight) {
    return buttonHeight / kQuestLogButtonHeight;
}
// The most each arrangement grows to, in units.
constexpr f32 kQuestLogMaxWideWidth = 960.0f, kQuestLogMaxWideHeight = 560.0f;
constexpr f32 kQuestLogMaxTallWidth = 560.0f, kQuestLogMaxTallHeight = 760.0f;
// Whether a place (in logical pixels, at `k` per unit) gets the wide arrangement: wider than tall, with room for the
// list and the parchment side by side.
bool questLogWide(const ui::Rect& place, f32 k);
// The quest log filling `place` from its top left corner (up to the kQuestLogMax* sizes of its arrangement), at `k`
// logical pixels per unit, or less when the place is smaller than the art's fixed parts. `listShare`: the list's part
// of the room (wide: at most, the parchment keeping the original's 300 units when it can; tall: of the height under the
// header).
QuestLogLayout questLogLayout(const ui::Rect& place, f32 k, bool wide, f32 listShare);

} // namespace vd
