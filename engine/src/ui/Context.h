#pragma once

#include "core/Types.h"
#include "platform/Input.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::ui {

using WidgetId = u64;

// A widget's identity across frames: its name (and an index for repeated widgets).
constexpr WidgetId widgetId(std::string_view name, u64 index = 0) {
    u64 hash = 1469598103934665603ull ^ (index * 0x9E3779B97F4A7C15ull);
    for (const char c : name) {
        hash = (hash ^ static_cast<u8>(c)) * 1099511628211ull;
    }
    return hash | 1; // never 0 (0 = the world)
}

// One finger (or the mouse) from down to up. Positions in logical pixels.
struct Touch {
    i32 id = -1;
    f32 x = 0.0f, y = 0.0f;           // now
    f32 startX = 0.0f, startY = 0.0f; // where it went down
    f32 dx = 0.0f, dy = 0.0f;         // moved since the last frame
    u64 downMs = 0;
    WidgetId owner = 0; // the widget it went down on (0: the world)
    bool began = false;    // went down this frame
    bool released = false; // went up this frame (last frame it is listed)
    bool cancelled = false;
    bool moved = false;       // beyond the tap slop at some point
    bool longPressed = false; // held still long enough (reported once)
};

struct WidgetState {
    bool pressed = false;   // a finger that went down on it is still down
    bool clicked = false;   // released on it without dragging away (a tap)
    bool longPress = false; // held still for kLongPressMs (this frame only)
    bool released = false;  // its finger went up this frame (anywhere)
    const Touch* touch = nullptr; // the finger on it, if any
};

// Immediate-mode input for the touch UI. Widgets are declared every frame in drawing order (later = on top); a finger
// belongs to the topmost widget under it in the previous frame, for its whole life. Fingers on no widget belong to
// the world (camera, movement, selecting).
class Context {
public:
    static constexpr u64 kLongPressMs = 450;
    static constexpr u32 kMaxTouches = 10;

    Context();

    void touchEvent(TouchPhase phase, i32 id, f32 x, f32 y);

    // Screen in logical pixels; dp: logical pixels per density-independent pixel (density x the player's UI scale);
    // insets: safe area (notch, rounded corners) in logical pixels.
    void begin(f32 width, f32 height, f32 dp, const Rect& safeArea, u64 nowMs);
    void end();

    WidgetState widget(WidgetId id, const Rect& r);
    // Phase 20 (the touch cluster): a round widget. A finger within `radius` of (cx, cy) is its; one that lands on no
    // widget at all but within `radius + margin` of round ones goes to the nearest of them (a finger between two
    // buttons: most missed taps on a phone).
    WidgetState widgetCircle(WidgetId id, f32 cx, f32 cy, f32 radius, f32 margin);
    // Swallows every finger that lands on `r` without a widget above it (modal dialogs, panels).
    void blocker(WidgetId id, const Rect& r) { widget(id, r); }
    // A finger went down on `id` this frame (for focus changes).
    bool anyTouchOn(WidgetId id) const;

    std::span<const Touch> touches() const { return {m_touches.data(), m_touchCount}; }
    f32 width() const { return m_width; }
    f32 height() const { return m_height; }
    f32 dp() const { return m_dp; }
    f32 px(f32 dps) const { return dps * m_dp; }
    const Rect& safeArea() const { return m_safe; }
    u64 nowMs() const { return m_now; }

private:
    struct Event {
        TouchPhase phase;
        i32 id;
        f32 x, y;
    };
    struct HitRect {
        WidgetId id;
        Rect rect;
        f32 cx = 0.0f, cy = 0.0f, radius = 0.0f, margin = 0.0f; // phase 20: a round widget when radius > 0
    };
    WidgetState widgetState(WidgetId id, const Rect& r);

    Touch* find(i32 id);
    WidgetId hitTest(f32 x, f32 y) const;

    std::array<Touch, kMaxTouches> m_touches{};
    u32 m_touchCount = 0;
    std::array<Event, 64> m_events{};
    u32 m_eventCount = 0;
    std::vector<HitRect> m_hits;     // this frame's widgets, in order
    std::vector<HitRect> m_lastHits; // the previous frame's: what new fingers are tested against
    f32 m_width = 1.0f, m_height = 1.0f, m_dp = 1.0f;
    Rect m_safe{};
    u64 m_now = 0;
};

} // namespace vd::ui
