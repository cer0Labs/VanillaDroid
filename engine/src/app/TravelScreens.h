#pragma once

#include "app/Dialogs.h"
#include "app/Hud.h"
#include "app/TravelWindows.h"
#include "core/Types.h"
#include "game/AreaData.h"
#include "game/TaxiData.h"
#include "game/Travel.h"
#include "game/WorldMapData.h"

#include <glm/mat4x4.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd {

class GameSession;
class UiResources;

// The travel side of the touch UI (phase 8): fills the travel windows (app/TravelWindows) from the session's
// game::Travel and applies what the player does there: the zone text and the minimap's zone name, exploration
// and flight messages, the taxi map, the innkeeper's confirmation, the world map and the loading screen.
// Owned by GameScreens (which calls it from its frame); Game draws the loading screen through it. Main
// thread; views are reserved once.
class TravelScreens {
public:
    void init(const game::AreaData* areas, const game::WorldMapData* maps, const game::TaxiData* taxi,
              UiResources* resources);

    // Before the HUD draws: the zone name over the minimap; remembers where the player is and how the camera
    // looks (the world map, the marks over flight masters).
    void prepareHud(GameSession& session, HudFrame& hud);
    // A line for the UIErrorsFrame-style messages of GameScreens.
    using Say = void (*)(void* context, u32 color, bool big, const char* text);
    // In the world, after the HUD and before the windows: travel messages (through `say`), the zone text, the
    // marks over flight masters whose path is new, and the gossip of a flight master that only flies.
    void drawOverlay(GameSession& session, DialogContext& c, u32 nowMs, Say say, void* sayContext);
    // Without a server (development): the zone text works too.
    void drawOffline(GameSession& session, DialogContext& c, u32 nowMs);

    // The travel window that takes the screen: the innkeeper's question. Parity pass: the taxi map and the world map
    // are panels (TaxiFrame, WorldMapFrame), drawn by drawTaxiPanel / drawWorldMapPanel where app/UiPanels says.
    bool wantsModal(const GameSession& session) const;
    void drawModal(GameSession& session, DialogContext& c, u32 nowMs);
    void openWorldMap(const GameSession& session);
    void closeWorldMap() { m_worldMapOpen = false; }
    bool worldMapOpen() const { return m_worldMapOpen; }
    bool taxiOpen(const GameSession& session) const;
    void drawTaxiPanel(GameSession& session, DialogContext& c) { taxiWindow(session, c); }
    void drawWorldMapPanel(GameSession& session, DialogContext& c) { worldMapWindow(session, c); }

    // The loading screen of `map` (Map.dbc id): its art, the bar at `progress` (0..1) and, over it, a tip;
    // `status`: a line under the bar when something is wrong. Drawn again after a pause it is a new loading screen,
    // with the next tip.
    void drawLoading(DialogContext& c, u32 map, f32 progress, std::string_view status = {});
    // Phase 20 (retouches): the next tip now (the gallery's page, each time its bar starts again).
    void nextLoadingTip();
    // Phase 20 (retouches): the options' showGameTips and the gameTip counter (nullptr: tips shown, counted here).
    void setSettings(SettingsView* settings) { m_settings = settings; }

    // What the world map shows besides its art.
    struct MapSubject {
        bool hasPlayer = false;
        u32 map = 0; // Map.dbc
        glm::dvec3 position{0.0};
        f32 facing = 0.0f;
        bool hasCorpse = false;
        u32 corpseMap = 0;
        f32 corpseX = 0.0f, corpseY = 0.0f;
        const game::Travel* travel = nullptr; // the explored areas (nullptr: every overlay shows)
        u32 currentTaxiNode = 0;
        // Phase 15: the player's name and the party / raid members where the server last put them.
        std::string_view playerName;
        struct Member {
            std::string_view name;
            u32 map = 0;
            f32 x = 0.0f, y = 0.0f;
        };
        std::span<const Member> members;
        // Phase 17 (scores): GetBattlefieldFlagPosition (world coordinates of the battleground's map).
        struct Flag {
            u32 map = 0;
            f32 x = 0.0f, y = 0.0f;
            u64 texture = 0;
        };
        std::span<const Flag> flags;
    };
    // Development (UiGallery): the windows with sample state instead of a session's.
    void previewWorldMap(DialogContext& c, u32 mapArea, const MapSubject& subject);
    void previewTaxiMap(DialogContext& c, u32 currentNode, const game::TaxiMask& known, u32 money);

private:
    struct TaxiState {
        u32 currentNode = 0;
        game::TaxiMask known{};
        u32 money = 0;
        u64 flightMaster = 0;
    };

