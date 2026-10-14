#include "app/Hud.h"

#include "app/FlyCamera.h"
#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "net/UpdateFields.gen.h"
#include "pack/TerrainFormat.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd {

using namespace hud;

namespace {

// ActionButtonTemplate's art (ActionButton.lua): the empty button (UI-Quickslot), the auto attack / auto-repeat flash,
// the checked highlight.
constexpr u64 kQuickslotEmpty = assetPathHash("Interface\\Buttons\\UI-Quickslot.blp");
constexpr u64 kQuickslotRed = assetPathHash("Interface\\Buttons\\UI-QuickslotRed.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kFlashPeriodMs = 400; // ATTACK_BUTTON_FLASH_TIME: the flash shows and hides every 0.4 s
// ActionButton_UpdateUsable's tints: short of power (icon and frame), unusable (icon only).
constexpr u32 kNoManaTint = rgba(128, 128, 255);
constexpr u32 kUnusableTint = rgba(102, 102, 102);
constexpr u32 kOutOfRangeTint = rgba(204, 26, 26); // ours: the range indicator's red on the whole icon (no hotkeys)

// Ring radius (dp) and angle (degrees, 180 = left of the context button, 90 = above) of a place in the cluster.
struct Place {
    f32 radius, angle;
};
// The twelve buttons of the main bar on four arcs around the context button (the thumb's rest): 1-3 on the inner
// ring, 4-6 on the second (as phase 7 placed its six), 7-12 on two outer arcs that stay below the minimap on the
// cover screen (their highest buttons sit left of it). Square buttons on a ring keep a button's width apart on one
// axis, hence the angles.
constexpr Place kSlotPlaces[12] = {{108.0f, 180.0f}, {108.0f, 135.0f}, {108.0f, 90.0f}, {176.0f, 157.5f},
                                   {176.0f, 112.5f}, {176.0f, 90.0f},  {250.0f, 180.0f}, {250.0f, 160.0f},
                                   {250.0f, 140.0f}, {320.0f, 180.0f}, {320.0f, 163.0f}, {320.0f, 146.0f}};
constexpr Place kJumpPlace{176.0f, 180.0f};
constexpr Place kNextTargetPlace{190.0f, 135.0f};
constexpr Place kPagePlace{390.0f, 180.0f};
constexpr f32 kClusterLeftDp = 410.0f; // the page button's left edge
constexpr f32 kClusterTopDp = 206.0f;  // the highest button's top (button 12)
constexpr f32 kDegrees = 3.14159265f / 180.0f;

} // namespace

ui::WidgetState Hud::slotButton(ui::WidgetId id, const Rect& r, const HudActionSlot& slot) {
    const ui::WidgetState state = m_ui.widget(id, r);
    const bool checked = slot.checked || slot.active;
    const bool flashing = slot.flashing || slot.active;
    const bool unusable = !slot.usable && !slot.noMana;
    // UI-Quickslot2 (UI-Quickslot when empty): 64x64 art around a 36-unit button, its border reaching past the icon;
    // blue with the icon when only the power is short.
    const f32 pad = r.w * (64.0f - 36.0f) / 36.0f * 0.5f;
    const bool blue = slot.noMana && slot.inRange && slot.iconHash != 0;
    m_draw.sprite({r.x - pad, r.y - pad, r.w + 2.0f * pad, r.h + 2.0f * pad},
                  slot.iconHash != 0 ? kQuickslot : kQuickslotEmpty, blue ? kNoManaTint : ui::kWhite);
    if (slot.iconHash != 0) {
        if (const std::optional<u32> icon = m_resources->icon(slot.iconHash)) {
            u32 tint = ui::kWhite;
            if (unusable) {
                tint = kUnusableTint;
            } else if (!slot.inRange) {
                tint = kOutOfRangeTint;
            } else if (slot.noMana) {
                tint = kNoManaTint;
            } else if (state.pressed) {
                tint = rgba(190, 190, 190);
            }
            m_draw.icon(r, *icon, tint);
        }
    }
    // The auto attack / auto-repeat flash (ActionButton_UpdateFlash): UI-QuickslotRed over the icon, on and off.
    if (flashing && (m_ui.nowMs() / kFlashPeriodMs) % 2 == 0) {
        m_draw.sprite(r, kQuickslotRed);
    }
    // Retouches (ui models): the Cooldown model (ActionButtonTemplate: 36 x 36, one unit lower; no seconds in 1.12).
    {
        const f32 unit = r.w / 36.0f;
        const f32 share = slot.cooldownTotal > 0.0f ? std::clamp(slot.cooldownLeft / slot.cooldownTotal, 0.0f, 1.0f)
                                                    : 0.0f;
        m_draw.models().cooldown(m_draw, id, {r.x, r.y + unit, r.w, r.h}, unit, share, slot.cooldownLeft,
                                 m_ui.nowMs());
    }
    // An equipped item: the green border (UI-ActionButton-Border, 62 units around 36, vertex color 0, 1, 0, 0.35).
    if (slot.equipped) {
        const f32 grow = r.w * (62.0f - 36.0f) / 36.0f * 0.5f;
        m_draw.spriteAdditive({r.x - grow, r.y - grow, r.w + 2.0f * grow, r.h + 2.0f * grow}, kActionBorder,
                              rgba(0, 255, 0, 89));
    }
    // The current action (ActionButton_UpdateState): the checked highlight.
    if (checked) {
        m_draw.spriteAdditive(r, kCheckHighlight);
    }
    if (slot.highlight) { // a place for the spell being put on the bar
        const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(m_ui.nowMs() % 800) * 0.00785f);
        const f32 grow = r.w * 0.3f;
        m_draw.spriteAdditive({r.x - grow, r.y - grow, r.w + 2.0f * grow, r.h + 2.0f * grow}, kActionBorder,
                              withAlpha(rgba(120, 255, 120), 0.4f + 0.6f * pulse));
    }
    if (slot.count > 0) {
        char count[8];
        std::snprintf(count, sizeof(count), "%u", slot.count);
        ui::TextStyle style;
        style.size = r.h * 0.3f;
        style.color = ui::kWhite;
        style.outline = std::max(1.0f, r.h * 0.025f);
        style.align = ui::Align::Right;
        m_draw.text(r.right() - r.w * 0.06f, r.bottom() - r.h * 0.06f, count, style);
    }
    if (!slot.label.empty()) {
        // Phase 15 (options): the macro's name ($parentName: GameFontHighlightSmallOutline, 2 units over the bottom).
        ui::TextStyle style;
        style.size = r.h * 0.27f;
        style.color = ui::kWhite;
        style.outline = std::max(1.0f, r.h * 0.025f);
        style.align = ui::Align::Center;
        m_draw.pushClip(r);
        m_draw.text(r.centerX(), r.bottom() - r.h * 0.08f, slot.label, style);
        m_draw.popClip();
    }
    if (state.pressed) {
        m_draw.sprite(r, kQuickslotDown);
        m_draw.spriteAdditive(r, kButtonHighlight, rgba(255, 255, 255, 140));
    }
    return state;
}

