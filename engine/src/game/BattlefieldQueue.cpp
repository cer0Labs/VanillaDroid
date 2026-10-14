#include "game/BattlefieldQueue.h"

#include "game/AreaData.h"
#include "game/Battlegrounds.h"
#include "game/Entities.h"
#include "game/Social.h"
#include "game/Travel.h"

#include <algorithm>
#include <utility>

namespace vd::game {
namespace {

using net::Opcode;

constexpr u32 kMaxBracketLevel = 60;                // 0x4AA731: a bracket's top level is at most 60
constexpr f32 kNoPosition = 2.384185791015625e-07f; // 0x8029D4: a stored position that is not one

} // namespace

const char* battlefieldLineKey(BattlefieldLine line) {
    switch (line) {
    case BattlefieldLine::GroupJoined: return "ERR_GROUP_JOIN_BATTLEGROUND_S";
    case BattlefieldLine::GroupDeserters: return "ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS";
    case BattlefieldLine::GroupFailed: return "ERR_GROUP_JOIN_BATTLEGROUND_FAIL";
    case BattlefieldLine::GroupTooMany: return "ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY";
    case BattlefieldLine::InstanceShutdown: return "INSTANCE_SHUTDOWN_MESSAGE";
    case BattlefieldLine::InstanceComplete: return "INSTANCE_COMPLETE_MESSAGE";
    }
    return "ERR_GROUP_JOIN_BATTLEGROUND_FAIL";
}

u32 battlefieldLinePlace(BattlefieldLine line) {
    switch (line) {
    case BattlefieldLine::GroupJoined:
    case BattlefieldLine::GroupDeserters:
        return 1; // the table's type 1: UI_INFO_MESSAGE (0x4945B0, event 0xE1)
    case BattlefieldLine::GroupFailed:
    case BattlefieldLine::GroupTooMany: return 2; // type 2: UI_ERROR_MESSAGE (event 0xE0)
    default: return 0; // DEFAULT_CHAT_FRAME:AddMessage in ChatTypeInfo SYSTEM's colour
    }
}

BattlefieldQueueing::BattlefieldQueueing(const EntityStore& entities, const Social& social,
                                         const Travel& travel, const Battlegrounds& battlegrounds,
                                         Events& events)
    : m_entities(entities), m_social(social), m_travel(travel), m_battlegrounds(battlegrounds),
      m_events(events) {
    m_body.reserve(32);
    m_list.instanceIds.reserve(16);
    m_incoming.instanceIds.reserve(16);
}

void BattlefieldQueueing::enterWorld(u64 player, bool newCharacter, u32 nowMs) {
    // 0x4A9DB0: the list, its GUID, the selection and the stored position go.
    m_inWorld = true;
    m_player = player;
    m_nowMs = nowMs;
    m_list.battlemaster = 0;
    m_list.mapId = 0;
    m_list.bracketId = 0;
    m_list.instanceIds.clear();
    m_bracketMinLevel = m_bracketMaxLevel = 0;
    m_selectedInstance = 0;
    m_watching = false;
    m_shutdownMs = 0; // WorldStateScoreFrame's PLAYER_ENTERING_WORLD: BATTLEFIELD_SHUTDOWN_TIMER = 0
    if (newCharacter) {
        m_queuedCount = 0; // a new interface: CURRENT_BATTLEFIELD_QUEUES starts empty
        m_statusChanges = m_battlegrounds.changes();
    }
    ++m_changes;
}

void BattlefieldQueueing::leaveWorld() {
    m_inWorld = false;
    m_watching = false;
}

bool BattlefieldQueueing::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_BATTLEFIELD_LIST || opcode == Opcode::SMSG_GROUP_JOINED_BATTLEGROUND;
}

bool BattlefieldQueueing::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    m_nowMs = nowMs;
    if (opcode == Opcode::SMSG_BATTLEFIELD_LIST) {
        if (!net::parseBattlefieldList(payload, m_incoming)) {
            return false;
        }
        applyList(nowMs);
        return true;
    }
    if (opcode == Opcode::SMSG_GROUP_JOINED_BATTLEGROUND) {
        i32 result = 0;
        if (!net::parseGroupJoinedBattleground(payload, result)) {
            return false;
        }
        // 0x4AACC0: -2 the deserters, a map Map.dbc knows its name, anything else (-1) the failure.
        const AreaData* areas = m_travel.areas();
        if (result == net::kGroupJoinDeserters) {
            notice(BattlefieldLine::GroupDeserters, 0, 0, nowMs);
        } else if (result >= 0 && areas && areas->map(static_cast<u32>(result))) {
            notice(BattlefieldLine::GroupJoined, static_cast<u32>(result), 0, nowMs);
        } else {
            notice(BattlefieldLine::GroupFailed, 0, 0, nowMs);
        }
        return true;
    }
    return false;
}

