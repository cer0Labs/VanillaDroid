#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "ui/DrawList.h"

#include <glm/vec2.hpp>

#include <array>
#include <span>
#include <string_view>

namespace vd {

// The travel windows (phase 8): the zone text, the world map, the taxi map and the loading screen. Pure views
// like app/Dialogs: a view-model in (textures already resolved by the caller through UiResources::texture),
// what the player did out; no game state, no network. World map art is the client's own (Interface\WorldMap,
// Interface\TaxiFrame, Interface\Glues\LoadingScreens), streamed from textures.vdpk, not in the UI atlas.

// --- Zone text (ZoneTextFrame / SubZoneTextFrame / PVPInfoTextString)
// ---------------------------------------

struct ZoneTextView {
    std::string_view zone;    // big, on a zone change (empty: a subzone change only)
    std::string_view subzone; // smaller, under it
    std::string_view pvp;     // "Alliance Territory", under both (may be empty)
    u32 color = ui::kGold;    // the PvP color of the place
    u32 ageMs = 0;            // since the change
};
constexpr u32 kZoneTextMs = 4500; // fade in 0.5 s, hold 2.5 s, fade out 1.5 s
// Centered in the upper third of `area`; nothing once ageMs >= kZoneTextMs.
void zoneText(DialogContext& c, const ui::Rect& area, const ZoneTextView& v);

// --- World map (WorldMapFrame)
// --------------------------------------------------------------------------------

// A piece of map art in the 1002 x 668 pixels of a map (tiles, explored overlays).
struct MapPiece {
    TextureHandle texture;
    ui::Rect rect;      // map pixels
    ui::TexCoords uv{}; // the part of the texture it shows
};
struct MapLabel {
    std::string_view text;
    glm::vec2 at{0.0f}; // map pixels (centered there)
    f32 width = 0.0f;   // map pixels of the place it names: hidden while the text is wider (0: always shown)
};
struct MapPin {
    glm::vec2 at{0.0f};   // map pixels
    bool current = false; // highlighted (the flight master the taxi map was opened at)
};
// Phase 15: a party or raid member on the map (WorldMapParty1-4 / WorldMapRaid1-40: WorldMapPartyIcon, 16 pixels).
struct MapUnit {
    glm::vec2 at{0.0f}; // map pixels
    std::string_view name;
};
// Phase 17 (scores): a battleground's flag on its map (WorldMapFlag1-2: Interface\WorldStateFrame\<token>, 24 units).
struct MapFlag {
    glm::vec2 at{0.0f}; // map pixels
    u64 texture = 0;
};
struct WorldMapView {
    std::string_view title;
    std::span<const MapPiece> pieces;
    std::span<const MapLabel> labels; // zone names on a continent
    std::span<const MapPin> pins;     // known flight points
    TextureHandle pinTexture;         // UI-Taxi-Icon-Green
    // Phase 15: the party (or raid) members on this map, and the player's name for WorldMapUnit_OnEnter's list.
    std::span<const MapUnit> units;
    TextureHandle unitTexture; // WorldMapPartyIcon
    std::string_view playerName;
    std::span<const MapFlag> flags; // phase 17 (scores): GetBattlefieldFlagPosition
    bool hasPlayer = false;
    glm::vec2 player{0.0f}; // map pixels
    f32 facing = 0.0f;      // radians, 0 = north (+X), counter-clockwise (towards +Y, west)
    bool hasCorpse = false;
    glm::vec2 corpse{0.0f};
    std::string_view up;     // the button back to the larger map ("Eastern Kingdoms"), empty: none
    std::string_view footer; // on the map's corner: coordinates, or why there is no map
    bool loading = false;    // art still on its way (a hint is shown)
};
// Zoom and pan of the map between frames, kept by the caller (reset() when another map is shown).
struct WorldMapCamera {
    f32 zoom = 1.0f;                  // 1: the map just covers the screen
    glm::vec2 center{501.0f, 334.0f}; // map pixels in the middle of the view
    bool fresh = true;                // just reset: the next frame centres it on the player (when on this map)
    void reset() {
        zoom = 1.0f;
        center = {501.0f, 334.0f};
        fresh = true;
    }
};
struct WorldMapResult {
    bool closed = false;
    bool up = false;     // the larger map
    bool tapped = false; // a tap on the map (not a drag or a pinch)
    glm::vec2 tap{0.0f}; // where, map pixels
};
// Modal, over the whole screen (phase 20). One finger drags the map, two pinch it; a tap is reported. Phase 15: a
// finger held still on the map lists the names of the player and the members under it (WorldMapUnit_OnEnter's
// tooltip: the desktop's mouse over).
WorldMapResult worldMapWindow(DialogContext& c, const WorldMapView& v, WorldMapCamera& camera);

// --- Taxi map (TaxiFrame)
// -----------------------------------------------------------------------------------

enum class TaxiNodeKind : u8 { Current, Reachable, Unreachable };
struct TaxiNodeView {
    std::string_view name;
    glm::vec2 at{0.0f}; // 0..1 across the taxi map
    TaxiNodeKind kind = TaxiNodeKind::Reachable;
    u32 cost = 0; // copper (reachable ones)
    u32 hops = 0; // stops on the way (0: direct)
};
struct TaxiMapView {
    std::string_view title;
    TextureHandle map;                    // Interface\TaxiFrame\TAXIMAP<map>.blp
    std::array<TextureHandle, 3> icons{}; // by TaxiNodeKind: green, white, gray
    std::span<const TaxiNodeView> nodes;  // the current one included
    std::span<const glm::vec2> route;     // the selected destination's way, 0..1 (start first)
    i32 selected = -1;                    // into nodes
    u32 money = 0;                        // the player's, copper
};
struct TaxiMapResult {
    i32 select = -1; // a node tapped (on the map or the list)
    bool fly = false;
    bool closed = false;
};
TaxiMapResult taxiMapWindow(DialogContext& c, const TaxiMapView& v);

// --- Loading screen
// -------------------------------------------------------------------------------------------

struct LoadingView {
    TextureHandle art;       // LoadingScreens.dbc of the map (black while it streams in)
    TextureHandle barBorder; // Interface\Glues\LoadingBar\Loading-BarBorder.blp
    TextureHandle barFill;   // Loading-BarFill.blp
    f32 progress = 0.0f;     // 0..1
    std::string_view tip;    // phase 20 (retouches): GameTips.dbc's line over the bar (empty: none)
    std::string_view status; // under the bar, only when something is wrong (the 1.12 has nothing there)
};
// The whole screen (it takes every finger), laid out as WoW.exe's (app/LoadingLayout).
void loadingScreen(DialogContext& c, const LoadingView& v);

} // namespace vd
