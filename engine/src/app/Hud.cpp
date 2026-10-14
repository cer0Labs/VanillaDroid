#include "app/Hud.h"

#include "app/FlyCamera.h"
#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/UnitFrames.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "net/UpdateFields.gen.h"
#include "pack/TerrainFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd {

using namespace hud;

bool Hud::init(UiResources* resources, u32 maxQuads) {
    if (!resources || !resources->ready()) {
        return false;
    }
    m_resources = resources;
    m_draw.init(&resources->assets(), maxQuads);
    return true;
}

void Hud::begin(const HudFrame& frame) {
    m_draw.reset();
    m_output = {};
    if (!m_resources) {
        return;
    }
    m_ui.begin(frame.width, frame.height, frame.dp, frame.safeArea, frame.nowMs);
    m_unit = frame.dp * kUnitDp;
}

void Hud::end() {
    if (m_resources) {
        m_ui.end();
    }
}

void Hud::drawGame(const HudFrame& frame) {
    VD_PROFILE_ZONE("Hud::drawGame");
    if (!m_resources) {
        return;
    }
    const Rect& safe = frame.safeArea;

    // Phase 13 (combat): LowHealthFrame and OutOfControlFrame (PlayerStatus_OnUpdate), the whole screen's edges glowing
    // under everything else.
    const PlayerFrameView* me = frame.playerFrameView;
    const f32 health = me && me->maxHealth > 0 ? static_cast<f32>(me->health) / static_cast<f32>(me->maxHealth) : 1.0f;
    m_statusFlash.update(frame.dt, health, me && me->health == 0, frame.outOfControl);
    const Rect screen{0.0f, 0.0f, frame.width, frame.height};
    if (const f32 a = m_statusFlash.lowHealthAlpha(); a > 0.0f) {
        m_draw.spriteAdditive(screen, StatusFlash::lowHealthTexture(), ui::withAlpha(ui::kWhite, a));
    }
    if (const f32 a = m_statusFlash.outOfControlAlpha(); a > 0.0f) {
        m_draw.spriteAdditive(screen, StatusFlash::outOfControlTexture(), ui::withAlpha(ui::kWhite, a));
    }

    // Names over the heads, under everything else.
    namePlates(frame);
    // Unit frames along the top.
    m_playerFrameRect = {safe.x, safe.y, u(232.0f), u(100.0f)};
    m_targetFrameRect = {safe.x + u(250.0f), safe.y, u(232.0f), u(100.0f)};
    if (!frame.unitFramesCovered) {
        playerFrame(frame, m_playerFrameRect);
        targetFrame(frame, m_targetFrameRect);
    }
    // Minimap, top right (its ring centered 87 units from the border's right edge, 92 below the zone bar's top).
    const f32 mapRadius = u(70.0f);
    const f32 mapX = safe.right() - u(87.0f) - m_ui.px(4.0f), mapY = safe.y + u(92.0f) + m_ui.px(2.0f);
    minimap(frame);
    if (frame.fps > 0.0f) {
        char fps[24];
        text::format(fps, "VD_HUD_FPS", frame.fps);
        ui::TextStyle style;
        style.size = m_ui.px(11.0f);
        style.color = ui::kWhite;
        style.outline = std::max(1.0f, m_ui.px(1.0f));
        style.align = ui::Align::Center;
        m_draw.text(mapX, mapY + mapRadius + u(30.0f), fps, style);
    }

    // Auras: the player's left of the minimap (BuffFrame), the target's under its frame (app/HudAuras.cpp).
    playerAuras(frame);
    targetAuras(frame);
    // Floating combat text, the cast bar above the bottom middle. Parity pass: the menu button on the minimap's lower
    // left rim became the micro menu (app/MicroMenu, drawn by app/GameScreens: folded into that spot where it does
    // not fit under the minimap).
    floatingTexts(frame);
    castBar(frame, {safe.centerX() - u(195.0f) * 0.5f, safe.bottom() - m_ui.px(70.0f), u(195.0f), u(13.0f)});

    // The action bar, bottom right, around the big context button; phase 20: the touch cluster instead (its layout is
    // the character's, app/TouchBarScreens) unless the player chose the classic bars.
    PlayerController::TouchInput input;
    if (frame.touch) {
        touchBar(frame, input);
    } else {
        actionBar(frame, input);
    }

    // Experience along the bottom edge (MainMenuExpBar: 13 units tall), from side to side of the screen where no
    // camera hole reaches (phase 20: the safe area left a gap at the left on a phone with its hole on that side).
    {
        const f32 top = safe.bottom() - u(13.0f), bottom = safe.bottom();
        const f32 left = frame.edges.left(top, bottom), right = frame.edges.right(top, bottom);
        experienceBar(frame, {left, top, std::max(0.0f, right - left), u(13.0f)});
    }

    // Fingers on no widget: the stick and the camera.
    worldControls(frame, input);
    heldUnit(frame); // parity: a long press on a unit keeps its tooltip
    drawStick(frame);
    // Three fingers at once on the world toggle the development panels.
    u32 fingersDown = 0, fingersBegan = 0;
    for (const ui::Touch& t : m_ui.touches()) {
        fingersDown += (!t.released && t.owner == 0) ? 1u : 0u;
        fingersBegan += (t.began && t.owner == 0) ? 1u : 0u;
    }
    if (fingersDown >= 3 && fingersBegan > 0) {
        m_debugToggle = true;
    }
    if (frame.player && !frame.freeCamera) {
        frame.player->setTouchInput(input);
    }
}

void Hud::drawStick(const HudFrame& frame) {
    (void)frame;
    if (m_stick.touch < 0) {
        return;
    }
    const f32 radius = kStickRadiusDp * m_ui.dp();
    const Rect base{m_stick.originX - radius, m_stick.originY - radius, radius * 2.0f, radius * 2.0f};
    m_draw.sprite(base, kCircleMask, rgba(0, 0, 0, 90));
    const f32 knob = radius * 0.45f;
    const f32 kx = m_stick.originX + m_stick.value.x * radius;
    const f32 ky = m_stick.originY - m_stick.value.y * radius;
    m_draw.sprite({kx - knob, ky - knob, knob * 2.0f, knob * 2.0f}, kCircleMask, rgba(255, 220, 140, 170));
}

void Hud::minimap(const HudFrame& frame) {
    // The MinimapCluster (app/MinimapView, filled by app/MinimapScreens): the same anchors as mapX / mapY above.
    if (frame.minimap) {
        const MinimapLayout layout = minimapLayout(frame.safeArea, u(1.0f), m_ui.dp());
        m_output.minimap = drawMinimap(m_ui, m_draw, *m_resources, layout, *frame.minimap, m_minimapGesture);
    }
}

} // namespace vd
