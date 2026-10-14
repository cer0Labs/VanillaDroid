#include "game/TouchLayoutEdit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace vd::game {
namespace {

constexpr i32 kSearchMm = 10; // a place dropped on another goes to the nearest free point this far at most

f32 radiusOf(const TouchGeometry& g, u32 p) {
    return g.places[p].size * 0.5f;
}

f32 snap(f32 v) {
    return std::round(v / kTouchGridMm) * kTouchGridMm;
}

// `p` centered at (x, y) keeps clear of every other element shown.
bool clearAt(const TouchGeometry& g, u32 p, f32 x, f32 y) {
    const f32 r = radiusOf(g, p);
    for (u32 q = 0; q < kTouchPlaces; ++q) {
        const TouchPlacement& other = g.places[q];
        if (q == p || other.size <= 0.0f) {
            continue;
        }
        if (std::hypot(x - other.x, y - other.y) < r + other.size * 0.5f + kTouchGapMm - 1e-4f) {
            return false;
        }
    }
    return true;
}

bool insideArea(const TouchArea& area, f32 x, f32 y, f32 r) {
    return area.empty() || (x - r >= area.left - 1e-4f && x + r <= area.right + 1e-4f && y - r >= area.bottom - 1e-4f &&
                            y + r <= area.top + 1e-4f);
}

bool sameAction(const TouchAction& a, const TouchAction& b) {
    return a.kind == b.kind && a.id == b.id && a.hidden() == b.hidden();
}

// What a layer's slot keeps so that it shows `action`: nothing (inherit) when it is the base's own, else the action
// as the player's.
TouchAction layerEntry(const TouchTemplate& t, u32 slot, TouchAction action) {
    if (sameAction(action, t.layers[0][slot]) && !action.hidden()) {
        return {};
    }
    if (action.empty() && !action.hidden()) {
        return {}; // nothing shown: the (empty) base's
    }
    action.flags |= kTouchManual;
    return action;
}

} // namespace

// --- Places ---

bool moveTouchPlace(TouchGeometry& g, TouchPlace place, f32 x, f32 y, const TouchArea& area) {
    const u32 p = touchIndex(place);
    if (p >= kTouchPlaces || g.places[p].size <= 0.0f) {
        return false;
    }
    if (place == TouchPlace::Main) {
        // The anchor: the whole cluster shifts, its extent kept inside the area.
        f32 minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
        for (const TouchPlacement& at : g.places) {
            if (at.size > 0.0f) {
                minX = std::min(minX, at.x - at.size * 0.5f);
                maxX = std::max(maxX, at.x + at.size * 0.5f);
                minY = std::min(minY, at.y - at.size * 0.5f);
                maxY = std::max(maxY, at.y + at.size * 0.5f);
            }
        }
        f32 dx = snap(x - g.places[p].x), dy = snap(y - g.places[p].y);
        if (!area.empty()) {
            // Snapped inward: the grid never pushes the cluster out of the area.
            const f32 loX = std::ceil((area.left - minX) / kTouchGridMm) * kTouchGridMm;
            const f32 hiX = std::floor((area.right - maxX) / kTouchGridMm) * kTouchGridMm;
            const f32 loY = std::ceil((area.bottom - minY) / kTouchGridMm) * kTouchGridMm;
            const f32 hiY = std::floor((area.top - maxY) / kTouchGridMm) * kTouchGridMm;
            dx = loX <= hiX ? std::clamp(dx, loX, hiX) : 0.0f;
            dy = loY <= hiY ? std::clamp(dy, loY, hiY) : 0.0f;
        }
        if (dx == 0.0f && dy == 0.0f) {
            return false;
        }
        // Main's center moves by (dx, dy): x toward the middle of the screen is away from the edge it is measured from.
        g.mainRight -= dx;
        g.mainBottom += dy;
        return true;
    }
    const f32 r = radiusOf(g, p);
    const f32 sx = snap(x), sy = snap(y);
    f32 bestX = 0.0f, bestY = 0.0f;
    bool found = false;
    for (i32 ring = 0; ring <= kSearchMm && !found; ++ring) {
        f32 best = std::numeric_limits<f32>::max();
        for (i32 i = -ring; i <= ring; ++i) {
            for (i32 j = -ring; j <= ring; ++j) {
                if (std::max(std::abs(i), std::abs(j)) != ring) {
                    continue; // only the ring's border: the inner ones were tried
                }
                const f32 cx = sx + static_cast<f32>(i) * kTouchGridMm, cy = sy + static_cast<f32>(j) * kTouchGridMm;
                if (!insideArea(area, cx, cy, r) || !clearAt(g, p, cx, cy)) {
                    continue;
                }
                const f32 d = std::hypot(cx - x, cy - y);
                if (d < best) {
                    best = d;
                    bestX = cx;
                    bestY = cy;
                    found = true;
                }
            }
        }
    }
    if (!found || (bestX == g.places[p].x && bestY == g.places[p].y)) {
        return false;
    }
    g.places[p].x = bestX;
    g.places[p].y = bestY;
    return true;
}

