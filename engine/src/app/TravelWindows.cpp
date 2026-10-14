#include "app/TravelWindows.h"

#include "app/DialogKit.h"
#include "app/LoadingLayout.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using namespace kit;

constexpr u64 kMinimapArrow = assetPathHash("Interface\\Minimap\\MinimapArrow.blp");
// POIIcons: an 8 x 8 grid of 16-pixel icons; the gravestone (the corpse) is the last of the first row.
constexpr u64 kPoiIcons = assetPathHash("Interface\\Minimap\\POIIcons.blp");
constexpr ui::TexCoords kCorpseIcon{7.0f / 8.0f, 0.0f, 1.0f, 1.0f / 8.0f};

constexpr f32 kMapWidth = 1002.0f; // WorldMapDetailFrame
constexpr f32 kMapHeight = 668.0f;
constexpr f32 kMaxZoom = 6.0f;

constexpr u32 kFadeInMs = 500;
constexpr u32 kFadeOutMs = 1500;

f32 zoneAlpha(u32 ageMs) {
    if (ageMs < kFadeInMs) {
        return static_cast<f32>(ageMs) / static_cast<f32>(kFadeInMs);
    }
    if (ageMs + kFadeOutMs > kZoneTextMs) {
        return std::max(0.0f, static_cast<f32>(kZoneTextMs - ageMs) / static_cast<f32>(kFadeOutMs));
    }
    return 1.0f;
}

// Retouches (windows): DrawRouteLine (TaxiFrame.lua): UI-Taxi-Line (its dark core and pale glow along it)
// from a to b as a turned quad `width` pixels wide (the client's 32 units), both scaled by
// TAXIROUTE_LINEFACTOR (32 / 30: the texture's transparent first and last texel columns reach past the ends).
constexpr u64 kTaxiLine = assetPathHash("Interface\\TaxiFrame\\UI-Taxi-Line.blp");
void routeLine(ui::DrawList& draw, glm::vec2 a, glm::vec2 b, f32 width) {
    const glm::vec2 d = b - a;
    const f32 length = glm::length(d);
    const std::optional<ui::Sprite> sprite = draw.assets() ? draw.assets()->sprite(kTaxiLine) : std::nullopt;
    if (length < 0.5f || !sprite) {
        return;
    }
    constexpr f32 kLineFactor = 32.0f / 30.0f;
    const glm::vec2 c = (a + b) * 0.5f;
    const glm::vec2 along = d / length * (length * kLineFactor * 0.5f);
    const glm::vec2 across = glm::vec2(-d.y, d.x) / length * (width * kLineFactor * 0.5f);
    const glm::vec2 p0 = c - along - across, p1 = c + along - across, p2 = c + along + across,
                    p3 = c - along + across;
    const f32 corners[8] = {p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y};
    draw.spriteQuad(corners, *sprite);
}

// The player's arrow (the minimap's), turned to `facing` (0 = north = up on the map).
void arrow(Gui& g, glm::vec2 at, f32 facing, f32 size) {
    const std::optional<ui::Sprite> sprite =
        g.draw.assets() ? g.draw.assets()->sprite(kMinimapArrow) : std::nullopt;
    if (!sprite) {
        g.draw.solid({at.x - size * 0.25f, at.y - size * 0.25f, size * 0.5f, size * 0.5f}, ui::kGold);
        return;
    }
    const f32 half = size * 0.5f;
    const f32 upX = -std::sin(facing), upY = -std::cos(facing);
    const f32 rightX = -upY, rightY = upX;
    const f32 corners[8] = {at.x - rightX * half + upX * half, at.y - rightY * half + upY * half,
                            at.x + rightX * half + upX * half, at.y + rightY * half + upY * half,
                            at.x + rightX * half - upX * half, at.y + rightY * half - upY * half,
                            at.x - rightX * half - upX * half, at.y - rightY * half - upY * half};
    g.draw.spriteQuad(corners, *sprite);
}

} // namespace

// --- Zone text
// --------------------------------------------------------------------------------------------------