void BattlefieldQueueing::demoList(const net::BattlefieldList& list, u32 nowMs) {
    m_incoming.battlemaster = list.battlemaster;
    m_incoming.mapId = list.mapId;
    m_incoming.bracketId = list.bracketId;
    m_incoming.instanceIds.assign(list.instanceIds.begin(), list.instanceIds.end());
    applyList(nowMs);
}

void BattlefieldQueueing::applyList(u32 nowMs) {
    std::swap(m_list, m_incoming); // both keep their vectors' capacity
    // The bracket's levels: Map.dbc MinLevel + bracket * the bracket's size, its last level at most 60; none
    // (0, 0) for a map without a bracket size.
    const AreaData* areas = m_travel.areas();
    const MapInfo* map = areas ? areas->map(m_list.mapId) : nullptr;
    if (map && map->bracketLevels > 0) {
        m_bracketMinLevel = map->minLevel + static_cast<u32>(m_list.bracketId) * map->bracketLevels;
        m_bracketMaxLevel = std::min(m_bracketMinLevel + map->bracketLevels - 1, kMaxBracketLevel);
    } else {
        m_bracketMinLevel = m_bracketMaxLevel = 0;
    }
    ++m_changes;
    event(GameEventKind::BattlefieldsShow, nowMs).id = m_list.mapId; // BATTLEFIELDS_SHOW
    // Where the player stands now: walking away from it closes the list.
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    m_watching = playerPosition(x, y, z, nowMs) && x * x + y * y + z * z > kNoPosition;
    m_listX = x;
    m_listY = y;
    m_listZ = z;
}

bool BattlefieldQueueing::playerPosition(f32& x, f32& y, f32& z, u32 nowMs) const {
    const Entity* me = m_player != 0 ? m_entities.find(m_player) : nullptr;
    if (!me) {
        return false;
    }
    const glm::vec3 p = me->positionAt(nowMs);
    x = p.x;
    y = p.y;
    z = p.z;
    return true;
}

void BattlefieldQueueing::update(u32 nowMs) {
    const u32 elapsed = m_updated ? nowMs - m_lastUpdateMs : 0;
    m_updated = true;
    m_lastUpdateMs = nowMs;
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    // 0x4AA090 (each frame): BATTLEFIELDS_CLOSED once the player is more than 5.56 yd from where the list
    // came.
    if (m_watching) {
        f32 x = 0.0f, y = 0.0f, z = 0.0f;
        if (playerPosition(x, y, z, nowMs)) {
            const f32 dx = x - m_listX, dy = y - m_listY, dz = z - m_listZ;
            if (dx * dx + dy * dy + dz * dz > kCloseDistance * kCloseDistance) {
                m_watching = false;
                event(GameEventKind::BattlefieldsClosed, nowMs).id = m_list.mapId;
            }
        }
    }
    if (m_battlegrounds.changes() != m_statusChanges) {
        m_statusChanges = m_battlegrounds.changes();
        syncStatus();
    }
    countDown(elapsed);
}

void BattlefieldQueueing::syncStatus() {
    // BattlefieldFrame_UpdateStatus(): the queues as GetBattlefieldStatus gives them now.
    m_shutdownMs = 0;
    const std::array<QueuedKey, 3> previous = m_queued;
    const u32 previousCount = m_queuedCount;
    m_queuedCount = 0;
    for (u32 slot = 0; slot < Battlegrounds::kQueues; ++slot) {
        const BattlefieldQueue& q = m_battlegrounds.queue(slot);
        switch (q.state) {
        case BattlefieldState::Queued: {
            // IsAlreadyInQueue(mapName): the map and its instance number, as the name compares them.
            bool already = false;
            for (u32 i = 0; i < previousCount; ++i) {
                already = already || (previous[i].mapId == q.mapId && previous[i].instanceId == q.instanceId);
            }
            if (!already) {
                event(GameEventKind::BattlefieldQueueJoined, m_nowMs).id = slot; // PVPENTERQUEUE, the shine
            }
            if (m_queuedCount < m_queued.size()) {
                m_queued[m_queuedCount++] = {q.mapId, q.instanceId};
            }
            break;
        }
        case BattlefieldState::Confirm:
            event(GameEventKind::BattlefieldConfirm, m_nowMs).id = slot; // PVPTHROUGHQUEUE, the popup again
            break;
        case BattlefieldState::Active:
            m_shutdownMs = static_cast<i32>(m_battlegrounds.instanceExpirationMs(m_nowMs));
            m_thresholdIndex = 0;
            m_previousMod = 0;
            break;
        default: break; // "error": "Should never happen haha"
        }
    }
}