void Hud::actionBar(const HudFrame& frame, PlayerController::TouchInput& input) {
    VD_PROFILE_ZONE("Hud::actionBar");
    const Rect& safe = frame.safeArea;
    m_bottomRowRect = {};
    // The big context button in the corner; the twelve buttons of the main bar on four arcs around it, the jump at
    // the second ring's start, the next-enemy button in its gap and the page button at the end of the bottom row.
    // Short screens (the cover display held sideways) shrink the whole cluster so it leaves room for the minimap
    // above it; narrow ones so it keeps to the right half (the character stands in the middle).
    const bool wide = frame.width >= frame.height * 1.6f;
    const f32 k = std::clamp(std::min(safe.h / m_ui.px(430.0f), safe.w * 0.5f / m_ui.px(kClusterLeftDp + 56.0f)),
                             0.72f, 1.0f);
    const f32 big = m_ui.px(76.0f) * k;
    const f32 margin = m_ui.px(18.0f) * k;
    const f32 cx = safe.right() - margin - big * 0.5f;
    const f32 cy = safe.bottom() - margin - big * 0.5f - m_ui.px(4.0f) * k;
    const Rect context{cx - big * 0.5f, cy - big * 0.5f, big, big};
    HudActionSlot contextSlot;
    switch (frame.context) {
    case HudContextAction::Attack: contextSlot.iconHash = kIconAttack; break;
    case HudContextAction::StopAttack:
        contextSlot.iconHash = kIconAttack;
        contextSlot.active = true;
        break;
    case HudContextAction::Talk: contextSlot.iconHash = kIconTalk; break;
    case HudContextAction::Loot: contextSlot.iconHash = kIconLoot; break;
    case HudContextAction::Use: contextSlot.iconHash = kIconUse; break;
    case HudContextAction::Resurrect: // phase 20 (smart): the spell's icon
    case HudContextAction::Skin: contextSlot.iconHash = frame.contextActionIcon; break;
    default:
        contextSlot.iconHash = kIconAttack;
        contextSlot.usable = false;
        break;
    }
    contextSlot.inRange = frame.contextInRange;
    const ui::WidgetState contextState = slotButton(ui::widgetId("context"), context, contextSlot);
    contextLabel(frame, context); // parity: its subject's name and cursor icon (HudWorld.cpp)
    if (contextState.longPress) {
        m_output.contextHold = true;
        m_contextHeld = true;
    }
    if (contextState.clicked && !m_contextHeld && frame.context != HudContextAction::None) {
        m_output.context = true;
    }
    if (contextState.released) {
        m_contextHeld = false;
    }

    const f32 slotSize = m_ui.px(54.0f) * k;
    m_actionBarRect = {cx - m_ui.px(kClusterLeftDp) * k, cy - m_ui.px(kClusterTopDp) * k,
                       m_ui.px(kClusterLeftDp) * k + big * 0.5f, m_ui.px(kClusterTopDp) * k + big * 0.5f};
    const auto placeRect = [&](const Place& place, f32 size) {
        const f32 radius = m_ui.px(place.radius) * k;
        const f32 sx = cx + radius * std::cos(place.angle * kDegrees);
        const f32 sy = cy - radius * std::sin(place.angle * kDegrees);
        return Rect{sx - size * 0.5f, sy - size * 0.5f, size, size};
    };
    const u32 slots = static_cast<u32>(std::min<usize>(frame.actions.size(), m_actionHeld.size()));
    for (u32 i = 0; i < m_actionHeld.size(); ++i) {
        const HudActionSlot slot = i < slots ? frame.actions[i] : HudActionSlot{};
        const Rect r = placeRect(kSlotPlaces[i], slotSize);
        const ui::WidgetState state = slotButton(ui::widgetId("action", i), r, slot);
        // A long press shows the button's tooltip (or edits an empty one; its release is not a tap); empty slots
        // only take taps while placing.
        if (state.longPress) {
            m_output.actionHold = static_cast<i32>(i);
            m_output.holdRect = r;
            m_actionHeld[i] = true;
        }
        if (state.clicked && !m_actionHeld[i] && (slot.iconHash != 0 || frame.actionPlacing)) {
            m_output.action = static_cast<i32>(i);
        }
        if (state.released) {
            m_actionHeld[i] = false;
        }
    }
    // The page button at the bottom row's end (ActionBar_PageUp and the page number): a tap turns the page, a long
    // press goes home.
    const f32 pageSize = m_ui.px(40.0f) * k;
    const Rect pageRect = placeRect(kPagePlace, pageSize);
    if (frame.actionPages > 1) {
        const Rect& r = pageRect;
        const ui::WidgetState state = m_ui.widget(ui::widgetId("action-page"), r);
        m_draw.sprite(r.inset(-pageSize * 0.12f), kQuickslot, ui::kWhite);
        m_draw.solid(r, rgba(0, 0, 0, state.pressed ? 200 : 140));
        char page[8];
        std::snprintf(page, sizeof(page), "%u", frame.actionPage + 1);
        ui::TextStyle style;
        style.size = pageSize * 0.5f;
        style.color = ui::kGold;
        style.outline = std::max(1.0f, pageSize * 0.03f);
        style.align = ui::Align::Center;
        m_draw.text(r.centerX(), r.centerY() + style.size * 0.35f, page, style);
        // Small marks of the page count under the number.
        const f32 dot = std::max(2.0f, pageSize * 0.07f);
        const u32 marks = std::min<u32>(frame.actionPages, 8);
        const f32 start = r.centerX() - (static_cast<f32>(marks) * dot * 1.8f - dot * 0.8f) * 0.5f;
        for (u32 i = 0; i < marks; ++i) {
            m_draw.solid({start + static_cast<f32>(i) * dot * 1.8f, r.bottom() - dot * 2.2f, dot, dot},
                         i == frame.actionPage % marks ? ui::kGold : rgba(150, 150, 150));
        }
        if (state.longPress) {
            m_output.pageHome = true;
            m_pageHeld = true;
        }
        if (state.clicked && !m_pageHeld) {
            m_output.pageNext = true;
        }
        if (state.released) {
            m_pageHeld = false;
        }
    }
    // Jump: pressed like a key (on touch, not on release).
    const ui::WidgetState jumpState =
        slotButton(ui::widgetId("jump"), placeRect(kJumpPlace, slotSize), HudActionSlot{kIconJump});
    input.jump = jumpState.touch && jumpState.touch->began;
    // Next enemy, smaller, in the second ring's gap.
    const bool online = frame.session && frame.session->inWorld();
    HudActionSlot next{kIconNextTarget};
    next.usable = online;
    if (slotButton(ui::widgetId("next-target"), placeRect(kNextTargetPlace, m_ui.px(44.0f) * k), next).clicked &&
        online) {
        frame.session->setTarget(nextEnemy(frame));
    }
    const f32 bottomLeft = frame.actionPages > 1 ? pageRect.x : placeRect(kSlotPlaces[9], slotSize).x;
    stanceBar(frame, k, wide, bottomLeft);
    extraBar(frame, k, wide);
    const f32 rowTop = cy - slotSize * 0.5f;
    const f32 rowLeft = m_stanceBarRect.h > 0.0f && m_stanceBarRect.y >= rowTop - 1.0f
                            ? std::min(bottomLeft, m_stanceBarRect.x)
                            : bottomLeft;
    m_bottomRowRect = {rowLeft, rowTop, safe.right() - rowLeft, safe.bottom() - rowTop};
}