void zoneText(DialogContext& c, const Rect& area, const ZoneTextView& v) {
    if (v.ageMs >= kZoneTextMs || (v.zone.empty() && v.subzone.empty())) {
        return;
    }
    Gui g{c.ui, c.draw};
    const f32 alpha = zoneAlpha(v.ageMs);
    const bool wide = c.profile == ScreenProfile::Wide;
    // ZoneTextFrame sits a third of the way down; SubZoneTextFrame under it, PVPInfoTextString under both.
    f32 y = area.y + area.h * (wide ? 0.22f : 0.26f);
    const auto drawLine = [&](std::string_view text, f32 size, u32 color) {
        TextStyle style = ui::gameFont(size, ui::withAlpha(color, alpha));
        style.outline = std::max(1.5f, size / 14.0f);
        style.shadow = 0.0f;
        style.align = Align::Center;
        // Long names shrink to fit the screen.
        const f32 width = g.draw.measure(text, style.font, style.size);
        if (width > area.w * 0.94f) {
            style.size *= area.w * 0.94f / width;
        }
        y += style.size;
        g.draw.text(area.centerX(), y, text, style);
        y += style.size * 0.25f;
    };
    if (!v.zone.empty()) {
        drawLine(v.zone, g.px(wide ? 40.0f : 44.0f), v.color);
    }
    if (!v.subzone.empty()) {
        drawLine(v.subzone, g.px(wide ? 26.0f : 28.0f), v.color);
    }
    if (!v.pvp.empty()) {
        drawLine(v.pvp, g.px(16.0f), v.color);
    }
}

// --- World map
// --------------------------------------------------------------------------------------------------

