#include "app/TravelScreens.h"

#include "app/GameSession.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vd {
namespace {

namespace uf = net::uf;
using game::GameEventKind;
using ui::rgba;

constexpr u64 kTaxiIcons[] = {assetPathHash("Interface\\TaxiFrame\\UI-Taxi-Icon-Green.blp"), // current
                              assetPathHash("Interface\\TaxiFrame\\UI-Taxi-Icon-White.blp"), // reachable
                              assetPathHash("Interface\\TaxiFrame\\UI-Taxi-Icon-Gray.blp")}; // out of reach
constexpr u64 kTaxiMaps[] = {assetPathHash("Interface\\TaxiFrame\\TAXIMAP0.blp"),
                             assetPathHash("Interface\\TaxiFrame\\TAXIMAP1.blp")};
constexpr u64 kLoadingBarBorder = assetPathHash("Interface\\Glues\\LoadingBar\\Loading-BarBorder.blp");
constexpr u64 kLoadingBarFill = assetPathHash("Interface\\Glues\\LoadingBar\\Loading-BarFill.blp");
// Phase 20 (retouches): a loading screen not drawn for this long is over; the next one gets the next tip.
constexpr u64 kLoadingPauseMs = 2000;
constexpr u32 kTaxiGossipIcon = 2; // GOSSIP_ICON_TAXI
constexpr u64 kPartyIcon = assetPathHash("Interface\\WorldMap\\WorldMapPartyIcon.blp"); // phase 15

constexpr u32 kInfoColor = rgba(255, 255, 0); // UIErrorsFrame.lua UI_INFO_MESSAGE
constexpr u32 kErrorColor = rgba(255, 26, 26);

// The colors of ZoneText.lua / Minimap.lua by PvP status (normal: the zone text's pale gold, the minimap's
// gold).
u32 pvpColor(game::ZonePvp pvp, bool minimap) {
    switch (pvp) {
    case game::ZonePvp::Friendly: return rgba(26, 255, 26);
    case game::ZonePvp::Hostile:
    case game::ZonePvp::Arena: return rgba(255, 26, 26);
    case game::ZonePvp::Contested: return rgba(255, 179, 0);
    case game::ZonePvp::Sanctuary: return rgba(105, 204, 240);
    default: return minimap ? ui::kGold : rgba(255, 237, 194);
    }
}

// Next power of two at least `n` pixels (at least 16): the size of an overlay piece's file
// (WorldMapFrame.lua).
f32 fileSize(u32 n) {
    u32 size = 16;
    while (size < n) {
        size *= 2;
    }
    return static_cast<f32>(size);
}

} // namespace

void TravelScreens::init(const game::AreaData* areas, const game::WorldMapData* maps,
                         const game::TaxiData* taxi, UiResources* resources) {
    m_areas = areas;
    m_maps = maps;
    m_taxi = taxi;
    m_resources = resources;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_taxiNodes.reserve(128);
    m_taxiNodeIds.reserve(128);
    m_taxiRoute.reserve(16);
    m_pieces.reserve(256);
    m_labels.reserve(64);
    m_pins.reserve(128);
}

// --- HUD
// --------------------------------------------------------------------------------------------------------

void TravelScreens::prepareHud(GameSession& session, HudFrame& hud) {
    m_position = hud.position;
    m_facing = hud.facing;
    m_viewProj = hud.viewProj;
    m_camera = hud.camera;
    m_units = hud.units;
    const game::Travel& travel = session.play().travel();
    const game::ZoneState& zone = travel.zone();
    m_hasMap = zone.map != 0 || zone.area != 0;
    m_mapId = zone.map;
    // MinimapZoneText: the subzone's name when there is one.
    hud.zoneName = !zone.subzoneName.empty() ? zone.subzoneName : zone.zoneName;
    hud.zoneColor = pvpColor(zone.pvp, true);
}

// --- Overlay: messages, zone text, flight masters
// ---------------------------------------------------------------

