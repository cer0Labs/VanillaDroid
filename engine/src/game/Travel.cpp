#include "game/Travel.h"

#include "core/Log.h"
#include "core/Text.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

constexpr u32 kStatusPruneMs = 10000; // node statuses of flight masters gone are forgotten this often
constexpr u32 kExploredWords = 64;    // PLAYER_EXPLORED_ZONES_1..64

bool due(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

} // namespace

ServerSplineStep serverSplineStep(const Entity& player, u32 handledSpline, u32 nowMs) {
    ServerSplineStep step;
    step.splineId = player.splineId;
    if (player.splineId == 0 || player.splineId == handledSpline) {
        return step;
    }
    if (player.splineStopped || !player.moving || player.path.size() < 2) {
        step.kind = ServerSplineStep::Kind::Done; // a stop: where the server holds it
        step.position = player.position;
        return step;
    }
    if (!player.cyclic && nowMs - player.splineStartMs >= player.splineDuration) {
        step.kind = ServerSplineStep::Kind::Done; // arrived: the end of the path, facing its final way
        step.position = player.destination;
        step.hasFacing = player.hasFinalAngle;
        step.facing = player.finalAngle;
        return step;
    }
    step.kind = ServerSplineStep::Kind::Follow;
    step.position = player.positionAt(nowMs);
    step.hasFacing = player.headingAt(nowMs, step.facing);
    return step;
}

std::string_view taxiReplyText(u32 reply) {
    // ActivateTaxiReplies: the client's ERR_TAXI* texts, one per code.
    switch (static_cast<net::TaxiReply>(reply)) {
    case net::TaxiReply::Ok: return {};
    case net::TaxiReply::NoSuchPath: return text::get("ERR_TAXINOSUCHPATH");
    case net::TaxiReply::NotEnoughMoney: return text::get("ERR_TAXINOTENOUGHMONEY");
    case net::TaxiReply::TooFarAway: return text::get("ERR_TAXITOOFARAWAY");
    case net::TaxiReply::NoVendorNearby: return text::get("ERR_TAXINOVENDORNEARBY");
    case net::TaxiReply::NotVisited: return text::get("ERR_TAXINOTVISITED");
    case net::TaxiReply::PlayerBusy: return text::get("ERR_TAXIPLAYERBUSY");
    case net::TaxiReply::PlayerAlreadyMounted: return text::get("ERR_TAXIPLAYERALREADYMOUNTED");
    case net::TaxiReply::PlayerShapeshifted: return text::get("ERR_TAXIPLAYERSHAPESHIFTED");
    case net::TaxiReply::PlayerMoving: return text::get("ERR_TAXIPLAYERMOVING");
    case net::TaxiReply::SameNode: return text::get("ERR_TAXISAMENODE");
    case net::TaxiReply::NotStanding: return text::get("ERR_TAXINOTSTANDING");
    default: return text::get("ERR_TAXIUNSPECIFIEDSERVERERROR");
    }
}

std::string_view transferAbortText(u8 reason) {
    switch (reason) {
    case net::kTransferAbortMaxPlayers: return text::get("TRANSFER_ABORT_MAX_PLAYERS");
    case net::kTransferAbortNotFound: return text::get("TRANSFER_ABORT_NOT_FOUND");
    case net::kTransferAbortTooManyInstances: return text::get("TRANSFER_ABORT_TOO_MANY_INSTANCES");
    case net::kTransferAbortZoneInCombat: return text::get("TRANSFER_ABORT_ZONE_IN_COMBAT");
    default: return {}; // silently, or a reason this client does not know
    }
}

std::string_view secondsToTime(std::span<char> out, u32 seconds, bool noSeconds) {
    if (out.empty()) {
        return {};
    }
    usize used = 0;
    u32 count = 0;
    const auto unit = [&](u32 n, std::string_view one, std::string_view many) {
        const std::string_view name = text::get(n == 1 ? text::Key(one) : text::Key(many));
        const int written = std::snprintf(out.data() + used, out.size() - used, "%s%u %.*s", used > 0 ? " " : "", n,
                                          static_cast<int>(name.size()), name.data());
        used = std::min(out.size() - 1, used + static_cast<usize>(std::max(written, 0)));
        ++count;
    };
    out[0] = '\0';
    if (seconds > 86400) {
        unit(seconds / 86400, "DAYS_ABBR", "DAYS_ABBR_P1");
        seconds %= 86400;
    }
    if (seconds > 3600) {
        unit(seconds / 3600, "HOURS_ABBR", "HOURS_ABBR_P1");
        seconds %= 3600;
    }
    if (count < 2 && seconds >= 60) {
        unit(seconds / 60, "MINUTES_ABBR", "MINUTES_ABBR_P1");
        seconds %= 60;
    }
    if (count < 2 && seconds > 0 && !noSeconds) {
        unit(seconds, "SECONDS_ABBR", "SECONDS_ABBR_P1");
    }
    return {out.data(), used};
}

