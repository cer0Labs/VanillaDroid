#include "app/Hud.h"

#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/UnitFrames.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>

namespace vd {

using namespace hud;

// The views come from app/UnitScreens (GameScreens::prepareHud); the drawing is app/UnitFrames'.

void Hud::experienceBar(const HudFrame& frame, const Rect& area) {
    m_experienceBarRect = area;
    if (!frame.experienceView) {
        return;
    }
    ui::Gui g{m_ui, m_draw};
    ExperienceBarView view = *frame.experienceView;
    view.showText = view.showText || frame.statusBarText; // phase 15 (options): STATUS_BAR_TEXT (MainMenuExpBar)
    m_output.experienceBar = vd::experienceBar(g, area, view);
}

void Hud::playerFrame(const HudFrame& frame, const Rect& area) {
    ui::Gui g{m_ui, m_draw};
    if (!frame.playerFrameView) { // offline (development): the frame with a stand-in name
        playerFramePlaceholder(g, area, text::get("VD_LOGIN_OFFLINE"));
        return;
    }
    PlayerFrameView view = *frame.playerFrameView;
    view.barText = frame.statusBarText; // phase 15 (options)
    const UnitFrameResult r = vd::playerFrame(g, area, view, frame.nowMs);
    m_output.playerFrame = r.tapped;
    m_output.playerFrameHold = r.held;
    m_output.playerBadge = r.badgeTapped;
}

void Hud::targetFrame(const HudFrame& frame, const Rect& area) {
    if (!frame.targetFrameView) {
        return;
    }
    ui::Gui g{m_ui, m_draw};
    const UnitFrameResult r = vd::targetFrame(g, area, *frame.targetFrameView);
    m_output.targetFrame = r.tapped; // phase 13: the unit tooltip kept (the hover); held: the menu
    m_output.targetFrameHold = r.held;
}

void Hud::heldUnit(const HudFrame& frame) {
    if (!frame.session || !frame.session->inWorld() || frame.freeCamera) {
        return;
    }
    for (const ui::Touch& t : m_ui.touches()) {
        if (t.owner != 0 || t.released || t.moved || t.id == m_stick.touch ||
            m_ui.nowMs() - t.downMs < ui::Context::kLongPressMs) {
            continue;
        }
        if (t.id == m_heldTouch && t.downMs == m_heldDownMs) {
            continue; // reported already
        }
        m_heldTouch = t.id;
        m_heldDownMs = t.downMs;
        if (const u64 picked = pickUnit(frame, t.x, t.y)) {
            m_output.unitHold = picked;
        } else if (const u64 object = pickObject(frame, t.x, t.y)) { // phase 16 (alerts): its tooltip
            m_output.objectHold = object;
        }
    }
}

} // namespace vd