void TravelScreens::readEvents(GameSession& session, Say say, void* sayContext) {
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        m_eventCursor = play.events().end();
    }
    // Lines are formatted into m_line (NUL-terminated) for `say`.
    const auto line = [&](bool big, text::Key key, const auto&... args) {
        say(sayContext, kInfoColor, big, text::format(m_line, key, args...).data());
    };
    const auto error = [&](std::string_view message) {
        if (!message.empty()) {
            const text::Arg arg(message);
            say(sayContext, kErrorColor, false, text::vformat(m_line, "%s", {&arg, 1}).data());
        }
    };
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        switch (e->kind) {
        case GameEventKind::AreaExplored: {
            const game::AreaInfo* area = m_areas ? m_areas->area(e->id) : nullptr;
            const std::string_view name = area ? area->name : text::get("VD_TRAVEL_NEW_AREA");
            if (e->value[0] > 0) {
                line(false, "ERR_ZONE_EXPLORED_XP", name, e->value[0]);
            } else {
                line(false, "ERR_ZONE_EXPLORED", name);
            }
            break;
        }
        case GameEventKind::TaxiPathLearned: line(false, "ERR_NEWTAXIPATH"); break;
        case GameEventKind::TaxiReply: error(game::taxiReplyText(static_cast<u32>(e->value[0]))); break;
        case GameEventKind::PlayerBound: {
            const game::AreaInfo* area = m_areas ? m_areas->area(e->id) : nullptr;
            line(false, "ERR_DEATHBIND_SUCCESS_S", area ? area->name : text::get("VD_TRAVEL_THIS_PLACE"));
            break;
        }
        case GameEventKind::TransferAborted:
            error(game::transferAbortText(static_cast<u8>(e->value[0])));
            break;
        default: break;
        }
        if (m_trace) {
            switch (e->kind) {
            case GameEventKind::ZoneChanged:
            case GameEventKind::AreaExplored:
            case GameEventKind::AreaTriggerSent:
            case GameEventKind::TaxiPathLearned:
            case GameEventKind::TaxiMapOpened:
            case GameEventKind::TaxiReply:
            case GameEventKind::BinderConfirm:
            case GameEventKind::PlayerBound:
            case GameEventKind::TransferAborted:
                VD_LOG_INFO("Travel event %u: id %u guid %llx values %d %d", static_cast<u32>(e->kind), e->id,
                            static_cast<unsigned long long>(e->guid), e->value[0], e->value[1]);
                break;
            default: break;
            }
        }
    }
}

void TravelScreens::drawZoneText(GameSession& session, DialogContext& c, u32 nowMs) {
    const game::Travel& travel = session.play().travel();
    const game::ZoneState& zone = travel.zone();
    if (zone.changes != m_zoneChanges) {
        m_zoneChanges = zone.changes;
        m_zoneShownMs = nowMs;
        m_zoneShowZone = zone.zoneChanged;
        m_zoneTextLive = zone.area != 0;
    }
    if (!m_zoneTextLive || nowMs - m_zoneShownMs >= kZoneTextMs) {
        m_zoneTextLive = false;
        return;
    }
    ZoneTextView view;
    // A new zone shows its name big and the subzone under it; a new subzone shows alone.
    view.zone = m_zoneShowZone ? zone.zoneName : std::string_view{};
    view.subzone = zone.subzoneName.empty() && !m_zoneShowZone ? zone.zoneName : zone.subzoneName;
    view.pvp = game::zonePvpText(m_pvpLine, zone.pvp, travel.playerTeam());
    view.color = pvpColor(zone.pvp, false);
    view.ageMs = nowMs - m_zoneShownMs;
    zoneText(c, c.safeArea, view);
}

void TravelScreens::drawOverlay(GameSession& session, DialogContext& c, u32 nowMs, Say say,
                                void* sayContext) {
    VD_PROFILE_ZONE("TravelScreens::overlay");
    game::Gameplay& play = session.play();
    game::Travel& travel = play.travel();
    readEvents(session, say, sayContext);
    // A flight master that only flies: its option at once (GossipFrame.lua selects a lone non-gossip option).
    const game::NpcDialog& dialog = play.dialog();
    if (dialog.kind() == game::DialogKind::Gossip && dialog.quests().empty() &&
        dialog.options().size() == 1 && dialog.options()[0].icon == kTaxiGossipIcon &&
        dialog.npc() != m_autoGossipNpc) {
        m_autoGossipNpc = dialog.npc();
        play.selectGossipOption(dialog.options()[0].index);
    }
    if (!dialog.open()) {
        m_autoGossipNpc = 0;
    }
    // The taxi map replaces the conversation that opened it.
    if (travel.taxiMap().open && dialog.open()) {
        play.closeDialog();
    }
    drawZoneText(session, c, nowMs); // the green "!" over flight masters is a model (app/QuestMarks)
}

