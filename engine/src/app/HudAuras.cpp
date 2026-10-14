#include "app/Hud.h"

#include "app/AuraLayout.h"
#include "app/AuraWindows.h"
#include "app/HudShared.h"
#include "app/UiResources.h"
#include "core/Profile.h"

#include <algorithm>
#include <array>

namespace vd {

using namespace hud;

void Hud::playerAuras(const HudFrame& frame) {
    m_playerAurasRect = {}; // phase 17 (scores)
    if (frame.playerAuras.empty()) {
        return;
    }
    VD_PROFILE_ZONE("Hud::playerAuras");
    // TemporaryEnchantFrame's TOPRIGHT: 175 units in from the screen's right edge and 13 down, from where the
    // minimap cluster's top right corner is (app/MinimapView: 4 dp in, 2 dp down).
    const Rect& safe = frame.safeArea;
    const f32 right = safe.right() - m_ui.px(4.0f) - u(175.0f);
    f32 top = safe.y + m_ui.px(2.0f) + u(13.0f);
    // Phase 15 (options): the wide screens' extra action bars along the top (last frame's place): the rows hang under
    // them where they reach into the buffs' (8 buffs and 2 enchantments wide).
    const Rect& bars = m_extraBarRect;
    if (bars.w > 0.0f && bars.y < top + u(30.0f) && bars.right() > right - u(8.0f * 35.0f + 2.0f * 32.0f + 5.0f)) {
        top = bars.bottom() + m_ui.px(6.0f);
    }
    std::array<AuraButtonPlace, kMaxPlayerBuffs + kMaxPlayerDebuffs + kMaxTempEnchants> places{};
    // Phase 15 (options): SHOW_BUFF_DURATIONS (BuffButtons_UpdatePositions, BuffFrame_UpdateDuration).
    const u32 count = buffFrameLayout(frame.playerAuras, right, top, u(1.0f), places, frame.showBuffDurations);
    for (u32 i = 0; i < count; ++i) { // phase 17 (scores): the rows' extent
        const Rect& r = places[i].hit;
        const Rect& a = m_playerAurasRect;
        m_playerAurasRect = a.w <= 0.0f ? r
                                        : Rect{std::min(a.x, r.x), std::min(a.y, r.y),
                                               std::max(a.right(), r.right()) - std::min(a.x, r.x),
                                               std::max(a.bottom(), r.bottom()) - std::min(a.y, r.y)};
    }
    const AuraButtonsResult r =
        auraButtons(m_ui, m_draw, *m_resources, frame.playerAuras, {places.data(), count}, u(1.0f), frame.nowMs,
                    frame.showBuffDurations ? AuraButtonStyle::Player : AuraButtonStyle::PlayerNoDuration,
                    "aura.player");
    if (r.tapped >= 0 || r.held >= 0) {
        m_output.auraTapped = r.tapped;
        m_output.auraHeld = r.held;
        m_output.auraOfTarget = false;
        // Touch: the tooltip hangs under the rows (the client's under the button would cover the next rows'
        // buttons).
        m_output.auraRect = {r.rect.x, r.rect.y, r.rect.w, r.bottom - r.rect.y};
    }
}

void Hud::targetAuras(const HudFrame& frame) {
    m_targetAurasBottom = m_targetFrameRect.bottom();
    if (frame.targetAuras.empty()) {
        return;
    }
    VD_PROFILE_ZONE("Hud::targetAuras");
    std::array<AuraButtonPlace, kMaxTargetBuffs + kMaxTargetDebuffs> places{};
    const u32 count = targetAuraLayout(frame.targetAuras, frame.targetFriendly, frame.targetOfTarget,
                                       m_targetFrameRect, u(1.0f), places, m_targetAurasBottom);
    const AuraButtonsResult r =
        auraButtons(m_ui, m_draw, *m_resources, frame.targetAuras, {places.data(), count}, u(1.0f),
                    frame.nowMs, AuraButtonStyle::Target, "aura.target");
    if (r.tapped >= 0 || r.held >= 0) {
        m_output.auraTapped = r.tapped;
        m_output.auraHeld = r.held;
        m_output.auraOfTarget = true;
        m_output.auraRect = {r.rect.x, r.rect.y, r.rect.w, r.bottom - r.rect.y}; // the tooltip under the rows
    }
}

} // namespace vd
