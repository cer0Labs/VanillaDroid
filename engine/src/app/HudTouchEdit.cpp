#include "app/HudTouch.h"

#include "app/HudShared.h"
#include "app/TouchRoundIcon.h" // retouches (ui models)
#include "app/UiResources.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <cmath>

namespace vd {

using namespace hud;
using game::touchIndex;
using game::TouchPlace;

namespace {

// Retouches (ui models): ActionButtonTemplate's empty button (UI-Quickslot: the 36-unit button in the middle of its 64
// texels) and its checked texture, instead of an outline round every place.
constexpr u64 kQuickslotEmpty = assetPathHash("Interface\\Buttons\\UI-Quickslot.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr f32 kQuickslotCrop = (64.0f - 36.0f) * 0.5f / 64.0f;
constexpr f32 kInheritedAlpha = 0.45f;
constexpr f32 kHiddenAlpha = 0.3f;
constexpr f32 kPanelGapMm = 1.5f; // places keep this far from the edit panel

// The fingers of a pinch (their ids): neither moves nor picks a place when it is lifted. One HUD per process.
struct PinchFingers {
    i32 a = -1;
    i32 b = -1;
    bool has(i32 id) const { return id >= 0 && (id == a || id == b); }
    void drop(i32 id) {
        a = a == id ? -1 : a;
        b = b == id ? -1 : b;
    }
};
PinchFingers g_pinch;

// A thin bar from (x0, y0) to (x1, y1), `w` wide: two triangles (the hidden slot's cross).
void bar(ui::DrawList& draw, f32 x0, f32 y0, f32 x1, f32 y1, f32 w, u32 color) {
    const f32 dx = x1 - x0, dy = y1 - y0;
    const f32 len = std::max(1e-3f, std::hypot(dx, dy));
    const f32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    draw.triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color);
    draw.triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, color);
}

} // namespace