void TravelScreens::drawOffline(GameSession& session, DialogContext& c, u32 nowMs) {
    drawZoneText(session, c, nowMs); // parity pass: the world map is a panel (VD_HUD_DEMO: app/PanelDemo)
}

// --- Modal windows
// ------------------------------------------------------------------------------------------------

bool TravelScreens::wantsModal(const GameSession& session) const {
    (void)session; // parity: CONFIRM_BINDER is a popup (app/PopupScreens), the taxi and world maps panels
    return false;
}

bool TravelScreens::taxiOpen(const GameSession& session) const {
    return session.play().travel().taxiMap().open;
}

void TravelScreens::drawModal(GameSession& session, DialogContext& c, u32 nowMs) {
    (void)nowMs;
    (void)session;
    (void)c; // parity: nothing modal is left here (see wantsModal)
}

// --- Taxi map
// -------------------------------------------------------------------------------------------------------

void TravelScreens::taxiWindow(GameSession& session, DialogContext& c) {
    game::Travel& travel = session.play().travel();
    const game::TaxiMapState& map = travel.taxiMap();
    TaxiState state;
    state.currentNode = map.currentNode;
    state.known = map.known;
    state.flightMaster = map.flightMaster;
    if (const game::Entity* me = session.entities().find(session.playerGuid())) {
        state.money = me->value(uf::PLAYER_FIELD_COINAGE);
    }
    m_knownNodes = map.known; // the world map's pins from now on
    game::TaxiRoute route;
    const TaxiMapResult r = drawTaxi(c, state, route);
    if (r.fly && route.count >= 2 && travel.takeFlight(route)) {
        VD_LOG_INFO("Taxi: %u -> %u, %u nodes, %u copper", route.nodes[0], route.nodes[route.count - 1],
                    route.count, route.cost);
    }
    if (r.closed || m_taxiNodes.empty()) {
        travel.closeTaxiMap(); // closed, or nothing to show (no data for its node)
    }
}

void TravelScreens::previewTaxiMap(DialogContext& c, u32 currentNode, const game::TaxiMask& known,
                                   u32 money) {
    TaxiState state;
    state.currentNode = currentNode;
    state.known = known;
    state.money = money;
    state.flightMaster = 1;
    game::TaxiRoute route;
    const TaxiMapResult r = drawTaxi(c, state, route);
    if (r.fly && route.count >= 2) {
        VD_LOG_INFO("UI gallery: fly %u -> %u (%u nodes, %u copper)", route.nodes[0],
                    route.nodes[route.count - 1], route.count, route.cost);
    }
}

TaxiMapResult TravelScreens::drawTaxi(DialogContext& c, const TaxiState& state,
                                      game::TaxiRoute& selectedRoute) {
    selectedRoute = game::TaxiRoute{};
    m_taxiNodes.clear();
    m_taxiNodeIds.clear();
    m_taxiRoute.clear();
    const game::TaxiNodeInfo* current = m_taxi ? m_taxi->node(state.currentNode) : nullptr;
    const game::WorldMapContinent* continent = current && m_maps ? m_maps->continent(current->map) : nullptr;
    if (!current || !continent) {
        return {};
    }
    if (m_taxiFlightMaster != state.flightMaster) {
        m_taxiFlightMaster = state.flightMaster;
        m_taxiSelected = 0;
    }
    game::TaxiMask known = state.known;
    known[state.currentNode / 32] |= 1u << (state.currentNode % 32);
    i32 selected = -1;
    for (const game::TaxiNodeInfo& node : m_taxi->nodes()) {
        if (node.map != current->map || !game::taxiNodeKnown(known, node.id)) {
            continue;
        }
        TaxiNodeView view;
        view.name = node.name;
        view.at = continent->toTaxiMap(node.x, node.y);
        game::TaxiRoute route;
        if (node.id == current->id) {
            view.kind = TaxiNodeKind::Current;
        } else if (m_taxi->route(current->id, node.id, known, route)) {
            view.kind = TaxiNodeKind::Reachable;
            view.cost = route.cost;
            view.hops = route.count - 2;
        } else {
            view.kind = TaxiNodeKind::Unreachable;
        }
        if (node.id == m_taxiSelected) {
            selected = static_cast<i32>(m_taxiNodes.size());
            selectedRoute = route;
        }
        m_taxiNodes.push_back(view);
        m_taxiNodeIds.push_back(node.id);
    }
    for (const u32 id : selectedRoute.list()) {
        if (const game::TaxiNodeInfo* n = m_taxi->node(id)) {
            m_taxiRoute.push_back(continent->toTaxiMap(n->x, n->y));
        }
    }
    TaxiMapView view;
    view.title = current->name;
    view.map =
        current->map < std::size(kTaxiMaps) ? m_resources->texture(kTaxiMaps[current->map]) : TextureHandle{};
    for (u32 i = 0; i < 3; ++i) {
        view.icons[i] = m_resources->texture(kTaxiIcons[i]);
    }
    view.nodes = m_taxiNodes;
    view.route = m_taxiRoute;
    view.selected = selected;
    view.money = state.money;
    const TaxiMapResult r = taxiMapWindow(c, view);
    if (r.select >= 0 && static_cast<usize>(r.select) < m_taxiNodeIds.size()) {
        m_taxiSelected = m_taxiNodeIds[static_cast<usize>(r.select)];
    }
    return r;
}

