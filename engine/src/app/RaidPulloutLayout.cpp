#include "app/RaidPulloutLayout.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr f32 kButtonWidth = 57.0f, kButtonHeight = 25.0f; // RaidPulloutButtonTemplate
constexpr f32 kButtonLeft =
    (kRaidPulloutWidth - kButtonWidth) * 0.5f + 1.0f; // SetPoint("TOP", frame, "TOP", 1, -10)
constexpr f32 kButtonTop = 10.0f;
constexpr f32 kAuraSize = 12.0f; // RaidBuffButtonTemplate

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

// The rectangle moved inside `safe` (its top left kept when it does not fit).
Rect clampInside(Rect r, const Rect& safe) {
    r.x = std::max(safe.x, std::min(r.x, safe.right() - r.w));
    r.y = std::max(safe.y, std::min(r.y, safe.bottom() - r.h));
    return r;
}

bool inside(const Rect& r, const Rect& safe) {
    constexpr f32 kSlack = 0.01f;
    return r.x >= safe.x - kSlack && r.y >= safe.y - kSlack && r.right() <= safe.right() + kSlack &&
           r.bottom() <= safe.bottom() + kSlack;
}

} // namespace

f32 raidPulloutUnit(f32 baseUnit, u32 members, f32 screenHeight) {
    const f32 units =
        kRaidPulloutTitle + static_cast<f32>(members) * kRaidPulloutButtonHeight + kRaidPulloutPadding;
    const f32 large = baseUnit * kRaidPulloutScale;
    if (units * large <= screenHeight || units <= 0.0f) {
        return large;
    }
    return std::max(baseUnit, screenHeight / units);
}

u32 raidPowerColor(u8 powerType) {
    constexpr u32 kColors[] = {rgba(0, 0, 255), rgba(255, 0, 0), rgba(255, 128, 64), rgba(255, 255, 0),
                               rgba(0, 255, 255)};
    return kColors[std::min<u32>(powerType, 4)];
}

Rect RaidPulloutGeometry::frame() const {
    return {x, y, kRaidPulloutWidth * unit,
            (static_cast<f32>(members) * kRaidPulloutButtonHeight + kRaidPulloutPadding) * unit};
}

Rect RaidPulloutGeometry::title() const {
    return {x, y - kRaidPulloutTitle * unit, kRaidPulloutWidth * unit, kRaidPulloutTitle * unit};
}

Rect RaidPulloutGeometry::button(u32 i) const {
    return {x + kButtonLeft * unit, y + (kButtonTop + static_cast<f32>(i) * kRaidPulloutButtonHeight) * unit,
            kButtonWidth * unit, kButtonHeight * unit};
}

Rect RaidPulloutGeometry::row(u32 i) const {
    const Rect b = button(i);
    return {x, b.y - 4.0f * unit, kRaidPulloutWidth * unit, kRaidPulloutButtonHeight * unit};
}

Rect RaidPulloutGeometry::barFrame(u32 i) const {
    const Rect b = button(i);
    return {b.x, b.bottom() - 16.0f * unit, 64.0f * unit, 16.0f * unit};
}

Rect RaidPulloutGeometry::name(u32 i) const {
    const Rect b = button(i);
    return {b.x + 2.0f * unit, b.y - 3.0f * unit, 52.0f * unit, 12.0f * unit};
}

Rect RaidPulloutGeometry::health(u32 i) const {
    const Rect b = button(i);
    return {b.x + 3.0f * unit, b.bottom() - 14.0f * unit, 51.0f * unit, 4.0f * unit};
}

Rect RaidPulloutGeometry::mana(u32 i) const {
    const Rect h = health(i);
    return {h.x, h.bottom() + 3.0f * unit, h.w, h.h};
}

Rect RaidPulloutGeometry::aura(u32 i, u32 a) const {
    const Rect b = button(i);
    const f32 step = (kAuraSize + 1.0f) * unit;
    return {b.right() + 6.0f * unit + static_cast<f32>(a % 2) * step,
            b.y + 1.0f * unit + static_cast<f32>(a / 2) * step, kAuraSize * unit, kAuraSize * unit};
}

Rect RaidPulloutGeometry::hit() const {
    const Rect f = frame();
    return {x, y - kRaidPulloutTitle * unit, kRaidPulloutReach * unit, f.h + kRaidPulloutTitle * unit};
}

Rect raidPulloutPlace(const Rect& hit, const Rect& safe, std::span<const Rect> avoid) {
    const Rect r = clampInside(hit, safe);
    // How much of the rectangles to keep clear a place covers (0: none).
    const auto covered = [&](const Rect& c) {
        f32 area = 0.0f;
        for (const Rect& a : avoid) {
            if (a.w > 0.0f && a.h > 0.0f && overlaps(c, a)) {
                area += (std::min(c.right(), a.right()) - std::max(c.x, a.x)) *
                        (std::min(c.bottom(), a.bottom()) - std::max(c.y, a.y));
            }
        }
        return area;
    };
    if (covered(r) <= 0.0f) {
        return r;
    }
    // The places against each edge of the rectangles to keep clear (and the column and row where it was), in
    // every combination: the nearest one on the screen that covers none of them; failing that, the one
    // covering least.
    constexpr u32 kMaxEdges = 1 + 2 * 32;
    std::array<f32, kMaxEdges> xs{}, ys{};
    u32 nx = 0, ny = 0;
    xs[nx++] = r.x;
    ys[ny++] = r.y;
    for (const Rect& a : avoid) {
        if (a.w <= 0.0f || a.h <= 0.0f || nx + 2 > kMaxEdges) {
            continue;
        }
        xs[nx++] = a.x - r.w;
        xs[nx++] = a.right();
        ys[ny++] = a.y - r.h;
        ys[ny++] = a.bottom();
    }
    Rect best = r;
    f32 bestCover = covered(r), bestDistance = 0.0f;
    for (u32 ix = 0; ix < nx; ++ix) {
        for (u32 iy = 0; iy < ny; ++iy) {
            const Rect c{xs[ix], ys[iy], r.w, r.h};
            if (!inside(c, safe)) {
                continue;
            }
            const f32 cover = covered(c);
            const f32 distance = std::fabs(c.x - r.x) + std::fabs(c.y - r.y);
            if (cover < bestCover || (cover == bestCover && distance < bestDistance)) {
                best = c;
                bestCover = cover;
                bestDistance = distance;
            }
        }
    }
    return best;
}

Rect raidPulloutDemoCell(u32 n, const Rect& space, f32 w, f32 h) {
    const u32 cols = std::max(1u, static_cast<u32>(space.w / std::max(w, 1.0f)));
    const u32 rows = std::max(1u, static_cast<u32>(space.h / std::max(h, 1.0f)));
    const u32 cell = n % (cols * rows);
    const f32 cascade =
        static_cast<f32>(n / (cols * rows)); // a full space: over the first ones, a bit lower right
    return {space.x + static_cast<f32>(cell % cols) * w + cascade * 0.3f * w,
            space.y + static_cast<f32>(cell / cols) * h + cascade * 0.12f * h, w, h};
}

} // namespace vd
