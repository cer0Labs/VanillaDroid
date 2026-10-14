#include "ui/Context.h"

#include <algorithm>
#include <cmath>

namespace vd::ui {
namespace {

constexpr f32 kTapSlopDp = 10.0f;

} // namespace

Context::Context() {
    m_hits.reserve(512);
    m_lastHits.reserve(512);
}

void Context::touchEvent(TouchPhase phase, i32 id, f32 x, f32 y) {
    if (m_eventCount < m_events.size()) {
        m_events[m_eventCount++] = {phase, id, x, y};
    } else if (phase != TouchPhase::Move) {
        m_events[m_events.size() - 1] = {phase, id, x, y}; // never lose a down/up to a flood of moves
    }
}

Touch* Context::find(i32 id) {
    for (u32 i = 0; i < m_touchCount; ++i) {
        if (m_touches[i].id == id) {
            return &m_touches[i];
        }
    }
    return nullptr;
}

WidgetId Context::hitTest(f32 x, f32 y) const {
    for (usize i = m_lastHits.size(); i-- > 0;) {
        const HitRect& h = m_lastHits[i];
        const bool inside = h.radius > 0.0f
                                ? (x - h.cx) * (x - h.cx) + (y - h.cy) * (y - h.cy) <= h.radius * h.radius
                                : h.rect.contains(x, y);
        if (inside) {
            return h.id;
        }
    }
    // Phase 20: on no widget, but near round ones: the nearest whose margin reaches.
    WidgetId best = 0;
    f32 bestGap = 0.0f;
    for (const HitRect& h : m_lastHits) {
        if (h.radius <= 0.0f) {
            continue;
        }
        const f32 gap = std::hypot(x - h.cx, y - h.cy) - h.radius;
        if (gap <= h.margin && (best == 0 || gap < bestGap)) {
            best = h.id;
            bestGap = gap;
        }
    }
    return best;
}

void Context::begin(f32 width, f32 height, f32 dp, const Rect& safeArea, u64 nowMs) {
    m_width = std::max(1.0f, width);
    m_height = std::max(1.0f, height);
    m_dp = std::max(0.1f, dp);
    m_safe = safeArea;
    m_now = nowMs;
    // Fingers released last frame are gone; the rest start the frame without motion.
    u32 kept = 0;
    for (u32 i = 0; i < m_touchCount; ++i) {
        if (!m_touches[i].released) {
            Touch t = m_touches[i];
            t.began = false;
            t.dx = t.dy = 0.0f;
            m_touches[kept++] = t;
        }
    }
    m_touchCount = kept;

    const f32 slop = kTapSlopDp * m_dp;
    for (u32 e = 0; e < m_eventCount; ++e) {
        const Event& event = m_events[e];
        Touch* t = find(event.id);
        if (event.phase == TouchPhase::Down) {
            if (t && !t->released) {
                continue; // duplicate down
            }
            if (!t) {
                if (m_touchCount == m_touches.size()) {
                    continue;
                }
                t = &m_touches[m_touchCount++];
            }
            *t = Touch{};
            t->id = event.id;
            t->x = t->startX = event.x;
            t->y = t->startY = event.y;
            t->downMs = nowMs;
            t->owner = hitTest(event.x, event.y);
            t->began = true;
            continue;
        }
        if (!t || t->released) {
            continue;
        }
        t->dx += event.x - t->x;
        t->dy += event.y - t->y;
        t->x = event.x;
        t->y = event.y;
        if (std::hypot(t->x - t->startX, t->y - t->startY) > slop) {
            t->moved = true;
        }
        if (event.phase == TouchPhase::Up || event.phase == TouchPhase::Cancel) {
            t->released = true;
            t->cancelled = event.phase == TouchPhase::Cancel;
        }
    }
    m_eventCount = 0;
    m_hits.clear();
}

void Context::end() {
    std::swap(m_hits, m_lastHits);
}

WidgetState Context::widget(WidgetId id, const Rect& r) {
    m_hits.push_back({id, r});
    return widgetState(id, r);
}

WidgetState Context::widgetCircle(WidgetId id, f32 cx, f32 cy, f32 radius, f32 margin) {
    const Rect bounds{cx - radius, cy - radius, 2.0f * radius, 2.0f * radius};
    m_hits.push_back({id, bounds, cx, cy, radius, std::max(0.0f, margin)});
    return widgetState(id, bounds);
}

WidgetState Context::widgetState(WidgetId id, const Rect& r) {
    WidgetState state;
    for (u32 i = 0; i < m_touchCount; ++i) {
        Touch& t = m_touches[i];
        if (t.owner != id) {
            continue;
        }
        state.touch = &t;
        if (!t.released) {
            state.pressed = true;
            if (!t.moved && !t.longPressed && m_now - t.downMs >= kLongPressMs) {
                t.longPressed = true;
                state.longPress = true;
            }
        } else {
            state.released = true;
            state.clicked = !t.cancelled && !t.longPressed && (!t.moved || r.contains(t.x, t.y));
        }
        break;
    }
    return state;
}

bool Context::anyTouchOn(WidgetId id) const {
    for (u32 i = 0; i < m_touchCount; ++i) {
        if (m_touches[i].owner == id && m_touches[i].began) {
            return true;
        }
    }
    return false;
}

} // namespace vd::ui
