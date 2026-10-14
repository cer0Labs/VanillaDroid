#include "app/MinimapView.h"

#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "pack/TerrainFormat.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::withAlpha;

constexpr u64 kBorder = assetPathHash("Interface\\Minimap\\UI-Minimap-Border.blp");
constexpr u64 kPlayerArrow = assetPathHash("Interface\\Minimap\\MinimapArrow.blp");
constexpr u64 kCircleMask = assetPathHash("Interface\\CharacterFrame\\TempPortraitAlphaMask.blp");
constexpr u64 kObjectIcons = assetPathHash("Interface\\Minimap\\ObjectIcons.blp");
constexpr u64 kGroupArrow = assetPathHash("Interface\\Minimap\\Rotating-MinimapGroupArrow.blp");
constexpr u64 kCorpseArrow = assetPathHash("Interface\\Minimap\\ROTATING-MINIMAPCORPSEARROW.blp");
// Phase 13: the gossip point (POIIcons: an 8 x 8 grid of 16-pixel icons) and its arrow beyond the map.
constexpr u64 kPoiIcons = assetPathHash("Interface\\Minimap\\POIIcons.blp");
constexpr u64 kGuideArrow = assetPathHash("Interface\\Minimap\\ROTATING-MINIMAPGUIDEARROW.blp");
constexpr f32 kPoiUnits = 16.0f; // the icon's size in interface units (its cell's pixels)
constexpr u64 kZoomIn[3] = {assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomInButton-Up.blp"),
                            assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomInButton-Down.blp"),
                            assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomInButton-Disabled.blp")};
constexpr u64 kZoomOut[3] = {assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomOutButton-Up.blp"),
                             assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomOutButton-Down.blp"),
                             assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomOutButton-Disabled.blp")};
constexpr u64 kZoomHighlight = assetPathHash("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight.blp");
constexpr u64 kToggleUp = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Up.blp");
constexpr u64 kToggleDown = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Down.blp");
constexpr u64 kToggleHighlight = assetPathHash("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight.blp");
constexpr u64 kTrackingBorder = assetPathHash("Interface\\Minimap\\MiniMap-TrackingBorder.blp");
constexpr u64 kTimeOfDay = assetPathHash("Interface\\Minimap\\UI-TOD-Indicator.blp");
constexpr u64 kLetter = assetPathHash("Interface\\Icons\\INV_Letter_15.blp"); // MiniMapMailIcon
constexpr u64 kRune = assetPathHash("Interface\\Icons\\INV_Misc_Rune_06.blp"); // phase 16: MiniMapMeetingStoneIcon

constexpr f32 kBlipUnits = 13.0f;   // a dot on the map
constexpr f32 kArrowUnits = 24.0f;  // a rim arrow
constexpr f32 kPinchStepDp = 40.0f; // two fingers moving apart (together) this much zoom one level in (out)

// A sprite turned `angle` radians clockwise (its art points up), centered on (x, y).
void rotatedSprite(ui::DrawList& draw, const ui::Sprite& sprite, f32 x, f32 y, f32 half, f32 angle, u32 color,
                   ui::TexCoords uv = {}) {
    const f32 upX = std::sin(angle), upY = -std::cos(angle);
    const f32 rightX = -upY, rightY = upX;
    const f32 corners[8] = {x - rightX * half + upX * half, y - rightY * half + upY * half,
                            x + rightX * half + upX * half, y + rightY * half + upY * half,
                            x + rightX * half - upX * half, y + rightY * half - upY * half,
                            x - rightX * half - upX * half, y - rightY * half - upY * half};
    draw.spriteQuad(corners, sprite, color, uv);
}

// The map's tiles around the centre: north up, west left (a tile's texture u runs east, v south).
void drawTiles(ui::DrawList& draw, UiResources& resources, const MinimapLayout& l, const MinimapView& view) {
    if (view.interior && view.radiusYards > 0.0f) { // phase 15: the building's map, turned with the building
        const f64 scale = l.radius / view.radiusYards;
        const f64 px = view.position.x, py = view.position.y;
        draw.sprite(l.map(), kCircleMask, rgba(0, 0, 0, 255));
        for (const MinimapInteriorTile& tile : view.interiorTiles) {
            f32 corners[8];
            for (u32 i = 0; i < 4; ++i) { // north up, west left
                corners[2 * i] = static_cast<f32>(l.cx + (py - tile.corners[2 * i + 1]) * scale);
                corners[2 * i + 1] = static_cast<f32>(l.cy + (px - tile.corners[2 * i]) * scale);
            }
            draw.textureQuadInCircle(corners, tile.texture, l.cx, l.cy, l.radius);
        }
        return;
    }
    if (view.map.empty() || view.radiusYards <= 0.0f) {
        return;
    }
    const f64 yards = view.radiusYards;
    const f64 scale = l.radius / yards;
    const f64 px = view.position.x, py = view.position.y;
    const i32 minTx = static_cast<i32>(std::floor(32.0 - (py + yards) / pack::kTileSize));
    const i32 maxTx = static_cast<i32>(std::floor(32.0 - (py - yards) / pack::kTileSize));
    const i32 minTy = static_cast<i32>(std::floor(32.0 - (px + yards) / pack::kTileSize));
    const i32 maxTy = static_cast<i32>(std::floor(32.0 - (px - yards) / pack::kTileSize));
    for (i32 ty = std::max(0, minTy); ty <= std::min(63, maxTy); ++ty) {
        for (i32 tx = std::max(0, minTx); tx <= std::min(63, maxTx); ++tx) {
            const TextureHandle texture =
                resources.minimapTile(view.map, static_cast<u32>(tx), static_cast<u32>(ty));
            if (!texture.valid()) {
                continue;
            }
            const f64 cornerX = pack::tileCornerX(static_cast<u32>(ty));
            const f64 cornerY = pack::tileCornerY(static_cast<u32>(tx));
            const Rect r{static_cast<f32>(l.cx + (py - cornerY) * scale),
                         static_cast<f32>(l.cy + (px - cornerX) * scale),
                         static_cast<f32>(pack::kTileSize * scale),
                         static_cast<f32>(pack::kTileSize * scale)};
            draw.textureInCircle(r, texture, l.cx, l.cy, l.radius);
        }
    }
}

void drawBlips(ui::DrawList& draw, const ui::Assets& assets, const MinimapLayout& l,
               const MinimapView& view) {
    const std::optional<ui::Sprite> icons = assets.sprite(kObjectIcons);
    const std::optional<ui::Sprite> group = assets.sprite(kGroupArrow);
    const std::optional<ui::Sprite> corpse = assets.sprite(kCorpseArrow);
    const std::optional<ui::Sprite> poiIcons = assets.sprite(kPoiIcons);
    const std::optional<ui::Sprite> guide = assets.sprite(kGuideArrow);
    const f32 dot = kBlipUnits * l.s;
    // Dots first, the rim arrows over them.
    for (const MinimapBlipView& b : view.blips) {
        if (b.art != MinimapBlipArt::Dot || !icons) {
            continue;
        }
        const f32 col = static_cast<f32>(b.cell % 4), row = static_cast<f32>(b.cell / 4);
        draw.sprite({l.cx + b.x * l.radius - dot * 0.5f, l.cy + b.y * l.radius - dot * 0.5f, dot, dot},
                    *icons, b.tint, {col * 0.25f, row * 0.25f, (col + 1.0f) * 0.25f, (row + 1.0f) * 0.25f});
    }
    const f32 poi = kPoiUnits * l.s;
    for (const MinimapBlipView& b : view.blips) {
        if (b.art != MinimapBlipArt::Poi || !poiIcons) {
            continue;
        }
        const f32 col = static_cast<f32>(b.cell % 8), row = static_cast<f32>((b.cell / 8) % 8);
        draw.sprite({l.cx + b.x * l.radius - poi * 0.5f, l.cy + b.y * l.radius - poi * 0.5f, poi, poi},
                    *poiIcons, b.tint,
                    {col * 0.125f, row * 0.125f, (col + 1.0f) * 0.125f, (row + 1.0f) * 0.125f});
    }
    for (const MinimapBlipView& b : view.blips) {
        const std::optional<ui::Sprite>* art = b.art == MinimapBlipArt::GroupArrow    ? &group
                                               : b.art == MinimapBlipArt::CorpseArrow ? &corpse
                                               : b.art == MinimapBlipArt::GuideArrow  ? &guide
                                                                                      : nullptr;
        if (art && *art) {
            rotatedSprite(draw, **art, l.cx + b.x * l.radius, l.cy + b.y * l.radius, kArrowUnits * l.s * 0.5f,
                          b.angle, b.tint);
        }
    }
}

// MiniMapPing (Minimap.xml: the 50-unit Model frame of Interface\MiniMap\Ping\MinimapPing.m2 at scale 0.4, centered on
// the ping): retouches (ui models), the model itself (ui/UiModels): its four arrows close in turning (Birth), then
// a ring ripples out of the dot every 0.83 s (Stand). The model draws its centre (7.7, 8.1) units up and right of the
// frame's (Minimap_OnClick's cursor offset puts a click's ping that far the other way: app/MinimapScreens).
void drawPing(ui::DrawList& draw, const MinimapLayout& l, const MinimapView& view) {
    if (view.pingAlpha <= 0.0f || view.pingX * view.pingX + view.pingY * view.pingY > 1.0f) {
        return; // Minimap_SetPing hides pings beyond the map
    }
    const f32 x = l.cx + view.pingX * l.radius, y = l.cy + view.pingY * l.radius;
    draw.models().minimapPing(draw, x, y, l.s, view.pingSeconds * 1000.0f, view.pingAlpha);
}

// A button of three textures (Normal, Pushed, Disabled) with an added highlight while pressed; true when
// tapped.
bool artButton(ui::Context& ui, ui::DrawList& draw, ui::WidgetId id, const Rect& touch, const Rect& art,
               const u64 (&textures)[3], u64 highlight, bool enabled) {
    const ui::WidgetState state = ui.widget(id, touch);
    const bool pressed = enabled && state.pressed;
    draw.sprite(art, textures[!enabled ? 2 : pressed ? 1 : 0]);
    if (pressed) {
        draw.spriteAdditive(art, highlight);
    }
    return enabled && state.clicked;
}

} // namespace

// --- Layout ------------------------------------------------------------------------------------------------

MinimapLayout minimapLayout(const Rect& safe, f32 unitPixels, f32 dp) {
    MinimapLayout l;
    l.s = unitPixels;
    l.left = safe.right() - 4.0f * dp - 192.0f * l.s;
    l.top = safe.y + 2.0f * dp;
    // Minimap: 140 x 140, CENTER at the cluster's TOP + (9, -92).
    l.cx = l.left + 105.0f * l.s;
    l.cy = l.top + 92.0f * l.s;
    l.radius = 70.0f * l.s;
    return l;
}

// Touch targets: the XML frames, grown towards the screen's edges where they touch them, the small buttons to
// ~40 units (31 dp: their neighbors leave no room for 44).
Rect MinimapLayout::zoneButton() const {
    return at(24.0f, -3.0f, 128.0f, 27.0f); // the plate (texels 25..150 of MinimapBorderTop)
}
Rect MinimapLayout::toggle() const {
    return at(156.0f, -3.0f, 40.0f, 27.0f); // CENTER at TOPRIGHT + (-15, -13), 32 x 32: the bar's right end
}
Rect MinimapLayout::time() const {
    return at(148.0f, 24.0f, 48.0f, 36.0f); // TOPRIGHT + (4, -19), 50 x 50, hit insets (6, 0, 5, 10)
}
Rect MinimapLayout::zoomIn() const {
    return at(173.0f - 21.0f, 129.0f - 21.0f, 42.0f, 42.0f); // MinimapBackdrop CENTER + (77, -13)
}
Rect MinimapLayout::zoomOut() const {
    return at(147.0f - 21.0f, 157.0f - 21.0f, 42.0f, 42.0f); // MinimapBackdrop CENTER + (51, -41)
}
Rect MinimapLayout::tracking() const {
    return at(18.0f, 20.0f, 37.0f, 37.0f); // Minimap TOPLEFT + (-15, 0), 33 x 33
}
Rect MinimapLayout::mail() const {
    return at(161.0f, 60.0f, 37.0f, 35.0f); // Minimap TOPRIGHT + (21, -38), 33 x 33
}
Rect MinimapLayout::meetingStone() const {
    return at(163.0f, 87.0f, 37.0f, 35.0f); // phase 16: Minimap TOPRIGHT + (23, -65), 33 x 33
}

namespace {
constexpr f32 kRimDistance = 77.9f; // units from the map's centre (105, 92) to the ring's
constexpr f32 kRingCentre = 16.0f;  // MiniMap-TrackingBorder at 52 units: its ring's centre from its top-left
constexpr f32 kRimIcon = 22.0f;     // the round icon: wider than the ring's hole, its edge under the ring

// The top-left of the ring's art (in the cluster's units) for a button at `degrees`.
void rimPlace(f32 degrees, f32& x, f32& y) {
    const f32 a = degrees * (3.14159265f / 180.0f);
    x = 105.0f + kRimDistance * std::cos(a) - kRingCentre;
    y = 92.0f - kRimDistance * std::sin(a) - kRingCentre;
}
} // namespace

Rect minimapRimButtonRect(const MinimapLayout& l, f32 degrees) {
    f32 x = 0.0f, y = 0.0f;
    rimPlace(degrees, x, y);
    return l.at(x - 4.0f, y - 4.0f, 41.0f, 41.0f);
}

ui::WidgetState minimapRimButton(ui::Context& ui, ui::DrawList& draw, ui::WidgetId id, const MinimapLayout& l,
                                 f32 degrees, std::optional<u32> icon, bool lit) {
    f32 x = 0.0f, y = 0.0f;
    rimPlace(degrees, x, y);
    const ui::WidgetState state = ui.widget(id, minimapRimButtonRect(l, degrees));
    if (icon) {
        const f32 inset = kRingCentre - kRimIcon * 0.5f;
        draw.iconCircle(l.at(x + inset, y + inset, kRimIcon, kRimIcon), *icon,
                        state.pressed ? rgba(190, 190, 190) : ui::kWhite);
    }
    draw.sprite(l.at(x, y, 52.0f, 52.0f), kTrackingBorder);
    if (lit || state.pressed) {
        draw.spriteAdditive(l.at(x, y, 33.0f, 33.0f), kZoomHighlight);
    }
    return state;
}

// --- The cluster -------------------------------------------------------------------------------------------

MinimapResult drawMinimap(ui::Context& ui, ui::DrawList& draw, UiResources& resources, const MinimapLayout& l,
                          const MinimapView& view, MinimapGesture& gesture) {
    VD_PROFILE_ZONE("Minimap::draw");
    MinimapResult result;
    const f32 s = l.s;
    const ui::Assets& assets = resources.assets();

    if (view.shown) {
        // The map: what a finger does on it (a tap pings, two fingers zoom).
        const ui::WidgetId mapId = ui::widgetId("minimap.map");
        const ui::WidgetState state = ui.widget(mapId, l.map());
        const ui::Touch* fingers[2] = {nullptr, nullptr};
        u32 down = 0;
        bool owned = false;
        for (const ui::Touch& t : ui.touches()) {
            if (t.owner != mapId) {
                continue;
            }
            owned = true;
            if (!t.released && down < 2) {
                fingers[down++] = &t;
            }
        }
        if (down == 2) {
            const f32 distance =
                std::hypot(fingers[0]->x - fingers[1]->x, fingers[0]->y - fingers[1]->y) / ui.dp();
            if (gesture.pinchDistance > 0.0f) {
                gesture.pinchCarry += distance - gesture.pinchDistance;
                while (gesture.pinchCarry >= kPinchStepDp) {
                    gesture.pinchCarry -= kPinchStepDp;
                    ++result.zoom;
                }
                while (gesture.pinchCarry <= -kPinchStepDp) {
                    gesture.pinchCarry += kPinchStepDp;
                    --result.zoom;
                }
            }
            gesture.pinchDistance = distance;
            gesture.pinched = true;
        } else {
            gesture.pinchDistance = 0.0f;
        }
        if (state.clicked && state.touch && !state.touch->moved && !gesture.pinched) {
            const f32 dx = (state.touch->x - l.cx) / l.radius, dy = (state.touch->y - l.cy) / l.radius;
            if (dx * dx + dy * dy <= 1.0f) {
                result.tappedMap = true;
                result.tapX = dx;
                result.tapY = dy;
            }
        }
        if (!owned) {
            gesture.pinched = false;
            gesture.pinchCarry = 0.0f;
        }

        draw.sprite(l.map(), kCircleMask, rgba(0, 0, 0, 255));
        drawTiles(draw, resources, l, view);
        drawBlips(draw, assets, l, view);
        drawPing(draw, l, view);
        // The player's arrow, turned with the character (its art points north).
        if (const std::optional<ui::Sprite> arrow = assets.sprite(kPlayerArrow)) {
            rotatedSprite(draw, *arrow, l.cx, l.cy, 16.0f * s * 0.7f, -view.facing, ui::kWhite);
        }
        // MinimapBackdrop: the ring, then the zoom buttons on it.
        draw.sprite(l.at(0.0f, 20.0f, 192.0f, 192.0f), kBorder, ui::kWhite, {0.25f, 0.125f, 1.0f, 0.875f});
        if (artButton(ui, draw, ui::widgetId("minimap.zoomIn"), l.zoomIn(),
                      l.at(157.0f, 113.0f, 32.0f, 32.0f), kZoomIn, kZoomHighlight, view.canZoomIn)) {
            ++result.zoom;
        }
        if (artButton(ui, draw, ui::widgetId("minimap.zoomOut"), l.zoomOut(),
                      l.at(131.0f, 141.0f, 32.0f, 32.0f), kZoomOut, kZoomHighlight, view.canZoomOut)) {
            --result.zoom;
        }
        // MiniMapTrackingFrame: the tracking spell's icon in its round border.
        if (view.trackingIcon != 0) {
            const ui::WidgetState tracking = ui.widget(ui::widgetId("minimap.tracking"), l.tracking());
            if (const std::optional<u32> icon = resources.icon(view.trackingIcon)) {
                draw.icon(l.at(27.0f, 28.0f, 26.0f, 26.0f), *icon,
                          tracking.pressed ? rgba(190, 190, 190) : ui::kWhite);
            }
            draw.sprite(l.at(20.0f, 22.0f, 64.0f, 64.0f), kTrackingBorder);
            if (tracking.longPress) {
                result.cancelTracking = true;
            } else if (tracking.clicked) {
                result.tapped = MinimapTip::Tracking;
            }
        }
        // MiniMapMailFrame: the letter while unread mail waits.
        if (view.newMail) {
            const ui::WidgetState mail = ui.widget(ui::widgetId("minimap.mail"), l.mail());
            if (const std::optional<u32> icon = resources.icon(kLetter)) {
                draw.icon(l.at(170.0f, 66.0f, 18.0f, 18.0f), *icon,
                          mail.pressed ? rgba(190, 190, 190) : ui::kWhite);
            }
            draw.sprite(l.at(163.0f, 60.0f, 52.0f, 52.0f), kTrackingBorder);
            if (mail.clicked) {
                result.tapped = MinimapTip::Mail;
            }
        }
        // Phase 16 (alerts): MiniMapMeetingStoneFrame while in a meeting stone's queue, under the letter: the
        // rune (18 x 18 at 7, -6) in the round border (52 x 52), the zoom buttons' highlight (ADD) while pressed.
        if (view.meetingStone) {
            const ui::WidgetState stone = ui.widget(ui::widgetId("minimap.meetingStone"), l.meetingStone());
            if (const std::optional<u32> icon = resources.icon(kRune)) {
                draw.icon(l.at(172.0f, 93.0f, 18.0f, 18.0f), *icon, ui::kWhite);
            }
            draw.sprite(l.at(165.0f, 87.0f, 52.0f, 52.0f), kTrackingBorder);
            if (stone.pressed) {
                draw.spriteAdditive(l.at(165.0f, 87.0f, 33.0f, 33.0f), kZoomHighlight);
            }
            if (stone.longPress) {
                result.tapped = MinimapTip::MeetingStone; // OnEnter's tooltip
            } else if (stone.clicked) {
                result.meetingStoneClicked = true; // OnClick
            }
        }
    } else {
        gesture = MinimapGesture{};
    }

    // MinimapBorderTop with the zone's name in its PvP color on the plate (MinimapZoneText: GameFontNormal,
    // 128 x 12, centered 13 units below the cluster's top).
    draw.sprite(l.at(0.0f, 0.0f, 192.0f, 32.0f), kBorder, ui::kWhite, {0.25f, 0.0f, 1.0f, 0.125f});
    const ui::WidgetState zone = ui.widget(ui::widgetId("minimap.zone"), l.zoneButton());
    if (!view.zoneName.empty()) {
        ui::TextStyle style = ui::gameFont(12.0f * s, view.zoneColor);
        style.outline = std::max(1.0f, s * 0.8f);
        style.shadow = 0.0f;
        style.align = ui::Align::Center;
        const f32 width = draw.measure(view.zoneName, style.font, style.size);
        const f32 room = 128.0f * s;
        if (width > room) {
            style.size *= room / width;
        }
        draw.text(l.left + 93.0f * s, ui::centeredBaseline(draw, style, l.top + 13.0f * s), view.zoneName,
                  style);
    }
    if (zone.clicked) {
        result.tapped = MinimapTip::Zone;
    }
    // MinimapToggleButton.
    {
        const ui::WidgetState toggle = ui.widget(ui::widgetId("minimap.toggle"), l.toggle());
        const Rect art = l.at(161.0f, -3.0f, 32.0f, 32.0f);
        draw.sprite(art, toggle.pressed ? kToggleDown : kToggleUp);
        if (toggle.pressed) {
            draw.spriteAdditive(art, kToggleHighlight);
        }
        result.toggle = toggle.clicked;
    }
    // GameTimeFrame: the sun or the moon (UI-TOD-Indicator: two 50 x 50 halves of a 128 x 64 texture).
    if (view.hasTime) {
        const ui::WidgetState dial = ui.widget(ui::widgetId("minimap.time"), l.time());
        const f32 u0 = view.night ? 0.5f : 0.0f;
        draw.sprite(l.at(146.0f, 19.0f, 50.0f, 50.0f), kTimeOfDay,
                    dial.pressed ? rgba(200, 200, 200) : ui::kWhite,
                    {u0, 0.0f, u0 + 50.0f / 128.0f, 50.0f / 64.0f});
        if (dial.clicked) {
            result.tapped = MinimapTip::Time;
        }
    }
    return result;
}

void drawMinimapTooltip(ui::Context& ui, ui::DrawList& draw, const Rect& anchor,
                        std::span<const MinimapTooltipLine> lines, const Rect& screen) {
    if (lines.empty()) {
        return;
    }
    ui::Gui g{ui, draw};
    const f32 size = ui.px(13.0f);
    const f32 lineHeight = size * 1.25f;
    const f32 pad = ui.px(9.0f);
    f32 width = 0.0f;
    for (const MinimapTooltipLine& line : lines) {
        width = std::max(width, draw.measure(line.text, ui::FontId::Friz, size));
    }
    const f32 w = std::min(width + pad * 2.0f, screen.w);
    const f32 h = lineHeight * static_cast<f32>(lines.size()) + pad * 2.0f - (lineHeight - size);
    // Left of the part, top-aligned with it; inside the screen.
    const f32 x = std::clamp(anchor.x - w - ui.px(2.0f), screen.x, std::max(screen.x, screen.right() - w));
    const f32 y = std::clamp(anchor.y, screen.y, std::max(screen.y, screen.bottom() - h));
    ui::tooltipPanel(g, {x, y, w, h});
    f32 top = y + pad;
    for (const MinimapTooltipLine& line : lines) {
        ui::TextStyle style = ui::gameFont(size, line.color);
        draw.text(x + pad, ui::centeredBaseline(draw, style, top + size * 0.5f), line.text, style);
        top += lineHeight;
    }
}

} // namespace vd