// --- World map
// ------------------------------------------------------------------------------------------------------

const game::WorldMapArea* TravelScreens::defaultMap(const GameSession& session) const {
    if (!m_maps) {
        return nullptr;
    }
    const game::ZoneState& zone = session.play().travel().zone();
    if (const game::WorldMapArea* own = m_maps->zoneMap(zone.zone)) {
        return own;
    }
    return m_maps->continentMap(zone.map);
}

void TravelScreens::openWorldMap(const GameSession& session) {
    m_worldMapOpen = true;
    const game::WorldMapArea* area = defaultMap(session);
    m_worldMapArea = area ? area->id : 0;
    m_camera2d.reset();
}

void TravelScreens::buildMap(const game::WorldMapArea& area, const MapSubject& subject) {
    m_pieces.clear();
    m_labels.clear();
    m_pins.clear();
    m_mapUnits.clear();
    m_piecesLoading = false;
    // The 4 x 3 tiles.
    for (u32 i = 0; i < game::WorldMapData::kTileColumns * game::WorldMapData::kTileRows; ++i) {
        MapPiece piece;
        piece.texture = m_resources->texture(game::WorldMapData::tileTexture(area.directory, i + 1));
        const f32 x = static_cast<f32>(i % game::WorldMapData::kTileColumns) * game::WorldMapData::kTileSize;
        const f32 y = static_cast<f32>(i / game::WorldMapData::kTileColumns) * game::WorldMapData::kTileSize;
        piece.rect = {x, y, game::WorldMapData::kTileSize, game::WorldMapData::kTileSize};
        m_piecesLoading = m_piecesLoading || !piece.texture.valid();
        m_pieces.push_back(piece);
    }
    if (!area.continent()) {
        // What the player explored uncovers its overlay (without a player, everything shows).
        for (const game::WorldMapOverlay& o : m_maps->overlays(area.id)) {
            bool explored = subject.travel == nullptr;
            for (const u32 a : o.areas) {
                explored = explored || (a != 0 && subject.travel->explored(a));
            }
            if (!explored) {
                continue;
            }
            const u32 columns = (o.width + 255) / 256, rows = (o.height + 255) / 256;
            for (u32 row = 0; row < rows; ++row) {
                const u32 pixelH = row + 1 < rows ? 256u : (o.height % 256 == 0 ? 256u : o.height % 256);
                for (u32 column = 0; column < columns; ++column) {
                    const u32 pixelW =
                        column + 1 < columns ? 256u : (o.width % 256 == 0 ? 256u : o.width % 256);
                    MapPiece piece;
                    piece.texture = m_resources->texture(game::WorldMapData::overlayTexture(
                        area.directory, o.texture, row * columns + column + 1));
                    piece.rect = {static_cast<f32>(o.offsetX + column * 256),
                                  static_cast<f32>(o.offsetY + row * 256), static_cast<f32>(pixelW),
                                  static_cast<f32>(pixelH)};
                    // The last column and row are files of the next power of two: only their used part shows.
                    piece.uv = {0.0f, 0.0f,
                                static_cast<f32>(pixelW) / (column + 1 < columns ? 256.0f : fileSize(pixelW)),
                                static_cast<f32>(pixelH) / (row + 1 < rows ? 256.0f : fileSize(pixelH))};
                    m_pieces.push_back(piece);
                }
            }
        }
    } else {
        // The zones of the continent by name, where a tap opens them.
        for (const game::WorldMapArea& zone : m_maps->areas()) {
            glm::vec2 min, max;
            const game::AreaInfo* info = m_areas ? m_areas->area(zone.area) : nullptr;
            if (zone.map != area.map || zone.continent() || !info || !m_maps->zoneRect(zone, min, max)) {
                continue;
            }
            const glm::vec2 size(game::WorldMapData::kMapWidth, game::WorldMapData::kMapHeight);
            m_labels.push_back({info->name, (min + max) * 0.5f * size, (max.x - min.x) * size.x});
        }
    }
    // Known flight points of this map.
    if (m_taxi) {
        for (const game::TaxiNodeInfo& node : m_taxi->nodes()) {
            if (node.map != area.map || !game::taxiNodeKnown(m_knownNodes, node.id)) {
                continue;
            }
            const glm::vec2 uv = area.toMap(node.x, node.y);
            if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
                m_pins.push_back(
                    {uv * glm::vec2(game::WorldMapData::kMapWidth, game::WorldMapData::kMapHeight),
                     node.id == subject.currentTaxiNode});
            }
        }
    }
}