std::string_view zonePvpText(std::span<char> out, ZonePvp pvp, u32 playerTeam) {
    switch (pvp) {
    case ZonePvp::Friendly:
    case ZonePvp::Hostile: {
        // FACTION_CONTROLLED_TERRITORY names who holds the place: the player's own faction when friendly.
        const bool alliance = (pvp == ZonePvp::Friendly) == (playerTeam == kAreaTeamAlliance);
        return text::format(out, "FACTION_CONTROLLED_TERRITORY",
                            text::get(alliance ? "FACTION_ALLIANCE" : "FACTION_HORDE"));
    }
    case ZonePvp::Contested: return text::format(out, "CONTESTED_TERRITORY");
    case ZonePvp::Sanctuary: return text::format(out, "VD_TRAVEL_SANCTUARY"); // no 1.12 text: later clients'
    case ZonePvp::Arena: return text::format(out, "FREE_FOR_ALL_TERRITORY");
    default: return {};
    }
}

Travel::Travel(const EntityStore& entities, Events& events) : m_entities(entities), m_events(events) {
    m_body.reserve(128);
    m_taxiStatus.reserve(64);
}

void Travel::setData(const AreaData* areas, const TaxiData* taxi) {
    m_areas = areas;
    m_taxi = taxi;
}

void Travel::enterWorld(u64 player, u32 map, bool newLogin, u32 nowMs) {
    if (newLogin) {
        m_lastDungeon = 0; // another character, or the same one again: nothing left behind yet
        m_ownsInstances = false;
        m_saved.clear();
        m_savedKnown = false;
        m_bootEndMs = 0;
    } else {
        noteLastInstance(m_map, map, nowMs); // WoW.exe on every world change: the party dungeon just left
    }
    m_inWorld = true;
    m_player = player;
    m_map = map;
    m_nowMs = nowMs;
    m_zone = ZoneState{}; // the zone text shows again on arrival, as after the client's loading screen
    m_insideCount = 0;
    m_taxiMap = {};
    m_taxiStatus.clear();
    m_statusPruneAt = nowMs + kStatusPruneMs;
    m_binder = 0;
    m_transferPending = false;
    if (newLogin) {
        m_transferTransport = 0; // a login never arrives on a transport's far teleport
        m_bindPoint = BindPoint{};
        m_triggersSent = 0;
        m_recent = {};
    }
    m_raidInfoAsked = false;
    requestRaidInfo(); // RaidFrame: RequestRaidInfo() on PLAYER_ENTERING_WORLD
}

void Travel::leaveWorld() {
    m_inWorld = false;
    m_taxiMap = {};
    m_taxiStatus.clear();
    m_binder = 0;
    m_insideCount = 0;
    m_transferPending = false;
}

bool Travel::send(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

GameEvent& Travel::event(GameEventKind kind, u32 id, u64 guid) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.id = id;
    e.guid = guid;
    return e;
}

bool Travel::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_EXPLORATION_EXPERIENCE:
    case Opcode::SMSG_SHOWTAXINODES:
    case Opcode::SMSG_TAXINODE_STATUS:
    case Opcode::SMSG_ACTIVATETAXIREPLY:
    case Opcode::SMSG_NEW_TAXI_PATH:
    case Opcode::SMSG_BINDER_CONFIRM:
    case Opcode::SMSG_BINDPOINTUPDATE:
    case Opcode::SMSG_PLAYERBOUND:
    case Opcode::SMSG_TRANSFER_PENDING:
    case Opcode::SMSG_TRANSFER_ABORTED:
    case Opcode::SMSG_INSTANCE_SAVE_CREATED:
    case Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP:
    case Opcode::SMSG_UPDATE_LAST_INSTANCE:
    case Opcode::SMSG_RAID_INSTANCE_MESSAGE:
    case Opcode::SMSG_INSTANCE_RESET:
    case Opcode::SMSG_INSTANCE_RESET_FAILED:
    case Opcode::SMSG_RAID_INSTANCE_INFO:
    case Opcode::SMSG_RAID_GROUP_ONLY: return true;
    default: return false;
    }
}

