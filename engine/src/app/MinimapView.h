#pragma once

#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <glm/vec3.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace vd {

class UiResources;

// The MinimapCluster of the 1.12 client as a pure view (parity pass): FrameXML Minimap.xml and GameTime.xml
// laid out by their anchors in the cluster's 192 x 192 units, drawn with their art. The zone bar
// (MinimapBorderTop, MinimapZoneTextButton, MinimapToggleButton), the map (Minimap: tiles, blips, the ping,
// the player's arrow) in its border (MinimapBackdrop) with the zoom buttons, the tracking icon
// (MiniMapTrackingFrame), the letter (MiniMapMailFrame) and the day / night dial (GameTimeFrame). Hover does
// not exist on a phone: a tap on a part reports it (MinimapResult::tapped) and the glue (app/MinimapScreens)
// shows its tooltip with drawMinimapTooltip(); a tap on the map pings, two fingers on it zoom. No game state
// here: a MinimapView in, a MinimapResult out.

// Where the parts are, in logical pixels. The cluster's top-right sits at the safe area's top-right, 4 dp in
// and 2 dp down (where the HUD put it in phase 7; other HUD parts anchor to its map: app/Hud's menu, the
// backpack button).
struct MinimapLayout {
    f32 s = 1.0f;                // logical pixels per interface unit
    f32 left = 0.0f, top = 0.0f; // the cluster's top-left
    f32 cx = 0.0f, cy = 0.0f;    // the map's centre
    f32 radius = 0.0f;           // the map's radius (70 units)

    // A rectangle of the cluster in its units (x right, y down from its top-left).
    ui::Rect at(f32 x, f32 y, f32 w, f32 h) const { return {left + x * s, top + y * s, w * s, h * s}; }
    ui::Rect zoneButton() const; // the bar's plate (MinimapZoneTextButton is 128 x 12 on it)
    ui::Rect toggle() const;     // MinimapToggleButton, the bar's right end
    ui::Rect time() const;       // GameTimeFrame (its hit rectangle)
    ui::Rect zoomIn() const;     // touch targets of MinimapZoomIn / MinimapZoomOut
    ui::Rect zoomOut() const;
    ui::Rect tracking() const; // MiniMapTrackingFrame
    ui::Rect mail() const;     // MiniMapMailFrame
    ui::Rect meetingStone() const; // phase 16 (alerts): MiniMapMeetingStoneFrame
    ui::Rect map() const { return {cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}; }
};
// `unitPixels`: logical pixels per interface unit (ui::kUnitDp dp); `dp`: logical pixels per dp.
MinimapLayout minimapLayout(const ui::Rect& safe, f32 unitPixels, f32 dp);

// A round button on the map's rim like MiniMapBattlefieldFrame (app/BattlefieldWindows): an icon cropped round under
// MiniMap-TrackingBorder (52 units, its ring's centre 16 units in), the ring 77.9 units from the map's centre at
// `degrees` counterclockwise from its right (180: left of it, 270: under it). The rectangle is its touch target, the
// battlefield button's 41 units around the ring.
ui::Rect minimapRimButtonRect(const MinimapLayout& l, f32 degrees);
// Draws it with the icon cache's `icon` (none: the ring alone), darker while pressed, lit by the zoom buttons'
// highlight while `lit`; returns its touch state.
ui::WidgetState minimapRimButton(ui::Context& ui, ui::DrawList& draw, ui::WidgetId id, const MinimapLayout& l,
                                 f32 degrees, std::optional<u32> icon, bool lit = false);

// One blip: its offset from the map's centre in radii (game::minimapPoint), an ObjectIcons.blp cell or an
// arrow; phase 13: the gossip point's POIIcons.blp cell, or its guide arrow beyond the map.
enum class MinimapBlipArt : u8 { Dot, GroupArrow, CorpseArrow, Poi, GuideArrow };
struct MinimapBlipView {
    f32 x = 0.0f, y = 0.0f; // radii, x right, y down
    f32 angle = 0.0f;       // arrows: where they point, radians clockwise from up
    MinimapBlipArt art = MinimapBlipArt::Dot;
    u8 cell = 0; // dots: the ObjectIcons.blp cell (4 per row); Poi: the POIIcons.blp cell (8 per row)
    u32 tint = ui::kWhite;
};

