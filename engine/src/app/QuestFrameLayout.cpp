#include "app/QuestFrameLayout.h"

#include <algorithm>

namespace vd {

QuestFrameLayout questFrameLayout(const ui::Rect& place, f32 k, bool center) {
    QuestFrameLayout l;
    if (place.w <= 0.0f || place.h <= 0.0f || k <= 0.0f) {
        return l;
    }
    k = std::min({k, place.w / kQuestFrameWidth, place.h / kQuestFrameHeight});
    l.k = k;
    const f32 w = kQuestFrameWidth * k, h = kQuestFrameHeight * k;
    l.frame = center ? ui::Rect{place.centerX() - w * 0.5f, place.centerY() - h * 0.5f, w, h}
                     : ui::Rect{place.x, place.y, w, h};
    l.art = {l.frame.x - kQuestFrameLeft * k, l.frame.y, 384.0f * k, 512.0f * k};
    // The picture's units (from its top left, y down) in logical pixels.
    const auto at = [&](f32 x, f32 y, f32 rw, f32 rh) {
        return ui::Rect{l.art.x + x * k, l.art.y + y * k, rw * k, rh * k};
    };
    l.portrait = at(7.0f, 6.0f, 60.0f, 60.0f);
    l.title = at(192.0f - 117.5f, 23.0f + 7.0f - 10.0f, 235.0f, 20.0f);
    l.close = at(384.0f - 42.0f - 16.0f, 31.0f - 16.0f, 32.0f, 32.0f);
    l.scroll = at(23.0f, 81.0f, 300.0f, 334.0f);
    l.scrollBar = at(23.0f + 300.0f + 6.0f, 81.0f, 16.0f, 334.0f);
    l.accept = at(23.0f, 512.0f - 72.0f - 22.0f, 77.0f, 22.0f);
    l.leftButton = at(22.0f, 512.0f - 72.0f - 22.0f, 120.0f, 22.0f);
    l.rightButton = at(384.0f - 39.0f - 78.0f, 512.0f - 73.0f - 22.0f, 78.0f, 22.0f);
    l.patch = at(22.0f, 512.0f - 68.0f - 64.0f, 128.0f, 64.0f);
    return l;
}

} // namespace vd
