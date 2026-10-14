#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

namespace vd {

// Development (retouches, ui models): the 1.12 interface's models drawn by ui/UiModels, as VD_UI_GALLERY pages:
//  - "cooldowns": UI-Cooldown-Indicator.m2 on action buttons at fixed points of the cooldown (0 to 95 %) above the look
//    it replaced (a dark fill from the top and the seconds), its flash at fixed moments, the same on the touch
//    cluster's round icons (the flash too), and live ones (a 4 s cooldown every 5 s: the sweep, then the flash).
//  - "autocast": UI-AutoCastButton.m2 on the pet bar's 30-unit buttons (model scale 1.2) and the spell book's
//    36-unit ones (1.22), at several sizes, with every particle (left) and at most the HUD's kParticleLimit per
//    emitter (right; VD_UIMODELS_PARTICLES=<n>: another limit there).
//  - "minimapping": MinimapPing.m2 at fixed moments of its Birth and Stand over a dark disc, and a live one every 6 s.
//  - "roundicons": the touch cluster's round icon (app/TouchRoundIcon) in each state of the 1.12 button: normal,
//    pushed, checked, auto attack flash, equipped, unusable, out of range, short of power, the edit mode's empty and
//    chosen places.
class UiModelsGallery {
public:
    enum class Page : u8 { Cooldowns, AutoCast, MinimapPing, RoundIcons };
    bool select(const char* name);
    void draw(DialogContext& c, u64 nowMs);

private:
    void drawCooldowns(DialogContext& c, u64 nowMs);
    void drawAutoCast(DialogContext& c, u64 nowMs);
    void drawMinimapPing(DialogContext& c, u64 nowMs);
    void drawRoundIcons(DialogContext& c, u64 nowMs);

    Page m_page = Page::Cooldowns;
    u64 m_startMs = 0;
};

} // namespace vd