// The parts that show a tooltip when tapped.
enum class MinimapTip : u8 { None, Zone, Time, Tracking, Mail, Blips, MeetingStone /* phase 16 (alerts) */ };
struct MinimapTooltipLine {
    std::string_view text;
    u32 color = ui::kWhite;
};

// Phase 15: a tile of the building the player is inside (its indoor minimap): its texture over four world points
// (x, y), clockwise from its texture's top-left corner.
struct MinimapInteriorTile {
    TextureHandle texture;
    f64 corners[8]{};
};

struct MinimapView {
    // The map.
    std::string_view map;     // Map.dbc directory of the tiles (empty: none)
    // Phase 15: indoors, the building's own map instead of the terrain's (over black, as the client).
    bool interior = false;
    std::span<const MinimapInteriorTile> interiorTiles;
    glm::dvec3 position{0.0}; // the centre: the player
    f32 facing = 0.0f;        // the player's arrow, radians, 0 = north
    f32 radiusYards = 133.0f;
    bool shown = true; // MinimapToggleButton: false hides the map with its border and buttons
    std::span<const MinimapBlipView> blips;
    f32 pingAlpha = 0.0f;           // 0: no ping
    f32 pingX = 0.0f, pingY = 0.0f; // radii
    f32 pingSeconds = 0.0f;         // its age (the animation)
    // The zone bar.
    std::string_view zoneName;
    u32 zoneColor = ui::kGold;
    // Buttons and frames.
    bool canZoomIn = true;
    bool canZoomOut = true;
    bool hasTime = false;
    bool night = false;   // the dial's moon
    u64 trackingIcon = 0; // icon texture of the tracking spell (0: no tracking)
    bool newMail = false;
    bool meetingStone = false; // phase 16 (alerts): MiniMapMeetingStoneFrame (IsInMeetingStoneQueue)
};

struct MinimapResult {
    i32 zoom = 0;                         // levels: > 0 in, < 0 out (the buttons, two fingers on the map)
    bool toggle = false;                  // MinimapToggleButton tapped
    MinimapTip tapped = MinimapTip::None; // a part with a tooltip was tapped
    bool tappedMap = false;               // a tap on the map (ping, the names of its blips)
    f32 tapX = 0.0f, tapY = 0.0f;         // where, radii from the centre
    // A long press on the tracking icon (the client's right click: CancelTrackingBuff).
    bool cancelTracking = false;
    // Phase 16 (alerts): MiniMapMeetingStoneFrame tapped (its OnClick: CONFIRM_LEAVE_QUEUE). Its tooltip
    // (OnEnter, hover) is a long press here: `tapped` = MeetingStone.
    bool meetingStoneClicked = false;
};

// The map's gesture between frames (the HUD keeps it): two fingers pinching.
struct MinimapGesture {
    f32 pinchDistance = 0.0f; // dp, 0 when not pinching
    f32 pinchCarry = 0.0f;    // dp of pinch not yet turned into zoom levels
    bool pinched = false;     // the fingers on the map pinched: their release is no tap
};

// The cluster, drawn on `draw` with its widgets on `ui`. No allocation.
MinimapResult drawMinimap(ui::Context& ui, ui::DrawList& draw, UiResources& resources,
                          const MinimapLayout& layout, const MinimapView& view, MinimapGesture& gesture);
// A tooltip (the GameTooltip backdrop, one line each) beside `anchor`: left of it, top-aligned (Minimap.xml's
// ANCHOR_LEFT and ANCHOR_BOTTOMLEFT), kept inside `screen`.
void drawMinimapTooltip(ui::Context& ui, ui::DrawList& draw, const ui::Rect& anchor,
                        std::span<const MinimapTooltipLine> lines, const ui::Rect& screen);

} // namespace vd