void Hud::stanceBar(const HudFrame& frame, f32 k, bool wide, f32 bottomLeft) {
    m_stanceBarRect = {};
    const u32 count = static_cast<u32>(std::min<usize>(frame.stances.size(), m_stanceHeld.size()));
    if (count == 0) {
        return;
    }
    // ShapeshiftBarFrame sits over the main bar's left end, its buttons smaller (30 units for 36). Here: on the cover
    // screen along the bottom, left of the cluster (the minimap leaves no room over it); on the inner screen right
    // over the cluster, near the thumb.
    const f32 size = m_ui.px(42.0f) * k;
    const f32 gap = m_ui.px(6.0f) * k;
    const f32 width = static_cast<f32>(count) * size + static_cast<f32>(count - 1) * gap;
    const f32 cy = m_actionBarRect.bottom() - m_ui.px(76.0f) * k * 0.5f; // the context button's center
    Rect row;
    if (wide) {
        row = {bottomLeft - m_ui.px(10.0f) * k - width, cy - size * 0.5f, width, size};
    } else {
        row = {m_actionBarRect.right() - width, m_actionBarRect.y - m_ui.px(10.0f) * k - size, width, size};
    }
    m_stanceBarRect = row;
    for (u32 i = 0; i < count; ++i) {
        const Rect r{row.x + static_cast<f32>(i) * (size + gap), row.y, size, size};
        const HudActionSlot& slot = frame.stances[i];
        const ui::WidgetState state = slotButton(ui::widgetId("stance", i), r, slot);
        if (state.longPress) {
            m_output.stanceHold = static_cast<i32>(i);
            m_output.holdRect = r;
            m_stanceHeld[i] = true;
        }
        if (state.clicked && !m_stanceHeld[i]) {
            m_output.stance = static_cast<i32>(i);
        }
        if (state.released) {
            m_stanceHeld[i] = false;
        }
    }
}

