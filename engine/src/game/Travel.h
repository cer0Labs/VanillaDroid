#pragma once

#include "core/Types.h"
#include "game/AreaData.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/TaxiData.h"
#include "net/Opcodes.gen.h"
#include "net/TravelMessages.h"

#include <glm/vec3.hpp>

#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::game {

// Where the player stands, as the zone text and the minimap's zone bar show it.
struct ZoneState {
    u32 map = 0;
    u32 area = 0; // under the player: a subzone or the zone itself (0: unknown, e.g. its tile is not loaded)
    u32 zone = 0; // the area's zone
    ZonePvp pvp = ZonePvp::Normal;
    std::string_view zoneName;
    std::string_view subzoneName; // empty while in the zone itself
    u32 changes = 0;              // bumped on every zone or subzone change
    bool zoneChanged = false;     // the last change entered another zone (else only the subzone changed)
    u32 changedMs = 0;
};

// A flight master's taxi map (SMSG_SHOWTAXINODES), open until a flight starts or the player closes it.
struct TaxiMapState {
    bool open = false;
    u64 flightMaster = 0;
    u32 currentNode = 0;
    TaxiMask known{};
};

// Whether a flight master's node is known (SMSG_TAXINODE_STATUS).
enum class TaxiNodeStatus : u8 { Unknown, NotLearned, Learned };

// The hearthstone's destination (SMSG_BINDPOINTUPDATE).
struct BindPoint {
    bool known = false;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    u32 map = 0;
    u32 areaId = 0;
};

// A raid the player is saved to (SMSG_RAID_INSTANCE_INFO: GetSavedInstanceInfo).
struct SavedInstance {
    u32 map = 0;
    u32 instanceId = 0;
    u32 resetAtMs = 0; // client clock (the Travel's nowMs) of its reset
};

// What the player's controller does with a spline the server put on the player's own entity (a taxi flight, a
// scripted move, fear): follow it, or answer its end or a stop (CMSG_MOVE_SPLINE_DONE), once per spline id.
struct ServerSplineStep {
    enum class Kind : u8 { None, Follow, Done };
    Kind kind = Kind::None;
    glm::vec3 position{0.0f}; // where the character is now (Follow) or ends (Done)
    f32 facing = 0.0f;
    bool hasFacing = false; // else it keeps its own
    u32 splineId = 0;
};
// `handledSpline`: the id last answered (0: none). Splines of cyclic paths are followed while they last.
ServerSplineStep serverSplineStep(const Entity& player, u32 handledSpline, u32 nowMs);

// What the player reads about travel (core/Text, the client's own texts).
// The error line of an SMSG_ACTIVATETAXIREPLY code: its ERR_TAXI* (empty for Ok).
std::string_view taxiReplyText(u32 reply);
// The error line of an SMSG_TRANSFER_ABORTED reason: its TRANSFER_ABORT_* (empty when the client shows none).
std::string_view transferAbortText(u8 reason);
// SecondsToTime (UIParent.lua 1.12): "6 Days 23 Hrs", "4 Hrs 5 Mins", "3 Mins 20 Secs": at most two units, days and
// hours only above a whole one (not equal), each with GetText's plural (DAYS_ABBR / _P1...). Phase 12: the raid
// information's "Resets in". Written into `out` (NUL-terminated).
std::string_view secondsToTime(std::span<char> out, u32 seconds, bool noSeconds = false);
// ZoneText.xml's PVPInfoTextString for a place of `pvp` seen by a player of `playerTeam`, written into `out`:
// "Alliance Territory", "Contested Territory"... (empty for none).
std::string_view zonePvpText(std::span<char> out, ZonePvp pvp, u32 playerTeam);

// The travel side of the world session (phase 8): the zone under the player (zone text, exploration), area
// triggers (CMSG_AREATRIGGER once per entry), flight masters (taxi map, flights, node status), innkeepers
// (bind point) and far teleports (the map of the loading screen). Owned by game::Gameplay, which hands it its
// packets (handles()) and session calls; the game feeds it the player's position every frame. Main thread;
// nothing allocates per frame once warmed up.
class Travel {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr f32 kTriggerLeaveMargin = 1.0f; // yards beyond a trigger before it counts as left
    static constexpr u32 kTriggerRepeatMs = 2000;    // the same trigger is never sent twice within this