f32 touchPlaceSizePercent(const TouchGeometry& g, TouchProfile profile, TouchPlace place) {
    const u32 p = touchIndex(place);
    const f32 standard = defaultTouchGeometry(profile).places[p].size;
    return standard > 0.0f ? g.places[p].size / standard * 100.0f : 100.0f;
}

f32 resizeTouchPlace(TouchGeometry& g, TouchProfile profile, TouchPlace place, f32 percent) {
    const u32 p = touchIndex(place);
    const f32 standard = defaultTouchGeometry(profile).places[p].size;
    if (p >= kTouchPlaces || standard <= 0.0f) {
        return 100.0f;
    }
    const f32 want = standard * std::clamp(percent, kTouchMinSizePercent, kTouchMaxSizePercent) / 100.0f;
    const f32 now = g.places[p].size;
    f32 size = want;
    if (want > now) {
        // Grows only as far as the nearest neighbour lets it.
        f32 room = std::numeric_limits<f32>::max();
        for (u32 q = 0; q < kTouchPlaces; ++q) {
            const TouchPlacement& other = g.places[q];
            if (q != p && other.size > 0.0f) {
                room = std::min(room, std::hypot(g.places[p].x - other.x, g.places[p].y - other.y) - other.size * 0.5f -
                                          kTouchGapMm);
            }
        }
        size = std::max(now, std::min(want, 2.0f * room));
    }
    g.places[p].size = size;
    return size / standard * 100.0f;
}

void setTouchScale(TouchGeometry& g, f32 scale) {
    g.scale = std::clamp(scale, kTouchMinScale, kTouchMaxScale);
}

void resetTouchGeometry(TouchGeometry& g, TouchProfile profile) {
    g = defaultTouchGeometry(profile);
}

void copyTouchGeometryToAll(TouchLayout& layout, u32 from) {
    if (from >= layout.count) {
        return;
    }
    for (u32 i = 0; i < layout.count; ++i) {
        if (i != from) {
            layout.templates[i].geometry = layout.templates[from].geometry;
        }
    }
}

bool touchPlacesOverlap(const TouchGeometry& g, TouchPlace a, TouchPlace b) {
    const TouchPlacement& pa = g.places[touchIndex(a)];
    const TouchPlacement& pb = g.places[touchIndex(b)];
    if (a == b || pa.size <= 0.0f || pb.size <= 0.0f) {
        return false;
    }
    return std::hypot(pa.x - pb.x, pa.y - pb.y) < (pa.size + pb.size) * 0.5f + kTouchGapMm - 1e-4f;
}

// --- Actions ---

void putTouchAction(TouchTemplate& t, TouchContext layer, u32 slot, TouchAction action) {
    const u32 c = static_cast<u32>(layer);
    if (slot >= kTouchSlots || c >= kTouchContexts) {
        return;
    }
    if (action.empty()) {
        hideTouchSlot(t, layer, slot);
        return;
    }
    action.flags = static_cast<u8>((action.flags | kTouchManual) & ~kTouchHidden);
    t.layers[c][slot] = action;
}