void Hud::extraBar(const HudFrame& frame, f32 k, bool wide) {
    m_extraBarRect = {};
    m_drawnMultiBars = 0;
    if (!frame.showExtraBar) {
        return; // a window on the right: the bars give way (their buttons are the main bar's pages meanwhile)
    }
    // Phase 15 (options): the four MultiActionBars (MultiActionBar_Update), each as two rows of six of the cluster's
    // smaller buttons. The original lines the bottom left and bottom right bars up over the main bar and stands the
    // right ones at the screen's right edge; the touch layout keeps the right half for the thumb and the left for the
    // stick and the chat, so:
    // - the inner screen stacks them over the cluster (over the stance row), right-aligned, the bottom left one
    //   nearest the thumb, then the bottom right, the right and the right 2, as many as fit under the minimap cluster;
    // - the wide screens (no height over the cluster) put them along the top between the target frame and the
    //   minimap, the bottom left first, as many as fit (their buttons smaller when needed, not under 34 dp); the
    //   player's buffs then hang under them (Hud::playerAuras).
    // A bar that does not fit is not drawn and its page stays one of the main bar's (VIEWABLE_ACTION_BAR_PAGES, as
    // the original does for a bar not shown). The second right bar shows only with the first (MultiBarLeft).
    const std::span<const HudActionSlot> bars[4] = {frame.extraActions, frame.moreBars[0], frame.moreBars[1],
                                                    frame.moreBars[2]};
    const f32 gap = m_ui.px(6.0f) * k;
    const f32 minimapBottom = frame.safeArea.y + u(92.0f + 70.0f + 34.0f) + m_ui.px(14.0f);
    // The top row on wide screens: from the target frame to the minimap cluster (190 units wide).
    const f32 topLeft = m_targetFrameRect.right() + m_ui.px(12.0f);
    const f32 topRight = frame.safeArea.right() - u(190.0f + 10.0f);
    f32 base = m_stanceBarRect.h > 0.0f ? m_stanceBarRect.y : m_actionBarRect.y;
    f32 nextX = topLeft;
    for (u32 b = 0; b < 4; ++b) {
        const u32 count = static_cast<u32>(std::min<usize>(bars[b].size(), 12));
        const bool wanted = (frame.multiBars & (1u << b)) != 0 && (b != 3 || (frame.multiBars & 4u) != 0);
        if (!wanted || count == 0) {
            continue;
        }
        // A bar with nothing on it and no grid shown is there but invisible: it takes no room (nor the buffs').
        bool visible = frame.alwaysShowMultiBars || frame.actionPlacing;
        for (u32 i = 0; i < count && !visible; ++i) {
            visible = bars[b][i].iconHash != 0 || bars[b][i].highlight;
        }
        if (!visible) {
            m_drawnMultiBars |= static_cast<u8>(1u << b);
            continue;
        }
        f32 size = m_ui.px(46.0f) * k;
        Rect block;
        if (wide) {
            size = std::min(size, (u(100.0f) + m_ui.px(30.0f) - gap) * 0.5f); // about the unit frames' height
            size = std::min(size, (topRight - nextX - 5.0f * gap) / 6.0f);   // smaller to fit, down to 34 dp
            if (size < m_ui.px(34.0f)) {
                break;
            }
            const f32 width = 6.0f * size + 5.0f * gap;
            block = {nextX, frame.safeArea.y + m_ui.px(4.0f), width, 2.0f * size + gap};
            if (block.right() > topRight) {
                break;
            }
            nextX = block.right() + m_ui.px(14.0f);
        } else {
            const f32 width = 6.0f * size + 5.0f * gap;
            const f32 height = 2.0f * size + gap;
            block = {m_actionBarRect.right() - width, base - m_ui.px(12.0f) * k - height, width, height};
            if (block.y < minimapBottom) {
                break;
            }
            base = block.y;
        }
        m_drawnMultiBars |= static_cast<u8>(1u << b);
        if (b == 0 || m_extraBarRect.w <= 0.0f) {
            m_extraBarRect = block;
        } else { // the bars' whole place (the cast bar and the panels keep clear of it)
            const f32 x0 = std::min(m_extraBarRect.x, block.x), y0 = std::min(m_extraBarRect.y, block.y);
            m_extraBarRect = {x0, y0, std::max(m_extraBarRect.right(), block.right()) - x0,
                              std::max(m_extraBarRect.bottom(), block.bottom()) - y0};
        }
        for (u32 i = 0; i < count; ++i) {
            const Rect r{block.x + static_cast<f32>(i % 6) * (size + gap), block.y + static_cast<f32>(i / 6) * (size + gap),
                         size, size};
            const HudActionSlot& slot = bars[b][i];
            const ui::WidgetId id = ui::widgetId("extra-action", b * 16 + i);
            ui::WidgetState state;
            if (slot.iconHash == 0 && !slot.highlight) {
                // An empty button of an extra bar: hidden unless ALWAYS_SHOW_MULTIBARS shows its grid, faint
                // (ActionButton_ShowGrid's half alpha); while a spell is placed every grid shows (ACTIONBAR_SHOWGRID).
                if (!frame.alwaysShowMultiBars && !frame.actionPlacing) {
                    m_extraHeld[b][i] = false;
                    continue;
                }
                state = m_ui.widget(id, r);
                const f32 pad = r.w * (64.0f - 36.0f) / 36.0f * 0.5f;
                m_draw.sprite({r.x - pad, r.y - pad, r.w + 2.0f * pad, r.h + 2.0f * pad}, kQuickslotEmpty,
                              withAlpha(ui::kWhite, state.pressed ? 0.8f : 0.5f));
            } else {
                state = slotButton(id, r, slot);
            }
            if (state.longPress) {
                m_output.extraActionHold = static_cast<i32>(i);
                m_output.extraBar = static_cast<u8>(b);
                m_output.holdRect = r;
                m_extraHeld[b][i] = true;
            }
            if (state.clicked && !m_extraHeld[b][i] && (slot.iconHash != 0 || frame.actionPlacing)) {
                m_output.extraAction = static_cast<i32>(i);
                m_output.extraBar = static_cast<u8>(b);
            }
            if (state.released) {
                m_extraHeld[b][i] = false;
            }
        }
    }
}