void TravelScreens::worldMapWindow(GameSession& session, DialogContext& c) {
    MapSubject subject;
    // Phase 15: the party (WorldMapFrame_Update's GetPlayerMapPosition("partyN") / ("raidN")): where the server last
    // put each member (its map from its zone).
    m_members.clear();
    if (session.inWorld()) {
        std::array<game::PartyMemberView, 40> members;
        const u32 count = session.play().social().groupMembers(members); // phase 16: a raid's whole roster
        for (u32 i = 0; i < count; ++i) {
            const game::PartyMemberView& m = members[i];
            const game::AreaInfo* zone = m_areas ? m_areas->area(m.zone) : nullptr;
            if (m.guid == session.playerGuid() || !m.online || !m.hasPosition) {
                continue;
            }
            m_members.push_back({m.name, m.inSight ? m_mapId : (zone ? zone->map : ~0u), m.x, m.y});
        }
        if (const std::string* me = session.playerName(session.playerGuid())) {
            subject.playerName = *me;
        }
        // Phase 17 (scores): WorldMapFrame_Update's team members out of the group (GetBattlefieldPosition) and its
        // flag (GetBattlefieldFlagPosition: the player's teammate carrying the enemy's), in the battleground.
        game::BattlefieldScore& score = session.play().battlefieldScore();
        const u32 nowMs = static_cast<u32>(c.ui.nowMs());
        for (u32 i = 0, n = score.positionCount(); i < n; ++i) {
            game::BattlefieldPosition p;
            if (score.position(i, nowMs, p)) {
                m_members.push_back({p.name, m_mapId, p.x, p.y});
            }
        }
        game::BattlefieldFlag flag;
        if (score.flagCarrier(nowMs, flag) && flag.token != game::BattlefieldFlag::Token::Unknown) {
            const bool alliance = flag.token == game::BattlefieldFlag::Token::AllianceFlag;
            m_flagSubjects[0] = {m_mapId, flag.x, flag.y,
                                 assetPathHash(alliance ? "Interface\\WorldStateFrame\\AllianceFlag.blp"
                                                        : "Interface\\WorldStateFrame\\HordeFlag.blp")};
            subject.flags = m_flagSubjects;
        }
    }
    subject.members = m_members;
    subject.hasPlayer = m_hasMap;
    subject.map = m_mapId;
    subject.position = m_position;
    subject.facing = m_facing;
    const game::DeathState& death = session.play().death();
    subject.hasCorpse = death.corpseKnown;
    subject.corpseMap = static_cast<u32>(death.corpseMap);
    subject.corpseX = death.corpseX;
    subject.corpseY = death.corpseY;
    subject.travel = session.inWorld() ? &session.play().travel() : nullptr;
    subject.currentTaxiNode = session.play().travel().taxiMap().currentNode;
    drawWorldMap(c, subject);
}