bool Travel::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    bool ok = true;
    switch (opcode) {
    case Opcode::SMSG_EXPLORATION_EXPERIENCE: {
        net::ExplorationExperience m;
        ok = net::parseExplorationExperience(p, m);
        if (ok) {
            event(GameEventKind::AreaExplored, m.areaId).value[0] = static_cast<i32>(m.experience);
        }
        break;
    }
    case Opcode::SMSG_SHOWTAXINODES:
        ok = net::parseShowTaxiNodes(p, m_showTaxiNodes);
        if (ok) {
            m_taxiMap.open = m_showTaxiNodes.showUi != 0;
            m_taxiMap.flightMaster = m_showTaxiNodes.flightMaster;
            m_taxiMap.currentNode = m_showTaxiNodes.currentNode;
            m_taxiMap.known = m_showTaxiNodes.knownNodes;
            // The node it is shown from is known: the server only offers the map there.
            if (m_taxiMap.currentNode < 256) {
                m_taxiMap.known[m_taxiMap.currentNode / 32] |= 1u << (m_taxiMap.currentNode % 32);
            }
            m_taxiStatus[m_taxiMap.flightMaster] = TaxiNodeStatus::Learned;
            event(GameEventKind::TaxiMapOpened, m_taxiMap.currentNode, m_taxiMap.flightMaster);
        }
        break;
    case Opcode::SMSG_TAXINODE_STATUS: {
        u64 guid = 0;
        bool known = false;
        ok = net::parseTaxiNodeStatus(p, guid, known);
        if (ok) {
            m_taxiStatus[guid] = known ? TaxiNodeStatus::Learned : TaxiNodeStatus::NotLearned;
        }
        break;
    }
    case Opcode::SMSG_ACTIVATETAXIREPLY: {
        u32 reply = 0;
        ok = net::parseActivateTaxiReply(p, reply);
        if (ok) {
            if (reply == static_cast<u32>(net::TaxiReply::Ok)) {
                m_taxiMap.open = false; // the flight starts
            }
            event(GameEventKind::TaxiReply).value[0] = static_cast<i32>(reply);
        }
        break;
    }
    case Opcode::SMSG_NEW_TAXI_PATH: event(GameEventKind::TaxiPathLearned); break;
    case Opcode::SMSG_BINDER_CONFIRM: {
        u64 guid = 0;
        ok = net::parseGuid(p, guid);
        if (ok) {
            m_binder = guid;
            event(GameEventKind::BinderConfirm, 0, guid);
        }
        break;
    }
    case Opcode::SMSG_BINDPOINTUPDATE: {
        net::BindPointUpdate m;
        ok = net::parseBindPointUpdate(p, m);
        if (ok) {
            m_bindPoint = {true, m.x, m.y, m.z, m.map, m.areaId};
        }
        break;
    }
    case Opcode::SMSG_PLAYERBOUND: {
        u64 binder = 0;
        u32 area = 0;
        ok = net::parsePlayerBound(p, binder, area);
        if (ok) {
            m_bindPoint.areaId = area;
            event(GameEventKind::PlayerBound, area, binder);
        }
        break;
    }
    case Opcode::SMSG_TRANSFER_PENDING: {
        net::TransferPending m;
        ok = net::parseTransferPending(p, m);
        if (ok) {
            m_transferMap = m.map;
            m_transferTransport = m.onTransport ? m.transportEntry : 0;
            m_transferPending = true;
            m_taxiMap.open = false;
        }
        break;
    }
    case Opcode::SMSG_TRANSFER_ABORTED: {
        u8 reason = 0;
        ok = net::parseTransferAborted(p, reason);
        if (ok) {
            m_transferPending = false;
            event(GameEventKind::TransferAborted).value[0] = reason;
        }
        break;
    }
    // Instances (phase 12). What WoW.exe does with each: its handlers at 0x49E1C0 (warnings) and 0x49E470 (reset)
    // write the client's own system lines; 0x49E660 / 0x49E6C0 feed CanShowResetInstances; 0x5E48F1 runs the boot.
    case Opcode::SMSG_INSTANCE_SAVE_CREATED: {
        u32 unused = 0;
        ok = net::parseU32(p, unused);
        if (ok) {
            event(GameEventKind::InstanceSaved, m_map);
        }
        break;
    }
    case Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP: {
        u32 owns = 0;
        ok = net::parseU32(p, owns);
        m_ownsInstances = ok && owns != 0;
        break;
    }
    case Opcode::SMSG_UPDATE_LAST_INSTANCE: {
        u32 map = 0;
        ok = net::parseU32(p, map);
        if (ok) {
            noteLastInstance(map, m_map, nowMs);
        }
        break;
    }
    case Opcode::SMSG_RAID_INSTANCE_MESSAGE: {
        net::RaidInstanceMessage m;
        ok = net::parseRaidInstanceMessage(p, m);
        if (ok && m.type >= net::kRaidInstanceWarningHours && m.type <= net::kRaidInstanceWelcome) {
            GameEvent& e = event(GameEventKind::InstanceResetWarning, m.map);
            e.value[0] = static_cast<i32>(m.type);
            e.value[1] = static_cast<i32>(std::min<u32>(m.secondsLeft, 0x7FFFFFFFu));
        }
        break;
    }
    case Opcode::SMSG_INSTANCE_RESET: {
        u32 map = 0;
        ok = net::parseU32(p, map);
        if (ok) {
            m_lastDungeon = 0; // WoW.exe forgets the last instance: the option goes away
            event(GameEventKind::InstanceReset, map);
        }
        break;
    }
    case Opcode::SMSG_INSTANCE_RESET_FAILED: {
        u32 reason = 0, map = 0;
        ok = net::parseInstanceResetFailed(p, reason, map);
        if (ok) {
            event(GameEventKind::InstanceResetFailed, map).value[0] = static_cast<i32>(reason);
        }
        break;
    }
    case Opcode::SMSG_RAID_INSTANCE_INFO: {
        ok = net::parseRaidInstanceInfo(p, m_savedScratch);
        if (ok) {
            m_saved.clear();
            for (const net::SavedInstanceInfo& s : m_savedScratch) {
                m_saved.push_back({s.map, s.instanceId, nowMs + std::min<u32>(s.secondsLeft, 30u * 86400u) * 1000u});
            }
            m_savedKnown = true;
            event(GameEventKind::SavedInstancesUpdated).value[0] = m_raidInfoAsked ? 1 : 0;
            m_raidInfoAsked = false;
        }
        break;
    }
    case Opcode::SMSG_RAID_GROUP_ONLY: {
        u32 timerMs = 0, error = 0;
        ok = net::parseRaidGroupOnly(p, timerMs, error);
        if (!ok) {
            break;
        }
        if (timerMs > 0) {
            m_bootEndMs = std::max(1u, nowMs + timerMs);
            event(GameEventKind::InstanceBootStarted).value[0] = static_cast<i32>(std::min(timerMs, 0x7FFFFFFFu));
        } else {
            // WoW.exe shows the error with no time, even when it cancels a countdown (vMaNGOS cancels with
            // ERR_RAID_GROUP_REQUIRED, so the line shows then too), and stops the countdown.
            if (error == net::kRaidGroupRequired || error == net::kRaidGroupFull) {
                event(GameEventKind::RaidGroupOnly).value[0] = static_cast<i32>(error);
            }
            m_bootEndMs = 0;
            event(GameEventKind::InstanceBootStopped);
        }
        break;
    }
    default: break;
    }
    return ok;
}

