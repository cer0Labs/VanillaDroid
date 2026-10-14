#pragma once

#include "core/Types.h"

namespace vd {

// Phase 13 (combat): the full-screen status flashes of the 1.12 interface, as UIParent.lua's PlayerStatus_OnUpdate
// drives them every frame with UIFrameFlash: LowHealthFrame (Interface\FullScreenTextures\LowHealth, red edges) while
// the player frame's health bar is at 20 % or less, and OutOfControlFrame (OutOfControl, blue edges) while
// PLAYER_CONTROL_LOST holds (feared, charmed, possessed...). Both fade in 0.5 s and out 0.5 s for 100 s.
// Phase 20: they stop when the reason goes, as PlayerStatus_OnUpdate means to with its UIFrameFlash(frame, 1, 1, 0) (a
// flash of no duration: hidden at the next update). In the 1.12 that call does nothing (UIFrameFlash returns at once
// for a frame already flashing) and a flash ran its 100 s, the red edges long after a potion; and its low health stop
// above 10 % would end a flash the frame after one starting at 20 %, so both thresholds are 20 % here. A frame already
// flashing still ignores a new start. No allocation; main thread.
class StatusFlash {
public:
    // `healthFraction`: the player frame's health bar (value / max); `dead`: UnitIsDead("player"); `enabled`:
    // SHOW_FULLSCREEN_STATUS ("1" by default).
    void update(f32 dt, f32 healthFraction, bool dead, bool outOfControl, bool enabled = true);
    f32 lowHealthAlpha() const { return m_lowHealth.shown ? m_lowHealth.alpha : 0.0f; }
    f32 outOfControlAlpha() const { return m_outOfControl.shown ? m_outOfControl.alpha : 0.0f; }
    void reset();

    // The frames' textures (alphaMode="ADD"), as path hashes of ui.vdpk.
    static u64 lowHealthTexture();
    static u64 outOfControlTexture();

private:
    struct Flash {
        bool flashing = false; // the Lua frame.flashing: PlayerStatus_OnUpdate's own state
        bool listed = false;   // in FLASHFRAMES (UIFrameFlash ignores a frame that is)
        bool shown = false;
        f32 timer = 0.0f;
        f32 fadeIn = 0.0f, fadeOut = 0.0f, duration = 0.0f;
        f32 alpha = 0.0f;
    };
    static void flash(Flash& f, f32 fadeIn, f32 fadeOut, f32 duration); // UIFrameFlash
    static void stop(Flash& f); // UIFrameFlash(frame, ..., 0) as meant: hidden at once
    static void step(Flash& f, f32 dt); // UIFrameFlashUpdate + UIFrameFadeUpdate

    Flash m_lowHealth;
    Flash m_outOfControl;
};

} // namespace vd
