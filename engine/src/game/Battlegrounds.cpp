#include "game/Battlegrounds.h"

#include "game/AreaData.h"
#include "game/Travel.h"

namespace vd::game {
namespace {

using net::Opcode;

constexpr u32 kMapTypeBattleground = 3; // Map.dbc InstanceType (MAP_BATTLEGROUND)

BattlefieldState stateOf(u32 status) {
    switch (status) {
    case net::kBattlefieldStatusNone: return BattlefieldState::None;
    case net::kBattlefieldStatusWaitQueue: return BattlefieldState::Queued;
    case net::kBattlefieldStatusWaitJoin: return BattlefieldState::Confirm;
    case net::kBattlefieldStatusInProgress: return BattlefieldState::Active;
    default: return BattlefieldState::Error;
    }
}

} // namespace

const char* battlefieldStateName(BattlefieldState state) {
    switch (state) {
    case BattlefieldState::None: return "none";
    case BattlefieldState::Queued: return "queued";
    case BattlefieldState::Confirm: return "confirm";
    case BattlefieldState::Active: return "active";
    case BattlefieldState::Error: return "error";
    }
    return "error";
}

Battlegrounds::Battlegrounds(const Travel& travel, Events& events) : m_travel(travel), m_events(events) {
    m_body.reserve(16);
}

void Battlegrounds::enterWorld(u64 player, u32 map, bool newCharacter, u32 nowMs) {
    (void)nowMs;
    if (newCharacter) {
        m_queues = {};
        m_activeSlot = -1;
        m_instanceExpires = false;
        m_instanceStarted = false;
    }
    m_inWorld = true;
    m_player = player;
    m_map = map;
    m_winner = -1; // 0x4A9DB0 clears the score
    m_ended = false;
    m_body.clear();
    sendBody(Opcode::CMSG_BATTLEFIELD_STATUS);
}

void Battlegrounds::leaveWorld() {
    m_inWorld = false;
}

bool Battlegrounds::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_BATTLEFIELD_STATUS;
}

bool Battlegrounds::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode != Opcode::SMSG_BATTLEFIELD_STATUS) {
        return false;
    }
    net::BattlefieldStatus s;
    if (!net::parseBattlefieldStatus(payload, s)) {
        return false;
    }
    apply(s, nowMs);
    return true;
}

void Battlegrounds::demoStatus(const net::BattlefieldStatus& status, u32 nowMs) {
    apply(status, nowMs);
}

void Battlegrounds::apply(const net::BattlefieldStatus& s, u32 nowMs) {
    if (s.queueSlot >= kQueues) {
        return; // 0x4AA86E: not even the event
    }
    BattlefieldQueue& q = m_queues[s.queueSlot];
    const BattlefieldState before = q.state;
    const i32 slot = static_cast<i32>(s.queueSlot);
    if (s.mapId == 0) {
        q = BattlefieldQueue{};
        if (m_activeSlot == slot) {
            m_activeSlot = -1; // 0x4AAAEC: the instance's times go with it (the slot index itself stays in WoW.exe,
            m_instanceExpires = false; // but nothing reads it while the slot is empty)
            m_instanceStarted = false;
        }
    } else {
        q.mapId = s.mapId;
        q.bracketId = s.bracketId;
        q.instanceId = s.instanceId;
        q.state = stateOf(s.status);
        q.portExpires = q.state == BattlefieldState::Confirm && s.time1 != 0;
        q.portExpiresAtMs = q.portExpires ? nowMs + s.time1 : 0;
        if (q.state == BattlefieldState::Active) {
            m_activeSlot = slot;
            m_instanceExpires = s.time1 != 0;
            m_instanceExpiresAtMs = nowMs + s.time1;
            m_instanceStarted = s.time2 != 0;
            m_instanceStartedAtMs = nowMs - s.time2;
        } else {
            m_instanceExpires = false; // 0x4AAA66: cleared by any other status, whatever the slot
            m_instanceStarted = false;
            if (m_activeSlot == slot) {
                m_activeSlot = -1;
            }
        }
        if (q.state == BattlefieldState::Queued) {
            q.averageWaitMs = s.time1;
            q.queuedAtMs = nowMs - s.time2;
        } else {
            q.averageWaitMs = 0;
            q.queuedAtMs = 0;
        }
    }
    ++m_changes;
    GameEvent& e = m_events.push(); // UPDATE_BATTLEFIELD_STATUS
    e = GameEvent{};
    e.kind = GameEventKind::BattlefieldStatus;
    e.timeMs = nowMs;
    e.id = s.queueSlot;
    e.value[0] = static_cast<i32>(q.state);
    e.value[1] = static_cast<i32>(before);
}

const BattlefieldQueue& Battlegrounds::queue(u32 slot) const {
    static const BattlefieldQueue kNone{};
    return slot < kQueues ? m_queues[slot] : kNone;
}

std::string_view Battlegrounds::mapName(u32 slot) const {
    const BattlefieldQueue& q = queue(slot);
    const AreaData* areas = m_travel.areas();
    const MapInfo* map = q.mapId != 0 && areas ? areas->map(q.mapId) : nullptr;
    return map ? map->name : std::string_view{};
}

u32 Battlegrounds::queueCount() const {
    u32 n = 0;
    for (const BattlefieldQueue& q : m_queues) {
        n += q.state != BattlefieldState::None ? 1 : 0;
    }
    return n;
}

u32 Battlegrounds::timeWaitedMs(u32 slot, u32 nowMs) const {
    const BattlefieldQueue& q = queue(slot);
    return q.state == BattlefieldState::Queued ? nowMs - q.queuedAtMs : 0;
}

u32 Battlegrounds::portExpirationMs(u32 slot, u32 nowMs) const {
    const BattlefieldQueue& q = queue(slot);
    if (!q.portExpires) {
        return 0;
    }
    const i32 left = static_cast<i32>(q.portExpiresAtMs - nowMs);
    return left > 0 ? static_cast<u32>(left) : 0;
}

u32 Battlegrounds::instanceExpirationMs(u32 nowMs) const {
    if (!m_instanceExpires) {
        return 0;
    }
    const i32 left = static_cast<i32>(m_instanceExpiresAtMs - nowMs);
    return left > 0 ? static_cast<u32>(left) : 0;
}

u32 Battlegrounds::instanceRunTimeMs(u32 nowMs) const {
    return m_instanceStarted ? nowMs - m_instanceStartedAtMs : 0;
}

bool Battlegrounds::inBattleground() const {
    const AreaData* areas = m_travel.areas();
    const MapInfo* map = areas ? areas->map(m_map) : nullptr;
    return map && map->instanceType == kMapTypeBattleground;
}

bool Battlegrounds::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool Battlegrounds::acceptPort(u32 slot, bool accept) {
    const BattlefieldQueue& q = queue(slot);
    if (q.mapId == 0) {
        return false;
    }
    m_body.clear();
    net::writeBattlefieldPort(m_body, q.mapId, accept);
    return sendBody(Opcode::CMSG_BATTLEFIELD_PORT);
}

bool Battlegrounds::leave() {
    // Phase 17 (scores): 0x4ABE60 sends nothing before the score's "ended", then the active queue's map.
    if (!m_ended) {
        return false;
    }
    m_body.clear();
    net::writeLeaveBattlefield(m_body, m_activeSlot >= 0 ? m_queues[static_cast<u32>(m_activeSlot)].mapId : 0);
    return sendBody(Opcode::CMSG_LEAVE_BATTLEFIELD);
}

} // namespace vd::game