    void readEvents(GameSession& session, Say say, void* sayContext);
    void drawZoneText(GameSession& session, DialogContext& c, u32 nowMs);
    void taxiWindow(GameSession& session, DialogContext& c);
    // The taxi map of `state`; the route to the selected destination in `route` (count 0: none).
    TaxiMapResult drawTaxi(DialogContext& c, const TaxiState& state, game::TaxiRoute& route);
    void worldMapWindow(GameSession& session, DialogContext& c);
    void drawWorldMap(DialogContext& c, const MapSubject& subject);
    // The pieces, labels and pins of a WorldMapArea.
    void buildMap(const game::WorldMapArea& area, const MapSubject& subject);
    const game::WorldMapArea* defaultMap(const GameSession& session) const;

    const game::AreaData* m_areas = nullptr;
    const game::WorldMapData* m_maps = nullptr;
    const game::TaxiData* m_taxi = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false; // VD_TRACE_GAMEPLAY

    // The frame's player and camera (prepareHud).
    glm::dvec3 m_position{0.0};
    f32 m_facing = 0.0f;
    u32 m_mapId = 0; // Map.dbc id of the map shown
    bool m_hasMap = false;
    glm::mat4 m_viewProj{1.0f};
    glm::dvec3 m_camera{0.0};
    std::span<const Units::UnitView> m_units;

    // Zone text.
    u32 m_zoneChanges = 0;
    u32 m_zoneShownMs = 0;
    bool m_zoneShowZone = false;
    bool m_zoneTextLive = false;
    std::array<char, 64> m_pvpLine{};

    // Events.
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0;
    std::array<char, 160> m_line{};

    // Taxi map.
    u64 m_taxiFlightMaster = 0;
    u32 m_taxiSelected = 0;  // node id
    u64 m_autoGossipNpc = 0; // a lone flight option was chosen for this NPC's menu
    std::vector<TaxiNodeView> m_taxiNodes;
    std::vector<u32> m_taxiNodeIds;
    std::vector<glm::vec2> m_taxiRoute;
    game::TaxiMask m_knownNodes{}; // the last taxi map's, for the world map's pins

    // World map.
    bool m_worldMapOpen = false;
    u32 m_worldMapArea = 0; // WorldMapArea id shown (0: none: the player's map has none)
    u32 m_previewArea = 0;  // the gallery's
    WorldMapCamera m_camera2d;
    std::vector<MapPiece> m_pieces;
    std::vector<MapLabel> m_labels;
    std::vector<MapPin> m_pins;
    std::vector<MapUnit> m_mapUnits;              // phase 15: the party on the map shown
    std::vector<MapSubject::Member> m_members;    // phase 15: this frame's members (worldMapWindow)
    std::array<MapSubject::Flag, 1> m_flagSubjects{}; // phase 17 (scores): the battleground's flag carrier
    std::array<MapFlag, 1> m_mapFlags{};
    bool m_piecesLoading = false;
    std::array<char, 64> m_footer{};

    // Loading screen (phase 20, retouches: its tip).
    SettingsView* m_settings = nullptr;
    u32 m_ownTip = 0; // the gameTip counter without settings
    std::string_view m_tip;
    bool m_loadingDrawn = false;
    u64 m_loadingDrawnMs = 0;
};

} // namespace vd