WorldMapResult worldMapWindow(DialogContext& c, const WorldMapView& v, WorldMapCamera& camera) {
    VD_PROFILE_ZONE("TravelWindows::worldMap");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    WorldMapResult result;
    // Phase 20: the map over the whole screen (WorldMapFrame's FULLSCREEN), with no frame nor title band: its title,
    // the way up and the close button float on it inside the safe area. At its smallest it just covers the screen (no
    // black around it); it opens on the player.
    const Rect viewport{0.0f, 0.0f, c.ui.width(), c.ui.height()};
    const Rect& safe = c.safeArea;
    const f32 fit = std::max(viewport.w / kMapWidth, viewport.h / kMapHeight);
    g.draw.solid(viewport, rgba(0, 0, 0));
    if (camera.fresh) {
        camera.fresh = false;
        if (v.hasPlayer) {
            camera.center = v.player;
        }
    }

    // Fingers on the map: one drags, two pinch; a tap that moved nothing is reported.
    const ui::WidgetId mapId = widgetId("worldMap.map");
    ui::hit(g, mapId, viewport);
    const ui::Touch* fingers[2] = {};
    u32 count = 0;
    for (const ui::Touch& t : c.ui.touches()) {
        if (t.owner == mapId) {
            if (count < 2) {
                fingers[count] = &t;
            }
            ++count;
        }
    }
    f32 scale = fit * camera.zoom;
    const glm::vec2 viewCenter(viewport.centerX(), viewport.centerY());
    const auto toMap = [&](glm::vec2 screen) { return camera.center + (screen - viewCenter) / scale; };
    if (count == 1 && fingers[0]->moved && !fingers[0]->released) {
        camera.center -= glm::vec2(fingers[0]->dx, fingers[0]->dy) / scale;
    } else if (count >= 2) {
        const glm::vec2 a(fingers[0]->x, fingers[0]->y), b(fingers[1]->x, fingers[1]->y);
        const glm::vec2 a0 = a - glm::vec2(fingers[0]->dx, fingers[0]->dy);
        const glm::vec2 b0 = b - glm::vec2(fingers[1]->dx, fingers[1]->dy);
        const f32 before = glm::length(b0 - a0), after = glm::length(b - a);
        // The map point under the old midpoint stays under the new one.
        const glm::vec2 anchor = toMap((a0 + b0) * 0.5f);
        if (before > 1.0f && after > 1.0f) {
            camera.zoom = std::clamp(camera.zoom * after / before, 1.0f, kMaxZoom);
        }
        scale = fit * camera.zoom;
        camera.center = anchor - ((a + b) * 0.5f - viewCenter) / scale;
    } else if (count == 1 && fingers[0]->released && !fingers[0]->moved && !fingers[0]->cancelled &&
               c.ui.nowMs() - fingers[0]->downMs < ui::Context::kLongPressMs) { // phase 15: a held finger only names
        result.tapped = true;
        result.tap = toMap(glm::vec2(fingers[0]->x, fingers[0]->y));
    }
    // Keep the view on the map (centered on it while it is narrower than the view).
    const auto clampCamera = [&]() {
        scale = fit * camera.zoom;
        const f32 halfW = viewport.w * 0.5f / scale, halfH = viewport.h * 0.5f / scale;
        camera.center.x = halfW * 2.0f >= kMapWidth ? kMapWidth * 0.5f
                                                    : std::clamp(camera.center.x, halfW, kMapWidth - halfW);
        camera.center.y = halfH * 2.0f >= kMapHeight ? kMapHeight * 0.5f
                                                     : std::clamp(camera.center.y, halfH, kMapHeight - halfH);
    };
    clampCamera();
    const auto toScreen = [&](glm::vec2 map) { return viewCenter + (map - camera.center) * scale; };

    g.draw.pushClip(viewport);
    for (const MapPiece& piece : v.pieces) {
        if (!piece.texture.valid()) {
            continue;
        }
        const glm::vec2 a = toScreen({piece.rect.x, piece.rect.y});
        const glm::vec2 b = toScreen({piece.rect.right(), piece.rect.bottom()});
        g.draw.texture({a.x, a.y, b.x - a.x, b.y - a.y}, piece.texture, ui::kWhite, piece.uv);
    }
    // Zone names of a continent, where their zone is wide enough on screen to hold them.
    TextStyle labelStyle = ui::gameFont(m.small, ui::kGold);
    labelStyle.outline = std::max(1.0f, g.px(1.2f));
    labelStyle.shadow = 0.0f;
    labelStyle.align = Align::Center;
    for (const MapLabel& label : v.labels) {
        const f32 room = label.width * scale;
        if (label.width > 0.0f && g.draw.measure(label.text, labelStyle.font, labelStyle.size) > room) {
            continue;
        }
        const glm::vec2 at = toScreen(label.at);
        g.draw.text(at.x, ui::centeredBaseline(g.draw, labelStyle, at.y), label.text, labelStyle);
    }
    // Flight points, the corpse, the player.
    const f32 pinSize = g.px(wide ? 14.0f : 16.0f);
    for (const MapPin& pin : v.pins) {
        const glm::vec2 at = toScreen(pin.at);
        const f32 s = pin.current ? pinSize * 1.4f : pinSize;
        if (v.pinTexture.valid()) {
            g.draw.texture({at.x - s * 0.5f, at.y - s * 0.5f, s, s}, v.pinTexture);
        }
    }
    if (v.hasCorpse) {
        const glm::vec2 at = toScreen(v.corpse);
        const f32 s = g.px(22.0f);
        g.draw.sprite({at.x - s * 0.5f, at.y - s * 0.5f, s, s}, kPoiIcons, ui::kWhite, kCorpseIcon);
    }
    // Phase 15: the party or raid (WorldMapParty / WorldMapRaid, under the player's arrow).
    const f32 unitSize = g.px(wide ? 14.0f : 16.0f);
    if (v.unitTexture.valid()) {
        for (const MapUnit& unit : v.units) {
            const glm::vec2 at = toScreen(unit.at);
            g.draw.texture({at.x - unitSize * 0.5f, at.y - unitSize * 0.5f, unitSize, unitSize}, v.unitTexture);
        }
    }
    // Phase 17 (scores): the flag carrier in a battleground (WorldMapFlagTemplate, over the units).
    for (const MapFlag& flag : v.flags) {
        const glm::vec2 at = toScreen(flag.at);
        const f32 s = unitSize * 1.5f;
        g.draw.sprite({at.x - s * 0.5f, at.y - s * 0.5f, s, s}, flag.texture);
    }
    if (v.hasPlayer) {
        arrow(g, toScreen(v.player), v.facing, g.px(30.0f));
    }
    if (v.loading) {
        TextStyle hint = ui::gameFont(m.text, ui::kWhite);
        hint.align = Align::Center;
        g.draw.text(safe.centerX(), safe.centerY(), text::get("VD_TRAVEL_MAP_LOADING"), hint);
    }
    // The coordinates at the bottom right, over the map.
    if (!v.footer.empty()) {
        TextStyle style = ui::gameFont(m.text, ui::kWhite);
        style.outline = std::max(1.0f, g.px(1.2f));
        style.shadow = 0.0f;
        style.align = Align::Right;
        g.draw.text(safe.right() - g.px(10.0f), safe.bottom() - g.px(10.0f), v.footer, style);
    }
    g.draw.popClip();

    // Phase 15: WorldMapUnit_OnEnter while a finger rests on the map: the names under it, one per line, on the side
    // away from the map's middle (the tooltip's ANCHOR_LEFT / ANCHOR_RIGHT).
    if (count == 1 && !fingers[0]->moved && !fingers[0]->released &&
        c.ui.nowMs() - fingers[0]->downMs >= ui::Context::kLongPressMs) {
        const glm::vec2 finger(fingers[0]->x, fingers[0]->y);
        const f32 reach = std::max(unitSize, g.px(18.0f));
        std::array<std::string_view, 8> names{};
        u32 nameCount = 0;
        const auto under = [&](glm::vec2 map) { return glm::length(toScreen(map) - finger) <= reach; };
        if (v.hasPlayer && !v.playerName.empty() && under(v.player)) {
            names[nameCount++] = v.playerName;
        }
        for (const MapUnit& unit : v.units) {
            if (nameCount < names.size() && under(unit.at)) {
                names[nameCount++] = unit.name;
            }
        }
        if (nameCount > 0) {
            TextStyle style = ui::gameFont(m.text, ui::kWhite);
            const f32 line = style.size * 1.25f;
            f32 width = 0.0f;
            for (u32 i = 0; i < nameCount; ++i) {
                width = std::max(width, g.draw.measure(names[i], style.font, style.size));
            }
            const f32 pad = g.px(8.0f);
            const f32 w = width + pad * 2.0f, h = line * static_cast<f32>(nameCount) + pad * 2.0f;
            const f32 x = finger.x > viewport.centerX() ? finger.x - reach - w : finger.x + reach;
            const f32 y = std::clamp(finger.y - h * 0.5f, safe.y, std::max(safe.y, safe.bottom() - h));
            ui::tooltipPanel(g, {x, y, w, h});
            for (u32 i = 0; i < nameCount; ++i) {
                ui::label(g, {x + pad, y + pad + line * static_cast<f32>(i), width, line}, names[i], style);
            }
        }
    }

    // Floating on the map: the title plate (top centre), back to the larger map (top left), the close button (top
    // right); the fingers zoom (no (+) / (-) since phase 20).
    if (!v.title.empty()) {
        ui::dialogHeader(g, {safe.x, safe.y + g.unit(12.0f), safe.w, safe.h}, v.title);
    }
    if (!v.up.empty()) {
        const f32 h = m.button;
        const Rect up{safe.x + g.px(6.0f), safe.y + g.px(6.0f), std::min(buttonWidth(g, v.up, h), safe.w * 0.3f),
                      h};
        result.up = ui::button(g, widgetId("worldMap.up"), up, v.up);
    }
    const f32 closeSize = g.px(32.0f);
    result.closed = ui::closeButton(g, widgetId("worldMap.close"),
                                    {safe.right() - closeSize - g.px(6.0f), safe.y + g.px(6.0f), closeSize, closeSize});
    return result;
}