bool Travel::isPartyDungeon(u32 map) const {
    const MapInfo* info = m_areas ? m_areas->map(map) : nullptr;
    return info && info->instanceType == 1;
}

void Travel::noteLastInstance(u32 map, u32 current, u32 nowMs) {
    // WoW.exe 0x495D10: a party dungeon other than the current map becomes the last instance, with the time.
    if (map != current && isPartyDungeon(map)) {
        m_lastDungeon = map;
        m_lastDungeonLeftMs = nowMs;
    }
}

void Travel::requestRaidInfo(bool slashCommand) {
    m_body.clear();
    if (send(Opcode::CMSG_REQUEST_RAID_INFO)) {
        m_raidInfoAsked = m_raidInfoAsked || slashCommand;
    }
}

bool Travel::canShowResetInstances(u32 nowMs) const {
    return m_inWorld && !isPartyDungeon(m_map) && m_lastDungeon != 0 &&
           nowMs - m_lastDungeonLeftMs <= kResetInstancesShownMs;
}

void Travel::resetInstances() {
    m_body.clear();
    send(Opcode::CMSG_RESET_INSTANCES);
}

u32 Travel::bootRemainingMs(u32 nowMs) const {
    if (m_bootEndMs == 0) {
        return 0;
    }
    const i32 left = static_cast<i32>(m_bootEndMs - nowMs);
    return left > 0 ? static_cast<u32>(left) : 0u;
}

