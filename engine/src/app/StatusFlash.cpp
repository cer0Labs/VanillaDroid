#include "app/StatusFlash.h"

#include "core/Hash.h"

#include <cmath>

namespace vd {
namespace {

constexpr f32 kLowHealth = 0.2f; // PlayerStatus_OnUpdate: flashes from 20 % (and, here, stops above it)
constexpr f32 kFade = 0.5f;           // UIFrameFlash(frame, 0.5, 0.5, 100)
constexpr f32 kFlashSeconds = 100.0f;

} // namespace

u64 StatusFlash::lowHealthTexture() {
    static constexpr u64 kHash = assetPathHash("Interface\\FullScreenTextures\\LowHealth.blp");
    return kHash;
}

u64 StatusFlash::outOfControlTexture() {
    static constexpr u64 kHash = assetPathHash("Interface\\FullScreenTextures\\OutOfControl.blp");
    return kHash;
}

void StatusFlash::reset() {
    m_lowHealth = Flash{};
    m_outOfControl = Flash{};
}

void StatusFlash::flash(Flash& f, f32 fadeIn, f32 fadeOut, f32 duration) {
    if (f.listed) {
        return; // "If frame is already set to flash then return"
    }
    f.listed = true;
    f.shown = true; // UIFrameFade shows it, from alpha 0
    f.timer = 0.0f;
    f.fadeIn = fadeIn;
    f.fadeOut = fadeOut;
    f.duration = duration;
    f.alpha = 0.0f;
}

void StatusFlash::stop(Flash& f) {
    f.listed = false;
    f.shown = false;
    f.alpha = 1.0f;
    f.flashing = false;
}

void StatusFlash::step(Flash& f, f32 dt) {
    if (!f.listed) {
        return;
    }
    f.timer += dt;
    if (f.timer > f.duration) { // flashDuration exceeded: alpha 1, hidden (showWhenDone is nil); again if still due
        stop(f);
        return;
    }
    // Fading in, then out, again and again.
    const f32 cycle = f.fadeIn + f.fadeOut;
    const f32 t = cycle > 0.0f ? std::fmod(f.timer, cycle) : 0.0f;
    f.alpha = t < f.fadeIn ? t / f.fadeIn : 1.0f - (t - f.fadeIn) / f.fadeOut;
}

void StatusFlash::update(f32 dt, f32 healthFraction, bool dead, bool outOfControl, bool enabled) {
    // PlayerStatus_OnUpdate, one branch per frame in this order (the stops as it means them).
    const bool lowHealth = healthFraction <= kLowHealth && !dead;
    if (lowHealth && !m_lowHealth.flashing && enabled) {
        flash(m_lowHealth, kFade, kFade, kFlashSeconds);
        m_lowHealth.flashing = true;
    } else if (!lowHealth && m_lowHealth.flashing) {
        stop(m_lowHealth);
    } else if (outOfControl && !m_outOfControl.flashing && enabled) {
        flash(m_outOfControl, kFade, kFade, kFlashSeconds);
        m_outOfControl.flashing = true;
    } else if (!outOfControl && m_outOfControl.flashing) {
        stop(m_outOfControl);
    }
    // UIFrameFlashUpdate (UIParent's OnUpdate).
    step(m_lowHealth, dt);
    step(m_outOfControl, dt);
}

} // namespace vd