// --- Taxi map
// ---------------------------------------------------------------------------------------------------

TaxiMapResult taxiMapWindow(DialogContext& c, const TaxiMapView& v) {
    VD_PROFILE_ZONE("TravelWindows::taxiMap");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    TaxiMapResult result;
    const Dialog d = beginDialog(c, g, "taxiMap", wide ? 900.0f : 560.0f, wide ? 520.0f : 820.0f, v.title);
    result.closed = d.closed;

    // The map (square) left on wide screens, on top otherwise; the destinations beside or under it.
    const f32 gap = g.px(8.0f);
    Rect mapRect, listRect;
    if (wide) {
        const f32 side = std::min(d.body.h, d.body.w * 0.55f);
        mapRect = {d.body.x, d.body.y + (d.body.h - side) * 0.5f, side, side};
        listRect = {mapRect.right() + gap, d.body.y, d.body.right() - mapRect.right() - gap, d.body.h};
    } else {
        const f32 side = std::min(d.body.w, d.body.h * 0.58f);
        mapRect = {d.body.centerX() - side * 0.5f, d.body.y, side, side};
        listRect = {d.body.x, mapRect.bottom() + gap, d.body.w, d.body.bottom() - mapRect.bottom() - gap};
    }
    g.draw.solid(mapRect, rgba(0, 0, 0, 255));
    if (v.map.valid()) {
        g.draw.texture(mapRect, v.map);
    }
    const auto onMap = [&](glm::vec2 at) {
        return glm::vec2(mapRect.x + at.x * mapRect.w, mapRect.y + at.y * mapRect.h);
    };
    // The selected route, node to node: DrawRouteLine's 32 units of TaxiRouteMap's 316.
    for (usize i = 1; i < v.route.size(); ++i) {
        routeLine(g.draw, onMap(v.route[i - 1]), onMap(v.route[i]), 32.0f * mapRect.w / 316.0f);
    }
    // The nodes, each a target as large as a finger (the icon itself is small, as in the client).
    const f32 icon = g.px(wide ? 15.0f : 17.0f);
    const f32 target = std::max(icon * 1.6f, g.px(28.0f));
    for (usize i = 0; i < v.nodes.size(); ++i) {
        const TaxiNodeView& n = v.nodes[i];
        const glm::vec2 at = onMap(n.at);
        const Rect hit{at.x - target * 0.5f, at.y - target * 0.5f, target, target};
        if (n.kind != TaxiNodeKind::Current && ui::tapped(ui::hit(g, widgetId("taxiMap.node", i), hit))) {
            result.select = static_cast<i32>(i);
        }
        const f32 s = static_cast<i32>(i) == v.selected ? icon * 1.5f : icon;
        const TextureHandle texture = v.icons[static_cast<usize>(n.kind)];
        if (texture.valid()) {
            g.draw.texture({at.x - s * 0.5f, at.y - s * 0.5f, s, s}, texture);
        } else {
            g.draw.solid({at.x - s * 0.3f, at.y - s * 0.3f, s * 0.6f, s * 0.6f}, ui::kGold);
        }
    }
    if (v.selected >= 0 && static_cast<usize>(v.selected) < v.nodes.size()) {
        // The destination's name over the map, as the client's tooltip.
        const TaxiNodeView& n = v.nodes[static_cast<usize>(v.selected)];
        TextStyle style = ui::gameFont(m.text, ui::kWhite);
        style.outline = std::max(1.0f, g.px(1.0f));
        style.shadow = 0.0f;
        style.align = Align::Center;
        const glm::vec2 at = onMap(n.at);
        const f32 width = g.draw.measure(n.name, style.font, style.size);
        const f32 x = std::clamp(at.x, mapRect.x + width * 0.5f + g.px(4.0f),
                                 mapRect.right() - width * 0.5f - g.px(4.0f));
        const f32 y = at.y - icon > mapRect.y + style.size * 1.5f ? at.y - icon : at.y + icon + style.size;
        g.draw.text(x, y, n.name, style);
    }

    // The destinations: reachable first (cheapest first), then the ones out of reach.
    const ParchmentScroll list = beginParchment(g, widgetId("taxiMap.list"), listRect);
    Flow flow = parchmentFlow(g, list);
    const TextStyle rowStyle = ui::questFont(m.text);
    TextStyle gray = rowStyle;
    gray.color = rgba(110, 100, 90);
    std::array<u16, 128> order{};
    u32 shown = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        for (usize i = 0; i < v.nodes.size() && shown < order.size(); ++i) {
            const TaxiNodeKind kind = v.nodes[i].kind;
            if ((pass == 0 && kind == TaxiNodeKind::Reachable) ||
                (pass == 1 && kind == TaxiNodeKind::Unreachable)) {
                order[shown++] = static_cast<u16>(i);
            }
        }
        if (pass == 0) {
            std::sort(order.begin(), order.begin() + shown,
                      [&](u16 a, u16 b) { return v.nodes[a].cost < v.nodes[b].cost; });
        }
    }
    if (shown == 0) {
        flow.text(text::get("ERR_TAXINOPATHS"), rowStyle, g.px(6.0f));
    }
    for (u32 k = 0; k < shown; ++k) {
        const u32 i = order[k];
        const TaxiNodeView& n = v.nodes[i];
        const Rect row{flow.x - g.px(4.0f), flow.y, flow.w + g.px(8.0f), m.row};
        const ui::WidgetState state = ui::hit(g, widgetId("taxiMap.row", i), row);
        if (static_cast<i32>(i) == v.selected || ui::held(state, row)) {
            g.draw.sprite(row, assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp"));
        }
        if (ui::tapped(state)) {
            result.select = static_cast<i32>(i);
        }
        const bool reachable = n.kind == TaxiNodeKind::Reachable;
        const f32 moneyW = reachable ? ui::moneyWidth(g.draw, n.cost, m.small) + g.px(8.0f) : 0.0f;
        ui::label(g,
                  {row.x + g.px(8.0f), row.y, row.w - moneyW - g.px(12.0f), row.h * (n.hops ? 0.62f : 1.0f)},
                  n.name, reachable ? rowStyle : gray);
        if (n.hops > 0) {
            char hops[32];
            text::format(hops, n.hops == 1 ? "VD_TRAVEL_TAXI_STOP" : "VD_TRAVEL_TAXI_STOPS", n.hops);
            TextStyle small = ui::questFont(m.small, rgba(90, 70, 40));
            ui::label(g, {row.x + g.px(8.0f), row.y + row.h * 0.5f, row.w * 0.6f, row.h * 0.45f}, hops,
                      small);
        }
        if (reachable) {
            ui::money(g, row.right() - g.px(6.0f), row.centerY(), n.cost, m.small, Align::Right);
        }
        flow.y += row.h;
    }
    endParchment(g, list, flow);

    // Fly (when the destination can be reached and paid), or go.
    const bool canFly = v.selected >= 0 && static_cast<usize>(v.selected) < v.nodes.size() &&
                        v.nodes[static_cast<usize>(v.selected)].kind == TaxiNodeKind::Reachable &&
                        v.nodes[static_cast<usize>(v.selected)].cost <= v.money;
    bool fly = false, cancel = false;
    footerButtons(g, d.footer, "taxiMap", text::get("VD_TRAVEL_FLY"), canFly, fly, text::get("CANCEL"),
                  cancel);
    result.fly = fly && canFly;
    result.closed = result.closed || cancel;
    if (v.selected >= 0 && !canFly && static_cast<usize>(v.selected) < v.nodes.size() &&
        v.nodes[static_cast<usize>(v.selected)].kind == TaxiNodeKind::Reachable) {
        TextStyle warn = ui::gameFont(m.small, ui::kRed);
        warn.align = Align::Center;
        g.draw.text(d.footer.centerX(), ui::centeredBaseline(g.draw, warn, d.footer.centerY()),
                    text::get("ERR_TAXINOTENOUGHMONEY"), warn);
    }
    return result;
}

