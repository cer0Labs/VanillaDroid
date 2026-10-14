#pragma once

#include "core/Hash.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <string_view>

namespace vd {

// What the client's engine writes over the units in the world (parity; WoW.exe, not FrameXML): their names
// with the NPC's title or the player's guild under them, the name plates (NamePlateFrame, the V / Shift-V
// keys) and the chat bubbles (ChatBubbleFrame). Pure views: a caller projects the units and fills them
// (app/HudWorld.cpp, the gallery's "worldlabels" page). Sizes are dp (`dp`: logical pixels per dp) or units
// of the client's 768-high interface (`unit`: logical pixels per unit), like the HUD's.

constexpr u64 kNameplateBorder = assetPathHash("Interface\\Tooltips\\Nameplate-Border.blp");
constexpr u64 kChatBubbleBackdrop = assetPathHash("Interface\\Tooltips\\ChatBubble-Backdrop.blp");
constexpr u64 kChatBubbleBackground = assetPathHash("Interface\\Tooltips\\ChatBubble-Background.blp");
constexpr u64 kChatBubbleTail = assetPathHash("Interface\\Tooltips\\ChatBubble-Tail.blp");

// One unit's label, standing on the point over its head (x, y: logical pixels).
struct UnitLabelView {
    f32 x = 0.0f;
    f32 y = 0.0f;
    std::string_view name;
    std::string_view title; // under the name in brackets: an NPC's subname ("Innkeeper"), a player's guild
    u32 color = ui::kWhite; // the name's (game::unitNameColor)
    // The name plate instead of the name: the health bar in `barColor`, the level in its box (0: the skull) in
    // `levelColor`, the name in white above. WoW.exe lights the plate under the mouse (0x7CB850: Nameplate-Glow and
    // the name in yellow; 0x7CB8B0 undoes it); without a mouse, the target's.
    bool plate = false;
    f32 health = 1.0f; // 0..1
    u32 barColor = ui::kWhite;
    u32 level = 0;
    u32 levelColor = ui::kGold;
    bool targeted = false;
};
// Draws the label; returns its top (logical pixels): what goes above it (a quest mark, a bubble) starts
// there.
f32 drawUnitLabel(ui::DrawList& draw, f32 dp, const UnitLabelView& v);
// How far above v.y drawUnitLabel() reaches (logical pixels).
f32 unitLabelHeight(const ui::DrawList& draw, f32 dp, const UnitLabelView& v);

// A chat bubble whose tail points at (tipX, tipY): the text wrapped at the bubble's width, in `color`, the
// whole bubble faded by `alpha` (0..1). The bubble slides sideways to stay inside `bounds` (the screen's safe
// area) as far as its tail stays under it.
struct ChatBubbleLayout {
    ui::Rect bubble; // the backdrop
    ui::Rect text;   // the wrapped text's box
    ui::Rect tail;
};
ChatBubbleLayout chatBubbleLayout(const ui::DrawList& draw, f32 unit, f32 tipX, f32 tipY,
                                  std::string_view text, const ui::Rect& bounds);
void drawChatBubble(ui::DrawList& draw, f32 unit, const ChatBubbleLayout& layout, std::string_view text,
                    u32 color, f32 alpha);
// The bubble's text size (units): ChatBubbleFont, the client's normal font.
constexpr f32 kChatBubbleTextUnits = 15.0f;

} // namespace vd