    Travel(const EntityStore& entities, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    // Client tables (either may be null: no zone names, no triggers, no routes). They must outlive this
    // object.
    void setData(const AreaData* areas, const TaxiData* taxi);
    const AreaData* areas() const { return m_areas; }
    const TaxiData* taxi() const { return m_taxi; }

    // Session (from Gameplay).
    void enterWorld(u64 player, u32 map, bool newLogin, u32 nowMs);
    void leaveWorld();
    // The packets handled here (Gameplay delegates them).
    static bool handles(net::Opcode opcode);
    // False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // After each SMSG_UPDATE_OBJECT: flight masters that appeared are asked about their node.
    void afterUpdate(std::span<const u64> created);
    void update(u32 nowMs);

    // Once per frame: where the player's feet are on `map`, and the terrain's area there (Terrain::areaAt, 0
    // when unknown). `canTrigger`: area triggers may be entered now (in the world, standing on loaded ground,
    // not on a taxi). Works offline too (zone text; triggers are only logged).
    void updatePosition(u32 map, const glm::vec3& feet, u32 terrainArea, bool canTrigger, u32 nowMs);
    const ZoneState& zone() const { return m_zone; }
    // Phase 14 (sound): the room of a building the player stands in (WMOAreaTable through world/WmoAreas): its area
    // (0: the terrain's) and its name ("Lion's Pride Inn", Stormwind's "Trade District"; empty: the area's), which the
    // client shows as the subzone (GetSubZoneText, GetMinimapZoneText). `name` must outlive the call (the rows'
    // names do). Call before updatePosition().
    void setIndoorArea(u32 areaId, std::string_view name) {
        m_indoorArea = areaId;
        m_indoorName = name;
    }
    // Area triggers the player is inside now (AreaTrigger.dbc ids), and how many were sent since the world
    // began.
    std::span<const u32> insideTriggers() const { return {m_inside.data(), m_insideCount}; }
    u32 triggersSent() const { return m_triggersSent; }
    // The player's team (kAreaTeamAlliance / kAreaTeamHorde) from its race; Alliance when unknown.
    u32 playerTeam() const;
    // Whether the player discovered an area (PLAYER_EXPLORED_ZONES).
    bool explored(u32 areaId) const;

    // --- Flight masters ---
    const TaxiMapState& taxiMap() const { return m_taxiMap; }
    void closeTaxiMap() { m_taxiMap.open = false; }
    // A flight master without a gossip menu: CMSG_TAXIQUERYAVAILABLENODES (its map, or "new flight path").
    void openTaxiMap(u64 flightMaster);
    // Flies the route from the open taxi map's node: CMSG_ACTIVATETAXI for one path, CMSG_ACTIVATETAXIEXPRESS
    // for several. False when not sent.
    bool takeFlight(const TaxiRoute& route);
    // The route the taxi map offers to `destination` (from its current node, through known nodes).
    bool routeTo(u32 destination, TaxiRoute& out) const;
    TaxiNodeStatus taxiStatus(u64 guid) const;
    void queryTaxiStatus(u64 guid); // CMSG_TAXINODE_STATUS_QUERY
    // The player is on a taxi (UNIT_FLAG_TAXI_FLIGHT).
    bool flying() const;

    // --- Innkeepers ---
    // An innkeeper asked to bind the player (SMSG_BINDER_CONFIRM), 0 when none: answer with answerBinder().
    u64 binderAsking() const { return m_binder; }
    void answerBinder(bool yes); // yes: CMSG_BINDER_ACTIVATE
    const BindPoint& bindPoint() const { return m_bindPoint; }

    // --- Far teleports ---
    // The map of the last SMSG_TRANSFER_PENDING (the loading screen's art), until the next world.
    u32 transferMap() const { return m_transferMap; }
    bool transferPending() const { return m_transferPending; }
    // The transport (game object entry) the far teleport carries the player on, 0 when none: SMSG_NEW_WORLD
    // then holds the player's offset on it, not a world position (Player::SendNewWorld). Kept until the next
    // one.
    u32 transferTransport() const { return m_transferTransport; }

    // --- Instances (phase 12) ---
    // The raids the player is saved to, as the last SMSG_RAID_INSTANCE_INFO listed them (asked on every world
    // entry, as RaidFrame does on PLAYER_ENTERING_WORLD, and by /raidinfo).
    std::span<const SavedInstance> savedInstances() const { return m_saved; }
    bool savedInstancesKnown() const { return m_savedKnown; }
    // RequestRaidInfo (CMSG_REQUEST_RAID_INFO). `slashCommand`: /raidinfo, whose answer says so in its
    // SavedInstancesUpdated event (the chat then reports "not saved" when there is nothing to show).
    void requestRaidInfo(bool slashCommand = false);
    // CanShowResetInstances, as WoW.exe decides it: not inside a party dungeon (Map.dbc type 1), having left one
    // (or been told of one, SMSG_UPDATE_LAST_INSTANCE) within the last 25 hours. WoW.exe also wants
    // SMSG_UPDATE_INSTANCE_OWNERSHIP's flag, which vMaNGOS only sets with a permanent (raid) bind: with it the
    // option would never show for the 5-man dungeons it resets, so it is left out. Leader or alone: the caller's
    // (UnitPopup).
    bool canShowResetInstances(u32 nowMs) const;
    // SMSG_UPDATE_INSTANCE_OWNERSHIP's flag (vMaNGOS: the player has a permanent bind).
    bool instanceOwnership() const { return m_ownsInstances; }
    // ResetInstances (CMSG_RESET_INSTANCES): the leader's or a lone player's; the server answers each map with
    // SMSG_INSTANCE_RESET or _FAILED.
    void resetInstances();
    // GetInstanceBootTimeRemaining: ms until the server sends the player to its bind point from an instance that
    // is not its group's (SMSG_RAID_GROUP_ONLY with a time), 0 when no countdown runs.
    u32 bootRemainingMs(u32 nowMs) const;
    static constexpr u32 kResetInstancesShownMs = 90000u * 1000u; // WoW.exe: 90000 s since leaving the dungeon

private:
    const Entity* me() const { return m_entities.find(m_player); }
    bool send(net::Opcode opcode);
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0);
    void updateZone(u32 map, u32 area, u32 nowMs);
    void updateTriggers(u32 map, const glm::vec3& feet, bool canTrigger, u32 nowMs);