// --- Loading screen
// -----------------------------------------------------------------------------------------------

void loadingScreen(DialogContext& c, const LoadingView& v) {
    Gui g{c.ui, c.draw};
    const Rect screen{0.0f, 0.0f, c.ui.width(), c.ui.height()};
    ui::hit(g, widgetId("loadingScreen"), screen); // nothing reaches the world or the HUD
    g.draw.solid(screen, ui::kBlack);
    // WoW.exe's tip font is 0.018 of the 4:3 height: 13.8 units of a 768-high screen. On a short screen (the
    // Fold5's cover) at least what the HUD makes of those units.
    const LoadingLayout l = loadingLayout(screen, c.safeArea, g.unit(0.018f * 768.0f));
    if (v.art.valid()) {
        g.draw.texture(l.art, v.art);
    }
    // The fill stretched over the part loaded, then the border over it; nothing behind (the art shows through).
    const f32 progress = std::clamp(v.progress, 0.0f, 1.0f);
    if (v.barFill.valid() && progress > 0.0f) {
        g.draw.texture({l.fill.x, l.fill.y, l.fill.w * progress, l.fill.h}, v.barFill);
    }
    if (v.barBorder.valid()) {
        g.draw.texture(l.border, v.barBorder);
    }
    if (!v.tip.empty()) {
        // 0x406640: Friz, light gray at 0.84 (CImVector 0xD7C8C8C8), a black shadow; "Tip:" brings its own gold.
        TextStyle style = ui::gameFont(l.tipSize, rgba(200, 200, 200, 215));
        style.shadow = l.tipShadow;
        const f32 height = g.draw.measureWrapped(l.tip.w, v.tip, style.font, style.size);
        g.draw.wrappedText({l.tip.x, l.tip.bottom() - height, l.tip.w, height}, v.tip, style);
    }
    if (!v.status.empty()) {
        TextStyle style = ui::gameFont(l.tipSize, ui::kGold);
        style.outline = std::max(1.0f, g.px(1.0f));
        style.align = Align::Center;
        g.draw.text(l.border.centerX(), l.border.bottom() + style.size * 1.1f, v.status, style);
    }
}

} // namespace vd