void Hud::castBar(const HudFrame& frame, const Rect& at) {
    const HudCastBar& cast = frame.cast;
    // Parity pass: over the action cluster's bottom row (and the stance row beside it on the cover screen) when it
    // reaches under the bar (last frame's).
    Rect area = at;
    const Rect& row = m_bottomRowRect;
    if (row.h > 0.0f && area.right() > row.x && area.x < row.right() && area.bottom() + u(12.0f) > row.y) {
        area.y = row.y - u(14.0f) - area.h;
    }
    // Phase 20: the touch cluster reaches further from its corner than the bars' row: the bar keeps beside it (last
    // frame's place), on the stick's side.
    const Rect& cluster = m_actionBarRect;
    if (frame.touch && cluster.w > 0.0f && area.right() > cluster.x && area.x < cluster.right() &&
        area.bottom() > cluster.y) {
        area.x = frame.touch->mirrored
                     ? std::min(frame.safeArea.right() - u(10.0f) - area.w, cluster.right() + u(14.0f))
                     : std::max(frame.safeArea.x + u(10.0f), cluster.x - u(14.0f) - area.w);
    }
    m_castBarRect = area; // phase 18 (p3): where it goes, shown or not (the tutorials' buttons go over it)
    if (!cast.visible) {
        return;
    }
    // The client's CastingBarFrame: a 195x13 bar over its half black background (setAllPoints), its 256x64
    // border 28 units above it, the spark at the fill's edge 2 units up (CastingBarFrame_OnUpdate).
    m_draw.solid(area, rgba(0, 0, 0, 128));
    const f32 progress = std::clamp(cast.progress, 0.0f, 1.0f);
    const u32 color = cast.failed ? rgba(255, 0, 0) : cast.succeeded ? rgba(0, 255, 0)
                      : cast.channel ? rgba(0, 255, 0)
                                     : rgba(255, 178, 0);
    const f32 fill = cast.failed || cast.succeeded ? 1.0f : progress;
    m_draw.sprite({area.x, area.y, area.w * fill, area.h}, kStatusBar, color, {0.0f, 0.0f, fill, 1.0f});
    const Rect border{area.centerX() - u(128.0f), area.y - u(28.0f), u(256.0f), u(64.0f)};
    m_draw.sprite(border, kCastBorder);
    if (!cast.failed && !cast.succeeded) {
        m_draw.spriteAdditive(
            {area.x + area.w * fill - u(16.0f), area.centerY() - u(2.0f) - u(16.0f), u(32.0f), u(32.0f)},
            kCastSpark);
    } else {
        m_draw.spriteAdditive(border, kCastFlash, withAlpha(ui::kWhite, 0.6f));
    }
    // CastingBarText: GameFontHighlight (FontHeight 12) in a 185x16 box whose top is 5 units over the bar's,
    // the line centred in it as WoW.exe lays it (its capitals reach the border's top edge, as in the
    // original).
    ui::TextStyle style = ui::gameFont(u(12.0f) * 1.2f, ui::kWhite);
    style.align = ui::Align::Center;
    m_draw.text(area.centerX(),
                ui::fontStringBaseline(m_draw, style.font, u(12.0f), area.y - u(5.0f) + u(8.0f)),
                cast.failed ? text::get("INTERRUPTED") : cast.name, style);
}

} // namespace vd