void hideTouchSlot(TouchTemplate& t, TouchContext layer, u32 slot) {
    const u32 c = static_cast<u32>(layer);
    if (slot < kTouchSlots && c < kTouchContexts) {
        t.layers[c][slot] = {TouchActionKind::Empty, static_cast<u8>(kTouchHidden | kTouchManual), 0, 0};
    }
}

void inheritTouchSlot(TouchTemplate& t, TouchContext layer, u32 slot) {
    const u32 c = static_cast<u32>(layer);
    if (slot < kTouchSlots && c != 0 && c < kTouchContexts) {
        t.layers[c][slot] = {};
    }
}

TouchAction touchSlotShown(const TouchTemplate& t, TouchContext layer, u32 slot, bool& inherited) {
    inherited = false;
    const u32 c = static_cast<u32>(layer);
    if (slot >= kTouchSlots || c >= kTouchContexts) {
        return {};
    }
    const TouchAction& own = t.layers[c][slot];
    if (c == 0 || own.hidden() || !own.empty()) {
        return own;
    }
    inherited = true;
    return t.layers[0][slot];
}

void swapTouchSlots(TouchTemplate& t, TouchContext layer, u32 a, u32 b) {
    const u32 c = static_cast<u32>(layer);
    if (a == b || a >= kTouchSlots || b >= kTouchSlots || c >= kTouchContexts) {
        return;
    }
    if (c == 0) {
        std::swap(t.layers[0][a], t.layers[0][b]);
        for (const u32 s : {a, b}) {
            TouchAction& x = t.layers[0][s];
            if (!x.empty() || x.hidden()) {
                x.flags |= kTouchManual;
            }
        }
        return;
    }
    bool inheritedA = false, inheritedB = false;
    const TouchAction shownA = touchSlotShown(t, layer, a, inheritedA);
    const TouchAction shownB = touchSlotShown(t, layer, b, inheritedB);
    t.layers[c][a] = layerEntry(t, a, shownB);
    t.layers[c][b] = layerEntry(t, b, shownA);
}

// --- Groups ---

i32 touchSlotGroup(const TouchTemplate& t, TouchContext layer, u32 slot) {
    bool inherited = false;
    const TouchAction a = touchSlotShown(t, layer, slot, inherited);
    return a.kind == TouchActionKind::Group && a.id < kTouchGroups ? static_cast<i32>(a.id) : -1;
}

bool touchGroupUsed(const TouchTemplate& t, u32 index) {
    for (const auto& layer : t.layers) {
        for (const TouchAction& a : layer) {
            if (a.kind == TouchActionKind::Group && a.id == index) {
                return true;
            }
        }
    }
    for (const u8 g : t.classBarGroups) {
        if (g != 0 && g - 1u == index) {
            return true;
        }
    }
    return false;
}

