#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/MessageFrame.h"

#include <span>
#include <string_view>

namespace vd {

namespace game {
class GameData;
class MirrorTimers;
} // namespace game

// What the 1.12 interface shows at the top centre of the screen besides the zone text: the UIErrorsFrame and
// the three mirror timer bars. Pure drawing: no game state (GameScreens fills them).

// The client's UIErrorsFrame (UIErrorsFrame.xml): `frame`'s lines centred 122 units under the top of `area`,
// newest on top, ErrorFont (larger on the phone: 16 dp), fading as the frame says. Call frame.update(nowMs)
// first. `below`: the bottom of the mirror timer bars shown (0 none): the lines start under them (in 1.12
// they overlapped bars 2 and 3). `avoid`: the chat where it reaches the top of a short screen; the lines move
// right of it rather than over it.
void errorsFrame(DialogContext& c, const ui::Rect& area, const ui::MessageFrame& frame, u64 nowMs,
                 f32 below = 0.0f, const ui::Rect& avoid = {});

// Phase 16 (alerts): RaidWarningFrame and RaidBossEmoteFrame (RaidWarning.xml): two MessageFrames 512 x 40,
// insertMode TOP, GameFontNormalHuge (20 units; 20 dp here, the UIErrorsFrame's enlargement), the first 10
// units under the UIErrorsFrame (its 60 units from where errorsFrame() starts, with the same `below`), the
// second 10 units under it. A line wraps in the frame's width; the newest is whole, older ones while the
// frame's two lines last. Kept clear of `avoid` as errorsFrame(). Call each frame's update(nowMs) first.
void raidWarningFrames(DialogContext& c, const ui::Rect& area, const ui::MessageFrame& warnings,
                       const ui::MessageFrame& emotes, u64 nowMs, f32 below = 0.0f,
                       const ui::Rect& avoid = {});
// Where RaidBossEmoteFrame ends (raidWarningFrames' layout, with the same `below`), shown or not.
f32 raidWarningFramesBottom(const DialogContext& c, const ui::Rect& area, f32 below = 0.0f);

// One mirror timer bar as the HUD shows it (breath, fatigue, feign death).
struct MirrorTimerBarView {
    bool shown = false;
    std::string_view label;
    f32 fill = 0.0f; // 0..1 of the bar
    u32 color = 0;   // MirrorTimerColors
};

// The client's MirrorTimer1..3 (MirrorTimer.xml): 96 units under the top of the screen, centred, each 206 x
// 26 under the one before (a hidden frame keeps its place): a 195 x 13 bar (UI-StatusBar) on a half-black
// background, the casting bar's border around it and the label (GameFontHighlight) over it. Returns the
// bottom of the last bar shown (logical pixels), 0 when none is. `avoid`: as errorsFrame's.
f32 mirrorTimerBars(DialogContext& c, std::span<const MirrorTimerBarView> bars, const ui::Rect& avoid = {});

// The views of the timers at `nowMs`, one per frame (MirrorTimers::kFrames): the label is the type's text
// (BREATH_LABEL, EXHAUSTION_LABEL), or the timer's spell (Spell.dbc, `data` may be null) when the client has
// none.
void mirrorTimerViews(const game::MirrorTimers& timers, const game::GameData* data, u32 nowMs,
                      std::span<MirrorTimerBarView> out);
// MirrorTimer.lua's MirrorTimerColors of a timer type (EXHAUSTION, BREATH, FEIGNDEATH).
u32 mirrorTimerColor(u32 type);

} // namespace vd