    const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    const AreaData* m_areas = nullptr;
    const TaxiData* m_taxi = nullptr;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_map = 0;
    u32 m_nowMs = 0;
    std::vector<u8> m_body; // outgoing scratch

    ZoneState m_zone;
    u32 m_indoorArea = 0;           // phase 14: setIndoorArea()
    std::string_view m_indoorName;
    std::string_view m_indoorShown; // the indoor name in m_zone
    std::array<u32, 16> m_inside{}; // triggers the player is in
    u32 m_insideCount = 0;
    struct SentTrigger {
        u32 id = 0;
        u32 ms = 0;
    };
    std::array<SentTrigger, 8> m_recent{}; // last sends (anti-repeat)
    u32 m_recentNext = 0;
    u32 m_triggersSent = 0;

    TaxiMapState m_taxiMap;
    std::unordered_map<u64, TaxiNodeStatus> m_taxiStatus;
    u32 m_statusPruneAt = 0;
    u64 m_binder = 0;
    BindPoint m_bindPoint;
    u32 m_transferMap = 0;
    u32 m_transferTransport = 0;
    bool m_transferPending = false;

    bool isPartyDungeon(u32 map) const;
    void noteLastInstance(u32 map, u32 current, u32 nowMs);
    std::vector<SavedInstance> m_saved;
    std::vector<net::SavedInstanceInfo> m_savedScratch;
    bool m_savedKnown = false;
    bool m_raidInfoAsked = false;  // a /raidinfo waits for its answer
    bool m_ownsInstances = false;  // SMSG_UPDATE_INSTANCE_OWNERSHIP (see canShowResetInstances)
    u32 m_lastDungeon = 0;         // the party dungeon left last (WoW.exe's "last instance"), 0: none
    u32 m_lastDungeonLeftMs = 0;
    u32 m_bootEndMs = 0;           // INSTANCE_BOOT's end (client clock), 0: none

    net::ShowTaxiNodes m_showTaxiNodes; // parsed messages, reused
};

} // namespace vd::game