void BattlefieldQueueing::countDown(u32 elapsedMs) {
    // BattlefieldFrame_OnUpdate(elapsed).
    if (m_shutdownMs == 0) {
        return;
    }
    m_shutdownMs -= static_cast<i32>(elapsedMs);
    m_checkDelayMs += elapsedMs;
    if (m_checkDelayMs < kShutdownCheckMs) {
        return;
    }
    m_checkDelayMs = 0;
    if (m_shutdownMs <= 0) {
        m_shutdownMs = 0;
        return;
    }
    const u32 left = static_cast<u32>(m_shutdownMs);
    const u32 threshold = kShutdownThresholdsMs[m_thresholdIndex];
    if (left < threshold && m_thresholdIndex + 1 != kShutdownThresholdsMs.size()) {
        ++m_thresholdIndex; // past this threshold: the next one at the next check
        return;
    }
    const u32 mod = left / threshold;
    if (m_previousMod == mod) {
        return;
    }
    // format(..., SecondsToTime(ceil(timer / threshold) * threshold)); GetBattlefieldWinner: nil, or 0 / 1.
    const u32 seconds = (left + threshold - 1) / threshold * threshold / 1000;
    notice(m_battlegrounds.winner() >= 0 ? BattlefieldLine::InstanceComplete
                                         : BattlefieldLine::InstanceShutdown,
           m_battlegrounds.map(), seconds, m_nowMs);
    m_previousMod = mod;
}

bool BattlefieldQueueing::hello(u64 battlemaster) {
    m_body.clear();
    net::writeBattlemasterHello(m_body, battlemaster);
    return sendBody(Opcode::CMSG_BATTLEMASTER_HELLO);
}

bool BattlefieldQueueing::info(BattlefieldInfo& out) const {
    out = BattlefieldInfo{};
    const AreaData* areas = m_travel.areas();
    const MapInfo* map = areas && m_list.mapId != 0 ? areas->map(m_list.mapId) : nullptr;
    if (!map) {
        return false;
    }
    out.name = map->name;
    out.description =
        m_travel.playerTeam() == kAreaTeamHorde ? map->descriptionHorde : map->descriptionAlliance;
    out.minLevel = map->minLevel;
    out.maxLevel = map->maxLevel;
    out.maxPlayers = map->maxPlayers;
    out.bracketMinLevel = m_bracketMinLevel;
    out.bracketMaxLevel = m_bracketMaxLevel;
    out.groupJoin = map->groupJoin;
    return true;
}

bool BattlefieldQueueing::canJoinAsGroup() const {
    const AreaData* areas = m_travel.areas();
    const MapInfo* map = areas ? areas->map(m_list.mapId) : nullptr;
    return map && map->groupJoin;
}

bool BattlefieldQueueing::groupLeader() const {
    return (m_social.partyMemberCount() > 0 || m_social.raidMemberCount() > 0) && m_social.isLeader();
}

void BattlefieldQueueing::select(u32 index) {
    const u32 i = index - 1; // 0 wraps out of the list: "First Available"
    m_selectedInstance = i < count() ? m_list.instanceIds[i] : 0;
}

u32 BattlefieldQueueing::selected() const {
    for (u32 i = 0; i < count(); ++i) {
        if (m_list.instanceIds[i] == m_selectedInstance) {
            return i + 1;
        }
    }
    return 0;
}

bool BattlefieldQueueing::join(u32 index, bool asGroup, u32 nowMs) {
    if (m_list.mapId == 0) {
        return false; // no list shown (WoW.exe would send map 0: the server ignores it)
    }
    if (asGroup) {
        // More party or raid members than the map takes per side: refused here.
        const AreaData* areas = m_travel.areas();
        const MapInfo* map = areas ? areas->map(m_list.mapId) : nullptr;
        if (map &&
            (map->maxPlayers < m_social.partyMemberCount() || map->maxPlayers < m_social.raidMemberCount())) {
            notice(BattlefieldLine::GroupTooMany, m_list.mapId, 0, nowMs);
            return false;
        }
    }
    const u32 i = index - 1;
    const u32 instance = i < count() ? m_list.instanceIds[i] : 0;
    m_body.clear();
    if (m_list.battlemaster != 0) {
        net::writeBattlemasterJoin(m_body, m_list.battlemaster, m_list.mapId, instance, asGroup);
        return sendBody(Opcode::CMSG_BATTLEMASTER_JOIN);
    }
    net::writeBattlefieldJoin(m_body, m_list.mapId, instance, asGroup);
    return sendBody(Opcode::CMSG_BATTLEFIELD_JOIN);
}

bool BattlefieldQueueing::showList(u32 slot) {
    const BattlefieldQueue& q = m_battlegrounds.queue(slot);
    if (q.mapId == 0 || q.state != BattlefieldState::Queued) {
        return false;
    }
    m_body.clear();
    net::writeBattlefieldListRequest(m_body, q.mapId);
    return sendBody(Opcode::CMSG_BATTLEFIELD_LIST);
}

void BattlefieldQueueing::notice(BattlefieldLine line, u32 mapId, u32 seconds, u32 nowMs) {
    GameEvent& e = event(GameEventKind::BattlefieldNotice, nowMs);
    e.id = mapId;
    e.value[0] = static_cast<i32>(line);
    e.value[1] = static_cast<i32>(seconds);
}

GameEvent& BattlefieldQueueing::event(GameEventKind kind, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = nowMs;
    return e;
}

bool BattlefieldQueueing::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

} // namespace vd::game
