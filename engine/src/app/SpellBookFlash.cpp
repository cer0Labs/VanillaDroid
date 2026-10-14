#include "app/SpellBookFlash.h"

#include "ui/ChatDock.h"

namespace vd {
namespace {

// UIFrameFlash(SpellBookTabFlashFrame, 0.5, 0.5, 30, nil): UIFrameFade in from 0 and out to 0, no holds.
constexpr ui::FlashTiming kTiming{SpellBookFlash::kFadeMs, SpellBookFlash::kFadeMs, 0, 0,
                                  SpellBookFlash::kDurationMs};

} // namespace

void SpellBookFlash::learned(u32 tab, bool petPage) {
    if (petPage || tab >= kMaxTabs) {
        return; // SpellBookFrame_OnEvent returns for BOOKTYPE_PET; there is no texture past the eighth tab
    }
    m_textures |= 1u << tab;
    m_flashTabs = true;
}

void SpellBookFlash::shown(u64 nowMs) {
    if (m_flashTabs && !m_flashing) {
        m_flashing = true;
        m_flashStart = nowMs;
    }
}

void SpellBookFlash::hidden() {
    m_flashing = false; // UIFrameFlashRemoveFrame
    m_textures = 0;
}

void SpellBookFlash::clicked(u32 tab) {
    if (tab < kMaxTabs) {
        m_textures &= ~(1u << tab);
    }
}

f32 SpellBookFlash::alpha(u32 tab, u64 nowMs) const {
    if (!m_flashing || !pending(tab) || nowMs < m_flashStart) {
        return 0.0f;
    }
    // Over (UIFrameFlashUpdate hides the frame when its time is up; it stays in no list to be flashed again
    // until the book shows again).
    return ui::flashAlpha(nowMs - m_flashStart, kTiming);
}

} // namespace vd
