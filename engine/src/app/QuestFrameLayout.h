#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

namespace vd {

// Quest windows: QuestFrame and GossipFrame with their art, QuestFramePanelTemplate's (and GossipFramePanelTemplate's)
// UI-QuestGreeting-TopLeft / -TopRight / -BotLeft / -BotRight, one 384 x 512 picture, at its proportions, and where
// QuestFrame.xml and GossipFrame.xml put their parts on it. Pure geometry (unit tested): app/Dialogs draws it.
// Rectangles in logical pixels.

// What the art shows of its 384 x 512 units: the portrait's ring from 2 across, the frame's border to 353 and 446.
constexpr f32 kQuestFrameLeft = 2.0f;
constexpr f32 kQuestFrameWidth = 351.0f;
constexpr f32 kQuestFrameHeight = 446.0f;

struct QuestFrameLayout {
    f32 k = 1.0f;          // logical pixels per unit of the art
    ui::Rect art;          // the 384 x 512 picture: TopLeft 256 x 256 at its top left, TopRight 128 x 256 right of it,
                           // BotLeft and BotRight under them
    ui::Rect frame;        // what of it shows
    ui::Rect portrait;     // QuestFramePortrait / GossipFramePortrait: 60 x 60 at (7, -6), under the ring
    ui::Rect title;        // QuestFrameNpcNameText (GameFontHighlight, 235 x 20) in QuestNpcNameFrame (TOP 0, -23)
    ui::Rect close;        // QuestFrameCloseButton: 32 x 32 centered at TOPRIGHT (-42, -31)
    ui::Rect scroll;       // the panels' scroll frames: 300 x 334 at (23, -81)
    ui::Rect scrollBar;    // UIPanelScrollFrameTemplate's bar, 16 wide 6 right of it, its buttons within
    ui::Rect accept;       // QuestFrameAcceptButton: 77 x 22 at BOTTOMLEFT (23, 72)
    ui::Rect leftButton;   // QuestFrameCompleteButton / QuestFrameCompleteQuestButton: 120 x 22 at BOTTOMLEFT (22, 72)
    ui::Rect rightButton;  // Decline / Cancel / Goodbye: 78 x 22 at BOTTOMRIGHT (-39, 73)
    ui::Rect patch;        // UI-Quest-BotLeftPatch over the left slot when it has no button: 128 x 64 at (22, 68)
};

// The frame at `k` logical pixels per unit, or less when `place` is smaller; at the place's top left, or centered in
// it.
QuestFrameLayout questFrameLayout(const ui::Rect& place, f32 k, bool center);

} // namespace vd