void TravelScreens::previewWorldMap(DialogContext& c, u32 mapArea, const MapSubject& subject) {
    if (mapArea != m_previewArea) {
        m_previewArea = mapArea;
        m_worldMapArea = mapArea;
        m_camera2d.reset();
    }
    m_worldMapOpen = true;
    drawWorldMap(c, subject);
    if (!m_worldMapOpen) { // closed: the gallery starts over at its map
        m_worldMapArea = mapArea;
        m_camera2d.reset();
    }
}

void TravelScreens::drawWorldMap(DialogContext& c, const MapSubject& subject) {
    // Phase 15: the World map (both continents, WorldMapContinent's place for each) above the continents.
    const bool world = m_maps && m_worldMapArea == game::WorldMapData::kWorldMap;
    const game::WorldMapArea* area = m_maps && !world ? m_maps->mapArea(m_worldMapArea) : nullptr;
    WorldMapView view;
    m_footer[0] = 0;
    const glm::vec2 mapSize(game::WorldMapData::kMapWidth, game::WorldMapData::kMapHeight);
    const auto onMap = [](glm::vec2 uv) { return uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f; };
    view.unitTexture = m_resources->texture(kPartyIcon);
    view.playerName = subject.playerName;
    if (world) {
        m_pieces.clear();
        m_labels.clear();
        m_pins.clear();
        m_mapUnits.clear();
        m_piecesLoading = false;
        for (u32 i = 0; i < game::WorldMapData::kTileColumns * game::WorldMapData::kTileRows; ++i) {
            MapPiece piece;
            piece.texture =
                m_resources->texture(game::WorldMapData::tileTexture(game::WorldMapData::kWorldDirectory, i + 1));
            piece.rect = {static_cast<f32>(i % game::WorldMapData::kTileColumns) * game::WorldMapData::kTileSize,
                          static_cast<f32>(i / game::WorldMapData::kTileColumns) * game::WorldMapData::kTileSize,
                          game::WorldMapData::kTileSize, game::WorldMapData::kTileSize};
            m_piecesLoading = m_piecesLoading || !piece.texture.valid();
            m_pieces.push_back(piece);
        }
        view.pieces = m_pieces;
        view.loading = m_piecesLoading;
        view.title = text::get("WORLD_MAP");
        if (const game::WorldMapContinent* own = subject.hasPlayer ? m_maps->continent(subject.map) : nullptr) {
            const glm::vec2 uv = own->toWorldMap(subject.position.x, subject.position.y);
            if (onMap(uv)) {
                view.hasPlayer = true;
                view.player = uv * mapSize;
                view.facing = subject.facing;
            }
        }
        if (const game::WorldMapContinent* own = subject.hasCorpse ? m_maps->continent(subject.corpseMap) : nullptr) {
            const glm::vec2 uv = own->toWorldMap(subject.corpseX, subject.corpseY);
            if (onMap(uv)) {
                view.hasCorpse = true;
                view.corpse = uv * mapSize;
            }
        }
        for (const MapSubject::Member& m : subject.members) {
            if (const game::WorldMapContinent* own = m_maps->continent(m.map)) {
                if (const glm::vec2 uv = own->toWorldMap(m.x, m.y); onMap(uv)) {
                    m_mapUnits.push_back({uv * mapSize, m.name});
                }
            }
        }
        view.units = m_mapUnits;
    } else if (area) {
        buildMap(*area, subject);
        view.pieces = m_pieces;
        view.labels = m_labels;
        view.pins = m_pins;
        view.loading = m_piecesLoading;
        view.pinTexture = m_resources->texture(kTaxiIcons[0]);
        const game::AreaInfo* zone = m_areas ? m_areas->area(area->area) : nullptr;
        const game::MapInfo* map = m_areas ? m_areas->map(area->map) : nullptr;
        view.title = zone ? zone->name : map ? map->name : area->directory;
        if (!area->continent()) {
            view.up = map ? map->name : text::get("CONTINENT");
        } else if (m_maps->continent(area->map)) {
            view.up = text::get("ZOOM_OUT"); // phase 15: to the World map
        }
        const glm::vec2 size(game::WorldMapData::kMapWidth, game::WorldMapData::kMapHeight);
        if (subject.hasPlayer && subject.map == area->map) {
            const glm::vec2 uv = area->toMap(subject.position.x, subject.position.y);
            if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
                view.hasPlayer = true;
                view.player = uv * size;
                view.facing = subject.facing;
                if (!area->continent()) {
                    text::format(m_footer, "VD_TRAVEL_MAP_POSITION", uv.x * 100.0f, uv.y * 100.0f);
                }
            }
        }
        if (subject.hasCorpse && subject.corpseMap == area->map) {
            const glm::vec2 uv = area->toMap(subject.corpseX, subject.corpseY);
            if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
                view.hasCorpse = true;
                view.corpse = uv * size;
            }
        }
        for (const MapSubject::Member& m : subject.members) { // phase 15: the party on this map
            if (const glm::vec2 uv = area->toMap(m.x, m.y); m.map == area->map && onMap(uv)) {
                m_mapUnits.push_back({uv * size, m.name});
            }
        }
        view.units = m_mapUnits;
        u32 flags = 0; // phase 17 (scores): the battleground's flag
        for (const MapSubject::Flag& f : subject.flags) {
            if (const glm::vec2 uv = area->toMap(f.x, f.y); f.map == area->map && onMap(uv) && flags < m_mapFlags.size()) {
                m_mapFlags[flags++] = {uv * size, f.texture};
            }
        }
        view.flags = {m_mapFlags.data(), flags};
    } else {
        view.title = text::get("WORLDMAP_BUTTON");
        text::format(m_footer, "VD_TRAVEL_MAP_NONE");
    }
    view.footer = m_footer.data();
    const WorldMapResult r = vd::worldMapWindow(c, view, m_camera2d);
    if (r.closed) {
        m_worldMapOpen = false;
    }
    if (area && r.up && !area->continent()) {
        if (const game::WorldMapArea* continent = m_maps->continentMap(area->map)) {
            m_worldMapArea = continent->id;
            m_camera2d.reset();
        }
    } else if (area && r.up && m_maps->continent(area->map)) { // phase 15: WorldMapZoomOutButton to the world
        m_worldMapArea = game::WorldMapData::kWorldMap;
        m_camera2d.reset();
    }
    if (world && r.tapped) { // phase 15: the continent under the tap (its ADT tiles)
        if (const game::WorldMapContinent* continent = m_maps->continentAt(r.tap / mapSize)) {
            if (const game::WorldMapArea* map = m_maps->continentMap(continent->map)) {
                m_worldMapArea = map->id;
                m_camera2d.reset();
            }
        }
    }
    if (area && r.tapped && area->continent()) {
        const glm::vec2 uv = r.tap / glm::vec2(game::WorldMapData::kMapWidth, game::WorldMapData::kMapHeight);
        if (const game::WorldMapArea* zone = m_maps->zoneAt(area->map, uv)) {
            m_worldMapArea = zone->id;
            m_camera2d.reset();
        }
    }
}

