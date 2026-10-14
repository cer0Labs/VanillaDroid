#include "app/HudTouch.h"

#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/TouchRoundIcon.h" // retouches (ui models)
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {

using namespace hud;
using game::TouchPlace;
using game::touchIndex;

namespace {

// docs/MOBILE_CONTROLS.md §4: the touch cluster's gestures and looks.
constexpr f32 kMarginMm = 2.0f;       // §3.5: a finger this far outside a button still takes it (the nearest)
constexpr u64 kFanDelayMs = 250;      // held still this long, a group opens its fan
constexpr u64 kRepeatDelayMs = 350;   // ...a repeating button starts firing again
constexpr f32 kSwipeMm = 6.0f;        // the templates' button: a sideways swipe this long changes template
constexpr u64 kNameShownMs = 1500;    // the template's name after a change
constexpr f32 kPi = 3.14159265f;

constexpr u32 kRingGold = rgba(214, 174, 82);
constexpr u32 kUnusableTint = rgba(102, 102, 102); // ActionButton_UpdateUsable's
constexpr u64 kGlowRing = assetPathHash("Interface\\Buttons\\IconBorder-GlowRing.blp");

// Phase 20 (assists, §4 "Apuntar arrastrando"): the ground aim's pad round a dragged button (a thumb's reach).
constexpr f32 kAimPadMm = 22.0f;

// Phase 20 (assists): where a dragged button's finger is: back on the button (it cancels there), and its offset as a
// share of the ground aim's pad (0 at the button's rim, 1 at the pad's edge).
void dragAim(f32 mm, f32 cx, f32 cy, f32 r, f32 x, f32 y, TouchBarOutput& out) {
    const f32 dx = x - cx, dy = y - cy, d = std::hypot(dx, dy);
    const f32 inner = r + kMarginMm * mm;
    const f32 pad = std::max(kAimPadMm * mm, inner * 2.0f);
    out.dragOnButton = d <= inner;
    const f32 share = d > inner ? std::min(1.0f, (d - inner) / (pad - inner)) / d : 0.0f;
    out.dragNx = dx * share;
    out.dragNy = dy * share;
}

f32 pulse(u64 nowMs, u64 periodMs) {
    return 0.5f + 0.5f * std::sin(static_cast<f32>(nowMs % periodMs) * (2.0f * kPi / static_cast<f32>(periodMs)));
}

// Retouches (ui models): the client's red button art over a dragged button back on its place (it cancels there).
constexpr u64 kQuickslotRed = assetPathHash("Interface\\Buttons\\UI-QuickslotRed.blp");

// Retouches (ui models): ComboFrame's art and its fades (COMBO_FRAME_LAST_NUM_POINTS and its UIFrameFade timers). One
// HUD per process.
constexpr u64 kComboPoint = assetPathHash("Interface\\ComboFrame\\ComboPoint.blp");
struct ComboFade {
    u32 last = 0;
    u64 shownMs = 0;
    std::array<u64, 5> gainedMs{};
};
ComboFade g_combo;

} // namespace

void Hud::touchButton(f32 cx, f32 cy, f32 r, const TouchButtonView& button, bool pressed, f32 alpha) {
    const HudActionSlot& slot = button.slot;
    // The round icon without a rim or a socket (app/TouchRoundIcon); it still shrinks while pressed, pulses a gold
    // ring while current and keeps our cooldown.
    const f32 rr = pressed ? r * kRoundIconPressed : r;
    const f32 iconR = rr;
    const u64 now = m_ui.nowMs();
    const bool unusable = !slot.usable && !slot.noMana;
    RoundIconState state;
    state.alpha = alpha;
    const std::optional<ui::Sprite> glyph =
        button.glyph != 0 ? m_draw.assets()->sprite(button.glyph) : std::optional<ui::Sprite>{};
    if (button.portrait.valid()) { // phase 20 (pets): the pet's portrait
        m_draw.textureInCircle({cx - iconR, cy - iconR, 2.0f * iconR, 2.0f * iconR}, button.portrait, cx, cy, iconR, {},
                               withAlpha(unusable ? kUnusableTint : ui::kWhite, alpha));
        drawRoundIconStates(m_draw, *m_resources, cx, cy, iconR, slot, state, now);
    } else if (glyph) {
        // Our own art (art/touch): light parchment over its dark disc, grey when unusable.
        m_draw.ring(cx, cy, rr, 0.0f, withAlpha(rgba(0, 0, 0), 0.55f * alpha));
        const f32 g = rr * 0.62f;
        const u32 tint = unusable ? kUnusableTint : (slot.checked || slot.active) ? rgba(255, 220, 120) : rgba(244, 232, 200);
        m_draw.sprite({cx - g, cy - g, 2.0f * g, 2.0f * g}, *glyph, withAlpha(tint, alpha));
        drawRoundIconStates(m_draw, *m_resources, cx, cy, iconR, slot, state, now);
    } else {
        drawRoundIcon(m_draw, *m_resources, cx, cy, iconR, slot, state, now);
    }
    if (button.glow || slot.highlight) { // a reactive ability usable now, a place for the spell being placed
        const f32 g = rr * 1.55f;
        const u32 color = slot.highlight ? rgba(120, 255, 120) : rgba(255, 230, 120);
        m_draw.spriteAdditive({cx - g, cy - g, 2.0f * g, 2.0f * g}, kGlowRing,
                              withAlpha(color, (0.5f + 0.5f * pulse(now, 700)) * alpha));
    }
    if (slot.count > 0) {
        char count[8];
        std::snprintf(count, sizeof(count), "%u", slot.count);
        ui::TextStyle style;
        style.size = std::max(m_ui.px(10.0f), rr * 0.55f);
        style.color = withAlpha(ui::kWhite, alpha);
        style.outline = std::max(1.0f, rr * 0.05f);
        style.align = ui::Align::Right;
        m_draw.text(cx + rr * 0.95f, cy + rr * 0.95f, count, style);
    }
    if (!slot.label.empty()) { // a macro's name
        ui::TextStyle style;
        style.size = std::max(m_ui.px(9.0f), rr * 0.42f);
        style.color = withAlpha(ui::kWhite, alpha);
        style.outline = std::max(1.0f, rr * 0.04f);
        style.align = ui::Align::Center;
        m_draw.pushClip({cx - iconR, cy - iconR, 2.0f * iconR, 2.0f * iconR});
        m_draw.text(cx, cy + iconR * 0.8f, slot.label, style);
        m_draw.popClip();
    }
    if (button.group) { // a group: a small mark on the rim, down right
        const f32 d = rr * 0.82f, t = rr * 0.3f;
        const f32 ax = cx + d * 0.7071f, ay = cy + d * 0.7071f;
        m_draw.triangle(ax + t * 0.5f, ay + t * 0.5f, ax + t * 0.5f, ay - t * 0.6f, ax - t * 0.6f, ay + t * 0.5f,
                        withAlpha(kRingGold, alpha));
    }
    if (button.isNew) {
        ui::TextStyle style;
        style.size = std::max(m_ui.px(9.0f), rr * 0.4f);
        style.color = withAlpha(rgba(120, 255, 120), alpha);
        style.outline = std::max(1.0f, rr * 0.04f);
        style.align = ui::Align::Center;
        m_draw.text(cx, cy - rr - style.size * 0.15f, text::get("VD_TOUCH_NEW"), style);
    }
    if (button.cornerIcon != 0) {
        const f32 c = rr * 0.5f;
        m_draw.sprite({cx + rr * 0.45f, cy - rr * 1.0f, c, c}, button.cornerIcon, withAlpha(ui::kWhite, alpha),
                      button.cornerUv); // phase 20 (pets): a face of UI-PetHappiness
    }
}

void Hud::touchGesture(const HudFrame& frame, i32 place, const ui::WidgetState& state, const TouchButtonView& button,
                       f32 cx, f32 cy, f32 r) {
    TouchPress& press = m_touchPress[static_cast<u32>(place)];
    TouchBarOutput& out = m_output.touch;
    const ui::Touch* t = state.touch;
    if (!t) {
        press = {};
        return;
    }
    if (frame.touch->dimmed && !button.breaksControl) {
        return; // no control: nothing fires (phase 20, pets: but what breaks it)
    }
    const u64 now = m_ui.nowMs();
    const u64 heldMs = now - t->downMs;
    const bool outside = std::hypot(t->x - cx, t->y - cy) > r + kMarginMm * frame.mm;
    const f32 aimMm = frame.mm > 0.0f ? frame.mm : m_ui.dp() * (160.0f / 25.4f); // phase 20 (assists)
    if (t->began && button.onPress && !button.group) { // §4: hostile spells fire as the finger goes down
        out.pressed = place;
        press.fired = true;
    }
    if (state.pressed) {
        if (button.aim && (press.dragging || (t->moved && outside))) { // §4: dragged out, it aims
            press.dragging = true;
            out.dragPlace = place;
            out.dragX = t->x;
            out.dragY = t->y;
            dragAim(aimMm, cx, cy, r, t->x, t->y, out); // phase 20 (assists)
        } else if (button.group && m_fanPlace != place && !press.held &&
                   ((!t->moved && heldMs >= kFanDelayMs) || (t->moved && outside))) {
            out.fanOpen = place; // its actions come from the screens next frame (TouchBarView::fan)
            m_fanPlace = place;
            m_fanHover = -1;
            m_fanX = cx;
            m_fanY = cy;
            m_fanStart = 0.0f;
            press.held = true;
        } else if (button.repeat && press.fired && !t->moved && heldMs >= kRepeatDelayMs) {
            out.repeating = place; // §8: held, it fires again whenever it can (the screens decide when)
        } else if (state.longPress && !button.group && !button.repeat) {
            out.held = place;
            out.heldRect = {cx - r, cy - r, 2.0f * r, 2.0f * r};
            press.held = true;
        }
        return;
    }
    // Lifted.
    if (press.dragging) {
        out.dragPlace = place;
        out.dragX = t->x;
        out.dragY = t->y;
        // Phase 20 (assists): lifted back on its button, nothing is cast.
        dragAim(aimMm, cx, cy, r, t->x, t->y, out);
        out.dragDropped = !t->cancelled && !out.dragOnButton;
    } else if (m_fanPlace == place) {
        if (m_fanHover >= 0 && !t->cancelled) {
            out.fanPlace = place;
            out.fanChosen = m_fanHover;
        }
        m_fanPlace = -1;
    } else if (state.clicked && !press.fired && !press.held) {
        out.pressed = place;
    }
    press = {};
}

void Hud::touchFan(const HudFrame& frame, const TouchFrame& at) {
    const TouchBarView& view = *at.view;
    // The class bar's single button (wide screens) fans its own buttons; the others' items come from the screens.
    const bool classFan = m_fanPlace == static_cast<i32>(touchIndex(TouchPlace::ClassBar));
    // Phase 20 (smart, §6.5): the Target button's fan: the units it offers, from the HUD's own frame.
    const bool targetFan = m_fanPlace == static_cast<i32>(touchIndex(TouchPlace::Target));
    std::array<TargetOption, 5> targets;
    const u32 targetCount = targetFan ? targetOptions(frame, targets) : 0u;
    const u32 count = targetFan  ? targetCount
                      : classFan ? view.classBarCount
                                 : (view.fanPlace == m_fanPlace ? view.fanCount : 0u);
    if (m_fanPlace < 0 || count == 0) {
        return;
    }
    // A ring of items around the held button (§6.6), pushed inward so it stays on the screen; the first item
    // toward the screen's center.
    const f32 item = std::max(4.0f * at.scale, m_ui.px(15.0f)); // its radius: 8 % of the height across, as the column
    const f32 fanRadius = std::max(item * 2.6f, 2.3f * item * static_cast<f32>(count) / (2.0f * kPi));
    const Rect& safe = frame.safeArea;
    const f32 reach = fanRadius + item * 1.1f;
    const f32 fx = std::clamp(m_fanX, safe.x + reach, safe.right() - reach);
    const f32 fy = std::clamp(m_fanY, safe.y + reach, safe.bottom() - reach);
    const f32 start = std::atan2(frame.height * 0.5f - fy, frame.width * 0.5f - fx);
    // The finger: the item nearest to it, if near enough.
    const ui::Touch* finger = nullptr;
    for (const ui::Touch& t : m_ui.touches()) {
        if (t.owner == ui::widgetId("touch", static_cast<u64>(m_fanPlace)) ||
            (m_fanPlace >= kTouchClassBarFan && t.owner == ui::widgetId("touch-class", static_cast<u64>(m_fanPlace - kTouchClassBarFan)))) {
            finger = &t;
        }
    }
    if (!finger) {
        m_fanPlace = -1; // its button went away (a form changed under the finger)
        return;
    }
    m_draw.ring(fx, fy, reach + item * 0.3f, 0.0f, rgba(0, 0, 0, 120));
    m_fanHover = -1;
    f32 best = item * 1.6f;
    for (u32 i = 0; i < count; ++i) {
        const f32 angle = start + 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(count);
        const f32 x = fx + fanRadius * std::cos(angle), y = fy + fanRadius * std::sin(angle);
        const f32 d = std::hypot(finger->x - x, finger->y - y);
        if (d < best) {
            best = d;
            m_fanHover = static_cast<i32>(i);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const f32 angle = start + 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(count);
        const f32 x = fx + fanRadius * std::cos(angle), y = fy + fanRadius * std::sin(angle);
        TouchButtonView b;
        if (targetFan) {
            b.slot = HudActionSlot{targets[i].icon};
        } else if (classFan) {
            b = view.classBar[i];
        } else {
            b.slot = view.fan[i];
            b.glow = i == view.fanCurrent;
        }
        const bool hover = m_fanHover == static_cast<i32>(i);
        touchButton(x, y, hover ? item * 1.18f : item, b, false, 1.0f);
        if (targetFan) { // phase 20 (smart): the player's own portrait; what the one under the finger is
            const f32 r = (hover ? item * 1.18f : item) * 0.88f;
            if (frame.session && targets[i].guid != 0 && targets[i].guid == frame.session->playerGuid() &&
                frame.playerPortrait.valid()) {
                m_draw.textureInCircle({x - r, y - r, 2.0f * r, 2.0f * r}, frame.playerPortrait, x, y, r);
            }
            if (hover) {
                ui::TextStyle style;
                style.size = m_ui.px(14.0f);
                style.color = ui::kGold;
                style.outline = std::max(1.0f, m_ui.px(1.0f));
                style.align = ui::Align::Center;
                const f32 below = fy + reach + item * 0.3f + style.size;
                m_draw.text(fx, below < frame.safeArea.bottom() ? below : fy - reach - item * 0.3f - style.size * 0.3f,
                            targets[i].label, style);
            }
        }
    }
}

void Hud::targetButton(const HudFrame& frame, const ui::WidgetState& state, bool enabled, f32 cx, f32 cy,
                       f32 scale) {
    // Phase 20 (smart, docs/MOBILE_CONTROLS.md §6.5): a tap targets the next enemy, a swipe up / down the next /
    // previous one, a hold opens the fan (slide to one and lift: the nearest enemy, a party member, the target's target,
    // the pet, yourself; lifted on the button: nothing).
    const u32 place = touchIndex(TouchPlace::Target);
    TouchPress& press = m_touchPress[place];
    const ui::Touch* t = state.touch;
    if (!t) {
        press = {};
        return;
    }
    if (!enabled) {
        return;
    }
    if (state.pressed) {
        if (m_fanPlace != static_cast<i32>(place) && !press.held && !t->moved &&
            m_ui.nowMs() - t->downMs >= kFanDelayMs) {
            m_fanPlace = static_cast<i32>(place);
            m_fanHover = -1;
            m_fanX = cx;
            m_fanY = cy;
            press.held = true;
        }
        return;
    }
    // Lifted.
    if (m_fanPlace == static_cast<i32>(place)) {
        std::array<TargetOption, 5> options;
        const u32 count = targetOptions(frame, options);
        if (m_fanHover >= 0 && static_cast<u32>(m_fanHover) < count && !t->cancelled && frame.session) {
            frame.session->setTarget(options[static_cast<u32>(m_fanHover)].guid);
        }
        m_fanPlace = -1;
    } else if (!press.held && !t->cancelled) {
        const f32 dx = t->x - t->startX, dy = t->y - t->startY;
        if (!frame.session) {
            // VD_TOUCH_SMART_DEMO: nothing to target
        } else if (std::fabs(dy) >= kSwipeMm * scale && std::fabs(dy) > std::fabs(dx)) {
            frame.session->setTarget(nextEnemy(frame, dy < 0.0f ? 1 : -1)); // up: the next one
        } else if (state.clicked) {
            frame.session->setTarget(nextEnemy(frame));
        }
    }
    press = {};
}

void Hud::touchBar(const HudFrame& frame, PlayerController::TouchInput& input) {
    VD_PROFILE_ZONE("Hud::touchBar");
    const TouchBarView& view = *frame.touch;
    m_bottomRowRect = {};
    m_stanceBarRect = {};
    m_extraBarRect = {};
    m_drawnMultiBars = 0;
    if (!view.geometry) {
        return;
    }
    primeRoundIconTextures(*m_resources); // retouches (ui models)
    const game::TouchGeometry& g = *view.geometry;
    const Rect& safe = frame.safeArea;
    const f32 mm = frame.mm > 0.0f ? frame.mm : m_ui.dp() * (160.0f / 25.4f);
    // The places' extent (hundredths of the height); the whole cluster shrinks when the screen is too small for it.
    f32 minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    for (u32 p = 0; p < game::kTouchPlaces; ++p) {
        const game::TouchPlacement& at = g.places[p];
        if (at.size <= 0.0f) {
            continue;
        }
        minX = std::min(minX, at.x - at.size * 0.5f);
        maxX = std::max(maxX, at.x + at.size * 0.5f);
        minY = std::min(minY, at.y - at.size * 0.5f);
        maxY = std::max(maxY, at.y + at.size * 0.5f);
    }
    // Hundredths of the safe area's height (the proportions of the reference's 16:9 screen, §12), or of the height a
    // 16:9 screen of its width would have when it is squarer (the Fold5's inner screen), times the player's scale.
    const f32 wanted = std::clamp(g.scale, 0.5f, 1.6f) * std::min(safe.h, safe.w * (9.0f / 16.0f)) * 0.01f;
    const f32 fit = std::min({1.0f, safe.h * 0.96f / std::max(1.0f, (g.mainBottom + maxY) * wanted),
                              safe.w * 0.62f / std::max(1.0f, (g.mainRight - minX) * wanted)});
    TouchFrame at;
    at.view = &view;
    at.geometry = &g;
    at.scale = wanted * fit;
    at.mirrored = view.mirrored;
    at.mainX = view.mirrored ? safe.x + g.mainRight * at.scale : safe.right() - g.mainRight * at.scale;
    at.mainY = safe.bottom() - g.mainBottom * at.scale;
    {
        const f32 x0 = at.mainX + (view.mirrored ? -maxX : minX) * at.scale;
        const f32 x1 = at.mainX + (view.mirrored ? -minX : maxX) * at.scale;
        m_actionBarRect = {x0, at.mainY - maxY * at.scale, x1 - x0, (maxY - minY) * at.scale};
        // The cast bar keeps over the row of the jump (Hud::castBar).
        const u32 jump = touchIndex(TouchPlace::Jump);
        const f32 rowTop = at.y(jump) - at.radius(jump);
        m_bottomRowRect = {x0, rowTop, x1 - x0, safe.bottom() - rowTop};
    }
    if (view.editing) {
        touchEdit(frame, at); // §9 (app/HudTouchEdit.cpp)
        return;
    }
    const f32 alpha = view.dimmed ? 0.45f : 1.0f;
    const f32 margin = kMarginMm * mm;
    const bool online = frame.session && frame.session->inWorld();
    for (u32 p = 0; p < game::kTouchPlaces; ++p) {
        if (g.places[p].size <= 0.0f || p == touchIndex(TouchPlace::ClassBar)) {
            continue;
        }
        const f32 cx = at.x(p), cy = at.y(p), r = at.radius(p);
        TouchButtonView button = view.places[p];
        const TouchPlace place = static_cast<TouchPlace>(p);
        // The client's own buttons: the jump, the next enemy, the context button (§6.4: what the old big button did).
        if (place == TouchPlace::Jump) {
            button.shown = true;
            button.slot = HudActionSlot{kIconJump};
            button.glyph = kTouchGlyphJump;
        } else if (place == TouchPlace::Target) {
            button.shown = true;
            button.slot = HudActionSlot{kIconNextTarget};
            button.glyph = kTouchGlyphTarget;
            button.slot.usable = online || frame.targetFanDemo;
        } else if (place == TouchPlace::Interact) {
            button.shown = frame.context != HudContextAction::None;
            switch (frame.context) {
            case HudContextAction::Attack: button.slot.iconHash = kIconAttack; break;
            case HudContextAction::StopAttack:
                button.slot.iconHash = kIconAttack;
                button.slot.active = true;
                break;
            case HudContextAction::Talk: button.slot.iconHash = kIconTalk; break;
            case HudContextAction::Loot: button.slot.iconHash = kIconLoot; break;
            case HudContextAction::Use: button.slot.iconHash = kIconUse; break;
            case HudContextAction::Resurrect: // phase 20 (smart): the spell's icon
            case HudContextAction::Skin: button.slot.iconHash = frame.contextActionIcon; break;
            default: break;
            }
            button.slot.inRange = frame.contextInRange;
        }
        if (!button.shown) {
            m_touchPress[p] = {};
            continue;
        }
        const ui::WidgetState state = m_ui.widgetCircle(ui::widgetId("touch", p), cx, cy, r, margin);
        touchButton(cx, cy, r, button, state.pressed, button.breaksControl ? 1.0f : alpha); // phase 20 (pets)
        if (place == TouchPlace::Jump) { // pressed like a key (on touch, not on release)
            input.jump = !view.dimmed && state.touch && state.touch->began;
        } else if (place == TouchPlace::Target) {
            targetButton(frame, state, (online || frame.targetFanDemo) && !view.dimmed, cx, cy, at.scale); // §6.5
        } else if (place == TouchPlace::Interact) {
            // No subject label over it (the classic bars' context button keeps it): in the cluster its icon says
            // what it does.
            if (state.longPress) {
                m_output.contextHold = true;
                m_contextHeld = true;
            }
            if (state.clicked && !m_contextHeld && !view.dimmed) {
                m_output.context = true;
            }
            if (state.released) {
                m_contextHeld = false;
            }
        } else if (place == TouchPlace::Templates) {
            // §5.3: a tap lists them; a sideways swipe goes to the previous / next one.
            const ui::Touch* t = state.touch;
            if (t && t->released && !t->cancelled && std::fabs(t->x - t->startX) >= kSwipeMm * at.scale &&
                std::fabs(t->x - t->startX) > std::fabs(t->y - t->startY)) {
                m_output.touch.swipe = t->x > t->startX ? 1 : -1;
                m_touchPress[p] = {};
            } else {
                touchGesture(frame, static_cast<i32>(p), state, button, cx, cy, r);
            }
            if (view.templateCount > 1 || !view.templateName.empty()) {
                char number[8];
                std::snprintf(number, sizeof(number), "%u", view.templateIndex + 1);
                ui::TextStyle style;
                style.size = r * 0.9f;
                style.color = ui::kGold;
                style.outline = std::max(1.0f, r * 0.06f);
                style.align = ui::Align::Center;
                m_draw.text(cx + r * 0.55f, cy + r * 0.95f, number, style);
            }
            if (!view.templateName.empty() && m_ui.nowMs() - view.templateShownMs < kNameShownMs) {
                ui::TextStyle style;
                style.size = m_ui.px(14.0f);
                style.color = ui::kGold;
                style.outline = std::max(1.0f, m_ui.px(1.0f));
                style.align = ui::Align::Right;
                const f32 x = view.mirrored ? cx + r + m_draw.measure(view.templateName, style.font, style.size)
                                            : cx - r - m_ui.px(6.0f);
                m_draw.text(x, cy + style.size * 0.35f, view.templateName, style);
            }
        } else {
            touchGesture(frame, static_cast<i32>(p), state, button, cx, cy, r);
        }
    }
    // Combo points: on an arc over Main. Retouches (ui models): ComboFrame's art and fades (ComboFrame.xml / .lua):
    // each point a 12 x 16 socket (ComboPoint.blp 0..0.375), its gem 8 x 16 two units in (0.375..0.5625) fading in over
    // 0.4 s when gained, then its shine 14 x 16 four units up (0.5625..1, ADD) fading in over 0.3 s and out over 0.4 s
    // (the fifth point's frame is larger, its textures the same); the frame fades in over 0.3 s with the first point.
    if (view.comboPoints > 0) {
        const u64 now = m_ui.nowMs();
        if (g_combo.last == 0) {
            g_combo.shownMs = now;
        }
        for (u32 i = g_combo.last; i < std::min<u32>(view.comboPoints, 5); ++i) {
            g_combo.gainedMs[i] = now;
        }
        g_combo.last = view.comboPoints;
        const u32 main = touchIndex(TouchPlace::Main);
        const f32 cx = at.x(main), cy = at.y(main), r = at.radius(main);
        const f32 gem = std::max(1.2f * at.scale, m_ui.px(5.0f));
        const f32 frameAlpha = std::clamp(static_cast<f32>(now - g_combo.shownMs) / 300.0f, 0.0f, 1.0f);
        for (u32 i = 0; i < 5; ++i) {
            const f32 angle = (150.0f - 30.0f * static_cast<f32>(i)) * (kPi / 180.0f);
            const f32 x = cx + (r + gem * 1.6f) * std::cos(angle), y = cy - (r + gem * 1.6f) * std::sin(angle);
            const f32 u = gem / 6.0f; // one interface unit: a socket two gems wide
            const Rect socket{x - 6.0f * u, y - 8.0f * u, 12.0f * u, 16.0f * u};
            m_draw.sprite(socket, kComboPoint, withAlpha(ui::kWhite, frameAlpha), {0.0f, 0.0f, 0.375f, 1.0f});
            if (i >= view.comboPoints) {
                continue;
            }
            const f32 since = static_cast<f32>(now - g_combo.gainedMs[i]);
            const f32 lit = std::clamp(since / 400.0f, 0.0f, 1.0f);
            m_draw.sprite({socket.x + 2.0f * u, socket.y, 8.0f * u, 16.0f * u}, kComboPoint,
                          withAlpha(ui::kWhite, lit * frameAlpha), {0.375f, 0.0f, 0.5625f, 1.0f});
            const f32 shine = since < 400.0f ? 0.0f
                              : since < 700.0f ? (since - 400.0f) / 300.0f
                                               : std::max(0.0f, 1.0f - (since - 700.0f) / 400.0f);
            if (shine > 0.0f) {
                m_draw.spriteAdditive({socket.x, socket.y - 4.0f * u, 14.0f * u, 16.0f * u}, kComboPoint,
                                      withAlpha(ui::kWhite, shine * frameAlpha), {0.5625f, 0.0f, 1.0f, 1.0f});
            }
        }
    } else {
        g_combo.last = 0;
    }
    // The class bar (§6.7): a row going left from its place (square), or one button with a fan (wide).
    const u32 classPlace = touchIndex(TouchPlace::ClassBar);
    if (view.classBarCount > 0 && g.places[classPlace].size > 0.0f) {
        const f32 r = at.radius(classPlace);
        const bool row = frame.width < frame.height * 1.6f;
        if (row) {
            for (u32 i = 0; i < view.classBarCount; ++i) {
                const f32 step = 2.0f * r + 1.5f * at.scale;
                const f32 cx = at.x(classPlace) + (view.mirrored ? step : -step) * static_cast<f32>(i);
                const f32 cy = at.y(classPlace);
                const ui::WidgetState state = m_ui.widgetCircle(ui::widgetId("touch-class", i), cx, cy, r, margin);
                const TouchButtonView& b = view.classBar[i];
                touchButton(cx, cy, r, b, state.pressed, alpha);
                TouchPress& press = m_touchPress[game::kTouchPlaces + i];
                const ui::Touch* t = state.touch;
                if (!t || view.dimmed) {
                    press = {};
                    continue;
                }
                if (state.pressed && b.group && m_fanPlace != kTouchClassBarFan + static_cast<i32>(i) && !press.held &&
                    ((!t->moved && m_ui.nowMs() - t->downMs >= kFanDelayMs) || t->moved)) {
                    m_output.touch.fanOpen = kTouchClassBarFan + static_cast<i32>(i);
                    m_fanPlace = kTouchClassBarFan + static_cast<i32>(i);
                    m_fanHover = -1;
                    m_fanX = cx;
                    m_fanY = cy;
                    press.held = true;
                } else if (!state.pressed) {
                    if (m_fanPlace == kTouchClassBarFan + static_cast<i32>(i)) {
                        if (m_fanHover >= 0 && !t->cancelled) {
                            m_output.touch.fanPlace = m_fanPlace;
                            m_output.touch.fanChosen = m_fanHover;
                        }
                        m_fanPlace = -1;
                    } else if (state.clicked && !press.held) {
                        m_output.touch.classBar = static_cast<i32>(i);
                    }
                    press = {};
                }
            }
        } else {
            // One button showing the form in use (or the first), its fan the bar's buttons.
            const f32 cx = at.x(classPlace), cy = at.y(classPlace);
            TouchButtonView b = view.classBar[0];
            for (u32 i = 0; i < view.classBarCount; ++i) {
                if (view.classBar[i].slot.checked) {
                    b = view.classBar[i];
                }
            }
            b.group = true;
            b.slot.checked = false;
            const ui::WidgetState state = m_ui.widgetCircle(ui::widgetId("touch", classPlace), cx, cy, r, margin);
            touchButton(cx, cy, r, b, state.pressed, alpha);
            TouchPress& press = m_touchPress[classPlace];
            const ui::Touch* t = state.touch;
            if (!t || view.dimmed) {
                press = {};
            } else if (state.pressed) {
                if (m_fanPlace != static_cast<i32>(classPlace) && !press.held) {
                    m_fanPlace = static_cast<i32>(classPlace); // opens at once: its only use is choosing
                    m_fanHover = -1;
                    m_fanX = cx;
                    m_fanY = cy;
                    press.held = true;
                }
            } else {
                if (m_fanPlace == static_cast<i32>(classPlace) && m_fanHover >= 0 && !t->cancelled) {
                    m_output.touch.classBar = m_fanHover;
                }
                m_fanPlace = -1;
                press = {};
            }
        }
    }
    touchFan(frame, at);
    // Phase 20 (assists, §4): a button dragged to aim: the ground aim's pad round it with the spell under the finger
    // (the circle on the ground goes with it), or the spell on its way to a unit frame (the frame lights:
    // app/TouchAssists); its rim red while the finger is back on it (lifted there: nothing).
    const TouchBarOutput& drag = m_output.touch;
    if (drag.dragPlace >= 0 && !drag.dragDropped && static_cast<u32>(drag.dragPlace) < game::kTouchPlaces &&
        view.places[static_cast<u32>(drag.dragPlace)].aim) {
        const u32 p = static_cast<u32>(drag.dragPlace);
        const TouchButtonView& b = view.places[p];
        const f32 cx = at.x(p), cy = at.y(p), r = at.radius(p);
        const std::optional<u32> icon = b.slot.iconHash != 0 ? m_resources->icon(b.slot.iconHash) : std::nullopt;
        if (drag.dragOnButton) { // retouches (ui models): the client's red button art over the icon, not a red rim
            roundTexture(m_draw, *m_resources, kQuickslotRed, cx, cy, r, withAlpha(ui::kWhite, 0.9f), false);
        }
        if (b.aimGround) {
            const f32 pad = std::max(kAimPadMm * mm, (r + margin) * 2.0f);
            m_draw.ring(cx, cy, pad, 0.0f, rgba(0, 0, 0, 70));
            if (!drag.dragOnButton) {
                const f32 dx = drag.dragX - cx, dy = drag.dragY - cy, d = std::hypot(dx, dy);
                const f32 k = d > pad ? pad / d : 1.0f;
                const f32 kx = cx + dx * k, ky = cy + dy * k, kr = r * 0.6f;
                if (icon) {
                    m_draw.iconCircle({kx - kr, ky - kr, 2.0f * kr, 2.0f * kr}, *icon, ui::kWhite);
                }
            }
        } else if (!drag.dragOnButton) {
            // A dotted way from the button; the spell a finger's width over the finger (it would hide it).
            const f32 ir = r * 0.7f;
            const f32 ix = drag.dragX, iy = drag.dragY - ir * 1.8f;
            for (u32 i = 1; i < 6; ++i) {
                const f32 f = static_cast<f32>(i) / 6.0f;
                m_draw.ring(cx + (ix - cx) * f, cy + (iy - cy) * f, std::max(m_ui.px(2.0f), r * 0.08f), 0.0f,
                            withAlpha(kRingGold, 0.8f));
            }
            if (icon) {
                m_draw.iconCircle({ix - ir, iy - ir, 2.0f * ir, 2.0f * ir}, *icon, withAlpha(ui::kWhite, 0.9f));
            }
        }
    }
    // Phase 20 (assists, §8 "Objetivo automático"): a button pressed without a hostile target: the unit the Target
    // button would choose.
    if (view.wantsEnemy && (drag.pressed >= 0 || drag.fanChosen >= 0 || drag.classBar >= 0)) {
        m_output.touch.enemy = nextEnemy(frame);
    }
}

} // namespace vd