i32 freeTouchGroup(const TouchTemplate& t) {
    for (u32 i = 0; i < kTouchGroups; ++i) {
        const TouchGroup& g = t.groups[i];
        if (g.count == 0 && g.autoKind == 0 && !touchGroupUsed(t, i)) {
            return static_cast<i32>(i);
        }
    }
    // A group the player made that nothing shows any more (its slot got something else).
    for (u32 i = 0; i < kTouchGroups; ++i) {
        if ((t.groups[i].flags & kTouchManual) != 0 && !touchGroupUsed(t, i)) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 putTouchGroup(TouchTemplate& t, TouchContext layer, u32 slot, std::span<const TouchAction> actions) {
    const u32 c = static_cast<u32>(layer);
    if (slot >= kTouchSlots || c >= kTouchContexts) {
        return -1;
    }
    std::array<TouchAction, TouchGroup::kMaxActions> list{};
    u32 n = 0;
    for (const TouchAction& a : actions) {
        if (n < list.size() && !a.empty() && !a.hidden() && a.kind != TouchActionKind::Group) {
            list[n] = a;
            list[n].flags &= kTouchFixedRank; // a group's actions keep their rank only
            ++n;
        }
    }
    if (n == 0) {
        return -1;
    }
    // The slot's own group is changed in place; an inherited one (the base's, in a layer) is left to the base.
    const TouchAction& own = t.layers[c][slot];
    i32 index = own.kind == TouchActionKind::Group && own.id < kTouchGroups ? static_cast<i32>(own.id) : -1;
    if (index < 0) {
        index = freeTouchGroup(t);
    }
    if (index < 0) {
        return -1;
    }
    TouchGroup& g = t.groups[static_cast<u32>(index)];
    const TouchAction current = g.count > 0 ? g.actions[std::min<u32>(g.current, g.count - 1u)] : TouchAction{};
    const u8 autoKind = g.autoKind;
    g = {};
    g.autoKind = autoKind; // kept for what it was made from; the player's list now (presets leave it)
    g.flags = kTouchManual;
    for (u32 i = 0; i < n; ++i) {
        g.actions[i] = list[i];
        if (!current.empty() && list[i].kind == current.kind && list[i].id == current.id) {
            g.current = static_cast<u8>(i);
        }
    }
    g.count = static_cast<u8>(n);
    putTouchAction(t, layer, slot, {TouchActionKind::Group, 0, 0, static_cast<u32>(index)});
    return index;
}

// --- Templates ---

i32 addTouchTemplate(TouchLayout& layout, const TouchTemplate& t) {
    if (layout.count >= kMaxTouchTemplates) {
        return -1;
    }
    layout.templates[layout.count] = t;
    return static_cast<i32>(layout.count++);
}

bool removeTouchTemplate(TouchLayout& layout, u32 index) {
    if (index >= layout.count || layout.count <= 1) {
        return false;
    }
    for (u32 i = index; i + 1 < layout.count; ++i) {
        layout.templates[i] = layout.templates[i + 1];
    }
    --layout.count;
    layout.templates[layout.count] = {};
    if (layout.active > index) {
        --layout.active;
    } else if (layout.active == index) {
        layout.active = std::min(index, layout.count - 1);
    }
    return true;
}

void uniqueTouchTemplateName(const TouchLayout& layout, std::string_view base, std::array<char, kTouchNameBytes>& out) {
    // "Solo 2" copied again is "Solo 3", not "Solo 2 2".
    if (const usize space = base.rfind(' '); space != std::string_view::npos && space + 1 < base.size() &&
                                             std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1,
                                                         base.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        base = base.substr(0, space);
    }
    const auto taken = [&](std::string_view name) {
        for (u32 i = 0; i < layout.count; ++i) {
            if (layout.templates[i].nameView() == name) {
                return true;
            }
        }
        return false;
    };
    for (u32 n = 1; n <= kMaxTouchTemplates + 1; ++n) {
        char suffix[8] = {};
        const int suffixBytes = n == 1 ? 0 : std::snprintf(suffix, sizeof(suffix), " %u", n);
        usize keep = std::min<usize>(base.size(), out.size() - 1 - static_cast<usize>(suffixBytes));
        while (keep > 0 && keep < base.size() && (static_cast<u8>(base[keep]) & 0xC0) == 0x80) {
            --keep; // never half a UTF-8 character
        }
        out = {};
        std::copy_n(base.data(), keep, out.data());
        std::copy_n(suffix, static_cast<usize>(suffixBytes), out.data() + keep);
        if (!taken(std::string_view(out.data()))) {
            return;
        }
    }
}

bool restoreTouchTemplate(TouchTemplate& t, const TouchLayout& presets) {
    const TouchTemplate* from = nullptr;
    for (u32 i = 0; i < presets.count && !from; ++i) {
        if (t.role != 0 && presets.templates[i].role == t.role) {
            from = &presets.templates[i];
        }
    }
    if (!from && presets.count > 0) {
        from = &presets.templates[0];
    }
    if (!from) {
        return false;
    }
    t.layers = from->layers;
    t.groups = from->groups;
    t.classBarGroups = from->classBarGroups;
    t.disabledLayers = from->disabledLayers;
    t.role = from->role;
    return true;
}

bool isTouchLayoutText(std::string_view text) {
    constexpr std::string_view kOurs = "VDTOUCH";
    return text.substr(0, kOurs.size()) == kOurs;
}

} // namespace vd::game
