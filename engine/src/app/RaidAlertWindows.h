#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "render/Renderer.h"

#include <string_view>

namespace vd {

// Phase 16 (alerts): ReadyCheckFrame (Blizzard_RaidUI.xml) as a pure view: what a raid member sees when its
// leader starts a ready check. The frame's art (Interface\RaidFrame\UI-ReadyCheckFrame, texels 0..323 x 0..97
// of 512 x 128) over the leader's portrait (ReadyCheckPortrait: 50 x 50 at 7, -6, in the art's round hole),
// the message (GameFontNormal, TOP + 15, -24) and Yes / No (UIPanelButtonTemplate 109 x 24, Yes at BOTTOMLEFT
// + 55, +18, No at its right). 323 x 97 units centred 10 units below the screen's centre (its CENTER anchor),
// as the StaticPopups at 1.25 dp a unit (the 12-unit text at the dialogs' 15 dp). Not modal: only its buttons
// take fingers.
struct ReadyCheckView {
    std::string_view text;    // READY_CHECK_MESSAGE with the leader's name ("\n" and colour codes in it)
    TextureHandle portrait;   // the leader's (app/Portraits); invalid: the stand-in below
    u64 portraitFallback = 0; // TemporaryPortrait-<gender>-<race> of the leader
};
enum class ReadyCheckChoice : u8 { None, Yes, No };
// `avoid`: the touch HUD's action buttons (the frame moves left of them where they reach it); `minTop`: where
// the raid warning frames end (app/MessageHud raidWarningFramesBottom): on a short screen the frame goes
// under them instead of behind them (they are HIGH strata).
ReadyCheckChoice readyCheckFrame(DialogContext& c, const ReadyCheckView& view, const ui::Rect& avoid = {},
                                 f32 minTop = 0.0f);
// The frame's rectangle as readyCheckFrame() draws it (the gallery, tests of the layout).
ui::Rect readyCheckFrameRect(const DialogContext& c, const ui::Rect& avoid = {}, f32 minTop = 0.0f);

} // namespace vd