// Phase 20 (edit, docs/MOBILE_CONTROLS.md §9): the touch cluster's edit mode. The world is dimmed and taken (no finger
// reaches the camera, the stick or the frames under it); every place is drawn with its outline, also the hidden fixed
// ones and the empty slots; the slots of the layer edited that show the base's action are translucent with a chain
// link, the ones hidden on purpose crossed out. A tap picks a place (TouchBarOutput::editTapped); in the layout mode
// a drag moves it (editMoved: mm relative to Main's center; dragging Main moves the whole cluster), in the actions mode
// a drag onto another slot swaps their actions (editDropOn); a long press asks for its tooltip (held); two fingers
// pinch (editPinch: the selected place's size, or the cluster's scale). Where places may go (editArea*): the safe area
// less the panel. app/TouchEditScreens applies it.
void Hud::touchEdit(const HudFrame& frame, const TouchFrame& at) {
    VD_PROFILE_ZONE("Hud::touchEdit");
    const TouchBarView& view = *at.view;
    TouchBarOutput& out = m_output.touch;
    const Rect& safe = frame.safeArea;
    const Rect screen{0.0f, 0.0f, frame.width, frame.height};
    const ui::WidgetId dimId = ui::widgetId("touch-edit-dim");
    m_draw.solid(screen, rgba(0, 0, 0, 120));
    m_ui.blocker(dimId, screen);

    // Where places may go: the safe area, less the panel's side.
    f32 left = safe.x, right = safe.right();
    const Rect& panel = view.editPanel;
    if (panel.w > 0.0f) {
        const f32 gap = kPanelGapMm * at.scale;
        if (panel.centerX() < at.mainX) {
            left = std::max(left, panel.right() + gap);
        } else {
            right = std::min(right, panel.x - gap);
        }
    }
    const f32 mmA = at.toMmX(left), mmB = at.toMmX(right);
    out.editAreaLeft = std::min(mmA, mmB);
    out.editAreaRight = std::max(mmA, mmB);
    out.editAreaBottom = at.toMmY(safe.bottom());
    out.editAreaTop = at.toMmY(safe.y);

    // Two fingers on the cluster or the dimmed world: a pinch.
    const ui::Touch* pinch[2] = {nullptr, nullptr};
    for (const ui::Touch& t : m_ui.touches()) {
        if (t.released || t.cancelled) {
            continue;
        }
        bool ours = t.owner == dimId;
        for (u32 p = 0; p < game::kTouchPlaces && !ours; ++p) {
            ours = t.owner == ui::widgetId("touch-edit", p);
        }
        if (ours && !pinch[0]) {
            pinch[0] = &t;
        } else if (ours && !pinch[1]) {
            pinch[1] = &t;
        }
    }
    if (pinch[0] && pinch[1]) {
        g_pinch.a = pinch[0]->id;
        g_pinch.b = pinch[1]->id;
        const f32 start = std::hypot(pinch[0]->startX - pinch[1]->startX, pinch[0]->startY - pinch[1]->startY);
        const f32 now = std::hypot(pinch[0]->x - pinch[1]->x, pinch[0]->y - pinch[1]->y);
        out.editPinch = start > m_ui.px(8.0f) ? now / start : 0.0f;
    }

    // Main dragged in the layout mode: the whole cluster follows the finger.
    const u32 main = touchIndex(TouchPlace::Main);
    f32 shiftX = 0.0f, shiftY = 0.0f;
    for (const ui::Touch& t : m_ui.touches()) {
        if (view.editLayout && t.owner == ui::widgetId("touch-edit", main) && t.moved && !t.cancelled &&
            !g_pinch.has(t.id)) {
            shiftX = t.x - t.startX;
            shiftY = t.y - t.startY;
        }
    }

    for (u32 p = 0; p < game::kTouchPlaces; ++p) {
        const f32 r = at.radius(p);
        if (r <= 0.0f) {
            continue;
        }
        const f32 cx = at.x(p) + shiftX, cy = at.y(p) + shiftY;
        const ui::WidgetState state = m_ui.widgetCircle(ui::widgetId("touch-edit", p), cx, cy, r, m_ui.px(4.0f));
        const ui::Touch* t = state.touch;
        const bool pinching = t && g_pinch.has(t->id);
        f32 x = cx, y = cy;
        if (t && t->moved && !t->cancelled && !pinching && p != main && (view.editLayout || p < game::kTouchSlots)) {
            // Follows the finger, its circle kept inside the area it may go to.
            x = std::clamp(t->x, left + r, std::max(left + r, right - r));
            y = std::clamp(t->y, safe.y + r, std::max(safe.y + r, safe.bottom() - r));
        }
        const bool slot = p < game::kTouchSlots;
        const bool inherited = slot && (view.editInherited & (1u << p)) != 0;
        const bool hidden = slot && (view.editHidden & (1u << p)) != 0;
        const TouchButtonView& button = view.places[p];
        const f32 alpha = hidden ? kHiddenAlpha : inherited ? kInheritedAlpha : 1.0f;
        // Retouches (ui models): no outlines; an empty slot is the 1.12 empty button (UI-Quickslot, its 36-unit middle)
        // in its circle, the chosen place has the checked texture (CheckButtonHilight).
        const bool selected = view.editSelected == static_cast<i32>(p);
        if (button.shown && button.slot.iconHash != 0) {
            touchButton(x, y, r, button, state.pressed, alpha);
        } else if (const std::optional<u32> empty = m_resources->icon(kQuickslotEmpty)) {
            m_draw.iconCircle({x - r, y - r, 2.0f * r, 2.0f * r}, *empty, withAlpha(ui::kWhite, alpha), kQuickslotCrop);
        }
        if (selected) {
            roundTexture(m_draw, *m_resources, kCheckHighlight, x, y, r, ui::kWhite, true);
        }
        if (inherited) {
            // A chain link at the bottom left: the base's action, not this layer's own.
            const f32 link = r * 0.2f;
            const f32 lx = x - r * 0.62f, ly = y + r * 0.62f;
            m_draw.ring(lx - link * 0.55f, ly + link * 0.55f, link, 0.55f, rgba(230, 230, 230));
            m_draw.ring(lx + link * 0.55f, ly - link * 0.55f, link, 0.55f, rgba(230, 230, 230));
        }
        if (hidden) {
            const f32 d = r * 0.55f;
            bar(m_draw, x - d, y - d, x + d, y + d, std::max(2.0f, r * 0.12f), rgba(220, 60, 60, 220));
            bar(m_draw, x - d, y + d, x + d, y - d, std::max(2.0f, r * 0.12f), rgba(220, 60, 60, 220));
        }
        if (!t) {
            continue;
        }
        if (state.longPress && !t->moved && !pinching) {
            out.held = static_cast<i32>(p);
            out.heldRect = {cx - r, cy - r, 2.0f * r, 2.0f * r};
        }
        if (!state.released || t->cancelled) {
            continue;
        }
        if (pinching) {
            g_pinch.drop(t->id);
            continue;
        }
        if (!t->moved) {
            if (!t->longPressed) {
                out.editTapped = static_cast<i32>(p);
            }
        } else if (view.editLayout) {
            out.editMoved = static_cast<i32>(p);
            if (p == main) {
                out.editX = at.toMmX(at.x(p) + (t->x - t->startX));
                out.editY = at.toMmY(at.y(p) + (t->y - t->startY));
            } else {
                out.editX = at.toMmX(x);
                out.editY = at.toMmY(y);
            }
        } else if (slot) {
            // Dropped on another content slot: the nearest center under the finger.
            for (u32 q = 0; q < game::kTouchSlots; ++q) {
                const f32 rq = at.radius(q);
                if (q != p && rq > 0.0f && std::hypot(t->x - at.x(q), t->y - at.y(q)) <= rq * 1.15f) {
                    out.editTapped = static_cast<i32>(p);
                    out.editDropOn = static_cast<i32>(q);
                    break;
                }
            }
        }
    }
    // A finger of the pinch lifted on the dimmed world.
    for (const ui::Touch& t : m_ui.touches()) {
        if ((t.released || t.cancelled) && g_pinch.has(t.id)) {
            g_pinch.drop(t.id);
        }
    }
}

} // namespace vd