void Travel::afterUpdate(std::span<const u64> created) {
    if (!m_inWorld) {
        return;
    }
    for (const u64 guid : created) {
        const Entity* e = m_entities.find(guid);
        if (e && e->type == ObjectType::Unit && (npcFlags(*e) & kNpcFlagFlightMaster) &&
            m_taxiStatus.find(guid) == m_taxiStatus.end()) {
            queryTaxiStatus(guid);
        }
    }
}

void Travel::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (m_inWorld && due(nowMs, m_statusPruneAt)) {
        m_statusPruneAt = nowMs + kStatusPruneMs;
        std::erase_if(m_taxiStatus,
                      [&](const auto& entry) { return m_entities.find(entry.first) == nullptr; });
    }
    if (m_taxiMap.open) {
        // The map closes when its flight master is gone (as the client's TaxiFrame on its distance check).
        if (!m_entities.find(m_taxiMap.flightMaster)) {
            m_taxiMap.open = false;
        }
    }
}

// -------------------------------------------------------------------------------------------------------
// Zones and area triggers.

u32 Travel::playerTeam() const {
    const Entity* player = me();
    const u8 race = player ? player->byteValue(uf::UNIT_FIELD_BYTES_0, 0) : 1;
    // ChrRaces: humans, dwarves, night elves and gnomes are the Alliance.
    return (race == 1 || race == 3 || race == 4 || race == 7) ? kAreaTeamAlliance : kAreaTeamHorde;
}

bool Travel::explored(u32 areaId) const {
    const Entity* player = me();
    if (!player || !m_areas || player->values.size() < uf::PLAYER_EXPLORED_ZONES_1 + kExploredWords) {
        return false;
    }
    return m_areas->explored({player->values.data() + uf::PLAYER_EXPLORED_ZONES_1, kExploredWords}, areaId);
}

void Travel::updatePosition(u32 map, const glm::vec3& feet, u32 terrainArea, bool canTrigger, u32 nowMs) {
    m_nowMs = nowMs;
    u32 area = m_indoorArea != 0 && terrainArea != 0 ? m_indoorArea : terrainArea; // phase 14: a building's room
    if (area == 0 && m_areas) {
        // Maps without terrain areas (instances) are one zone.
        if (const MapInfo* m = m_areas->map(map); m && m->instanceType != 0) {
            area = m_areas->mapZone(map);
        }
    }
    if (area != 0 || map != m_zone.map) {
        updateZone(map, area, nowMs);
    }
    updateTriggers(map, feet, canTrigger, nowMs);
}

void Travel::updateZone(u32 map, u32 area, u32 nowMs) {
    if (map == m_zone.map && area == m_zone.area && m_indoorName == m_indoorShown) {
        return;
    }
    const u32 previousZone = m_zone.zone;
    m_zone.map = map;
    m_zone.area = area;
    m_zone.zone = m_areas ? m_areas->zoneOf(area) : 0;
    m_zone.zoneName = {};
    m_zone.subzoneName = {};
    m_zone.pvp = ZonePvp::Normal;
    if (m_areas) {
        if (const AreaInfo* zone = m_areas->area(m_zone.zone)) {
            m_zone.zoneName = zone->name;
        }
        if (const AreaInfo* sub = m_areas->area(area); sub && area != m_zone.zone) {
            m_zone.subzoneName = sub->name;
        }
        m_zone.pvp = m_areas->pvp(area, playerTeam());
    }
    // Phase 14: inside a building's room with a name, that is the subzone.
    m_indoorShown = area != 0 ? m_indoorName : std::string_view{};
    if (!m_indoorShown.empty()) {
        m_zone.subzoneName = m_indoorShown;
    }
    if (area == 0) {
        return; // a new map whose terrain is not known yet: no text until it is
    }
    ++m_zone.changes;
    m_zone.zoneChanged = m_zone.zone != previousZone;
    m_zone.changedMs = nowMs;
    GameEvent& e = event(GameEventKind::ZoneChanged, m_zone.zone);
    e.value[0] = static_cast<i32>(area);
    e.value[1] = m_zone.zoneChanged ? 1 : 0;
}