// --- Loading screen
// -------------------------------------------------------------------------------------------------

void TravelScreens::drawLoading(DialogContext& c, u32 map, f32 progress, std::string_view status) {
    // The character list's loading screen and the world's follow each other: one screen, one tip.
    const u64 now = c.ui.nowMs();
    if (!m_loadingDrawn || now - m_loadingDrawnMs > kLoadingPauseMs) {
        nextLoadingTip();
    }
    m_loadingDrawn = true;
    m_loadingDrawnMs = now;
    LoadingView view;
    const game::MapInfo* info = m_areas ? m_areas->map(map) : nullptr;
    view.art = info ? m_resources->texture(info->loadingScreen) : TextureHandle{};
    view.barBorder = m_resources->texture(kLoadingBarBorder);
    view.barFill = m_resources->texture(kLoadingBarFill);
    view.progress = progress;
    view.tip = m_tip;
    view.status = status;
    loadingScreen(c, view);
}

void TravelScreens::nextLoadingTip() {
    // WoW.exe 0x46B5F0, as each loading screen starts: with showGameTips, GameTips.dbc's row gameTip, and gameTip moves
    // on to the next row (the tips come in order, not at random); without, no tip and the counter stays.
    const bool show = m_settings ? m_settings->showGameTips : true;
    u32& next = m_settings ? m_settings->gameTip : m_ownTip;
    m_tip = show && m_areas ? m_areas->gameTip(next) : std::string_view{};
}

} // namespace vd
