#include "app/LoadingLayout.h"

#include <algorithm>

namespace vd {

LoadingLayout loadingLayout(const ui::Rect& screen, const ui::Rect& safe, f32 minTipSize) {
    LoadingLayout l;
    // Pillarboxed on a wide screen, letterboxed on a tall one.
    const f32 w = std::min(screen.w, screen.h * 4.0f / 3.0f), h = w * 0.75f;
    l.art = {screen.centerX() - w * 0.5f, screen.centerY() - h * 0.5f, w, h};
    const auto quad = [&](f32 cx, f32 cy, f32 qw, f32 qh) {
        return ui::Rect{l.art.x + (cx - qw * 0.5f) * w, l.art.bottom() - (cy + qh * 0.5f) * h, qw * w, qh * h};
    };
    l.border = quad(0.5f, 0.075f, 0.6f, 0.05f);
    l.fill = quad(0.5f, 0.075f, 0.525f, 0.025f);
    const f32 lift = std::max(0.0f, l.border.bottom() - safe.bottom());
    l.border.y -= lift;
    l.fill.y -= lift;

    const f32 size = 0.018f * h;
    l.tipSize = std::max(size, minTipSize);
    const f32 grow = size > 0.0f ? l.tipSize / size : 1.0f;
    l.tipShadow = std::max(1.0f, 0.001f * h * grow);
    const f32 tipW = std::min(w * 515.0f / 1024.0f * grow, safe.w);
    const f32 tipX = std::clamp(l.art.centerX() - tipW * 0.5f, safe.x, safe.right() - tipW);
    l.tip = {tipX, screen.y, tipW, std::max(0.0f, l.border.y - screen.y)};
    return l;
}

} // namespace vd