void Travel::updateTriggers(u32 map, const glm::vec3& feet, bool canTrigger, u32 nowMs) {
    if (!m_areas) {
        return;
    }
    // Left: no longer inside (by a margin, so the border does not flicker), or another map.
    for (u32 i = 0; i < m_insideCount;) {
        const AreaTriggerInfo* t = m_areas->trigger(m_inside[i]);
        if (!t || t->map != map || !t->contains(feet.x, feet.y, feet.z, kTriggerLeaveMargin)) {
            m_inside[i] = m_inside[--m_insideCount];
        } else {
            ++i;
        }
    }
    if (!canTrigger) {
        return; // entering waits until the player can (standing on loaded ground, off the taxi)
    }
    for (const AreaTriggerInfo& t : m_areas->triggers(map)) {
        if (!t.contains(feet.x, feet.y, feet.z)) {
            continue;
        }
        const u32* begin = m_inside.data();
        const u32* end = begin + m_insideCount;
        if (std::find(begin, end, t.id) != end) {
            continue; // still inside: sent once per entry
        }
        if (m_insideCount < m_inside.size()) {
            m_inside[m_insideCount++] = t.id;
        }
        bool repeated = false;
        for (const SentTrigger& sent : m_recent) {
            repeated = repeated || (sent.id == t.id && nowMs - sent.ms < kTriggerRepeatMs);
        }
        if (repeated) {
            continue;
        }
        m_recent[m_recentNext++ % m_recent.size()] = {t.id, nowMs};
        m_body.clear();
        net::writeAreaTrigger(m_body, t.id);
        const bool sent = send(Opcode::CMSG_AREATRIGGER);
        m_triggersSent += sent ? 1 : 0;
        VD_LOG_INFO("Area trigger %u entered at (%.1f, %.1f, %.1f)%s", t.id, feet.x, feet.y, feet.z,
                    sent ? ": CMSG_AREATRIGGER" : " (offline: not sent)");
        event(GameEventKind::AreaTriggerSent, t.id).value[0] = sent ? 1 : 0;
    }
}

// -------------------------------------------------------------------------------------------------------
// Flight masters.

void Travel::openTaxiMap(u64 flightMaster) {
    m_body.clear();
    net::writeGuid(m_body, flightMaster);
    send(Opcode::CMSG_TAXIQUERYAVAILABLENODES);
}

bool Travel::routeTo(u32 destination, TaxiRoute& out) const {
    return m_taxi && m_taxiMap.currentNode != 0 &&
           m_taxi->route(m_taxiMap.currentNode, destination, m_taxiMap.known, out);
}

bool Travel::takeFlight(const TaxiRoute& route) {
    if (!m_taxiMap.open || route.count < 2 || route.nodes[0] != m_taxiMap.currentNode) {
        return false;
    }
    m_body.clear();
    if (route.count == 2) {
        net::writeActivateTaxi(m_body, m_taxiMap.flightMaster, route.nodes[0], route.nodes[1]);
        return send(Opcode::CMSG_ACTIVATETAXI);
    }
    net::writeActivateTaxiExpress(m_body, m_taxiMap.flightMaster, route.cost, route.list());
    return send(Opcode::CMSG_ACTIVATETAXIEXPRESS);
}

TaxiNodeStatus Travel::taxiStatus(u64 guid) const {
    const auto found = m_taxiStatus.find(guid);
    return found == m_taxiStatus.end() ? TaxiNodeStatus::Unknown : found->second;
}

void Travel::queryTaxiStatus(u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    send(Opcode::CMSG_TAXINODE_STATUS_QUERY);
}

bool Travel::flying() const {
    const Entity* player = me();
    return player && (player->value(uf::UNIT_FIELD_FLAGS) & kUnitFlagTaxiFlight) != 0;
}

// -------------------------------------------------------------------------------------------------------
// Innkeepers.

void Travel::answerBinder(bool yes) {
    if (m_binder == 0) {
        return;
    }
    if (yes) {
        m_body.clear();
        net::writeGuid(m_body, m_binder);
        send(Opcode::CMSG_BINDER_ACTIVATE);
    }
    m_binder = 0;
}

} // namespace vd::game
