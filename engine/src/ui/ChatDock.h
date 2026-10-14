#pragma once

#include "core/Types.h"

#include <span>

namespace vd::ui {

// The timing and the tab rule of the client's chat frames (parity: ChatFrame.xml, FloatingChatFrame.lua and
// UIParent.lua's UIFrameFade / UIFrameFlash) as pure functions; app/ChatFrame draws with them.

// ScrollingMessageFrame as ChatFrameTemplate sets it (displayDuration="120.0"; fadeDuration, UI.xsd's 3.0): a
// line stays for `visibleMs` after it came in (or after the frame last showed every line again), then fades
// out over `fadeMs`.
constexpr u32 kChatVisibleMs = 120000;
constexpr u32 kChatFadeMs = 3000;
// The opacity of a line shown since `shownMs` at `nowMs`: 1, then down to 0 over the fade.
f32 chatLineAlpha(u64 shownMs, u64 nowMs, u32 visibleMs = kChatVisibleMs, u32 fadeMs = kChatFadeMs);

// UIFrameFlash(frame, fadeInTime, fadeOutTime, flashDuration, showWhenDone, flashInHoldTime,
// flashOutHoldTime), with the times FCF_FlashTab gives a docked tab that got a whisper while another one was
// shown: (0.25, 0.25, 60, nil, 0.5, 0.5). It fades in, holds, fades out, holds, again, until the duration is
// over.
struct FlashTiming {
    u32 fadeInMs = 250;
    u32 fadeOutMs = 250;
    u32 inHoldMs = 500;  // held shown after fading in
    u32 outHoldMs = 500; // held hidden after fading out
    u32 durationMs = 60000;
};
// The flashing texture's opacity `elapsedMs` after the flash started (0 once it is over).
f32 flashAlpha(u64 elapsedMs, const FlashTiming& timing = {});
bool flashOver(u64 elapsedMs, const FlashTiming& timing = {});

// ChatFrame_OnUpdate: while a frame is scrolled back from its newest line, its bottom button's flash
// (UI-ChatIcon-BlinkHilight) turns on and off every CHAT_BUTTON_FLASH_TIME (0.5 s).
constexpr u32 kChatButtonFlashMs = 500;
bool bottomButtonLit(u64 nowMs);

// UIFrameFadeIn / UIFrameFadeOut: `alpha` moved towards `target` after `dtMs`, a whole fade taking `fadeMs`
// (CHAT_FRAME_FADE_TIME, 0.15 s, for the chat's tabs and background).
constexpr f32 kChatFrameFadeMs = 150.0f;
f32 fadeTowards(f32 alpha, f32 target, f32 dtMs, f32 fadeMs = kChatFrameFadeMs);

// FCF_DockUpdate: the docked frames' tabs side by side, each as wide as its name needs
// (PanelTemplates_TabResize). When they do not fit in `available`, the ones narrower than the average keep
// their width and the others share what is left equally (their names are then cut). `widths` holds the wanted
// widths in and the drawn ones out.
void dockTabWidths(std::span<f32> widths, f32 available);

} // namespace vd::ui
