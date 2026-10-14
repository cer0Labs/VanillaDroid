#include "game/WorldStates.h"

#include <algorithm>

namespace vd::game {
namespace {

using net::Opcode;

constexpr usize kReservedStates = 256; // Alterac Valley sends ~150

bool byId(const net::WorldStatePair& a, u32 id) {
    return a.id < id;
}

} // namespace

WorldStates::WorldStates(Events& events) : m_events(events) {
    m_states.reserve(kReservedStates);
    m_init.states.reserve(kReservedStates);
}

void WorldStates::leaveWorld() {
    m_states.clear();
    m_mapId = 0;
    m_zoneId = 0;
    m_known = false;
    ++m_changes;
}

bool WorldStates::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_INIT_WORLD_STATES || opcode == Opcode::SMSG_UPDATE_WORLD_STATE;
}

bool WorldStates::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode == Opcode::SMSG_INIT_WORLD_STATES) {
        if (!net::parseInitWorldStates(payload, m_init)) {
            return false;
        }
        demoInit(m_init.mapId, m_init.zoneId, m_init.states, nowMs);
        return true;
    }
    if (opcode == Opcode::SMSG_UPDATE_WORLD_STATE) {
        net::WorldStatePair s;
        if (!net::parseUpdateWorldState(payload, s)) {
            return false;
        }
        demoSet(s.id, s.value, nowMs);
        return true;
    }
    return false;
}

void WorldStates::demoInit(u32 mapId, u32 zoneId, std::span<const net::WorldStatePair> states, u32 nowMs) {
    m_mapId = mapId;
    m_zoneId = zoneId;
    m_known = true;
    m_states.clear();
    for (const net::WorldStatePair& s : states) {
        set(s.id, s.value); // sorted, and a state sent twice keeps its last value
    }
    ++m_changes;
    event(0, 0, nowMs);
}

void WorldStates::demoSet(u32 id, u32 value, u32 nowMs) {
    set(id, value);
    ++m_changes;
    event(id, value, nowMs);
}

void WorldStates::set(u32 id, u32 value) {
    auto it = std::lower_bound(m_states.begin(), m_states.end(), id, byId);
    if (it != m_states.end() && it->id == id) {
        it->value = value;
    } else {
        m_states.insert(it, net::WorldStatePair{id, value});
    }
}

void WorldStates::event(u32 id, u32 value, u32 nowMs) {
    GameEvent& e = m_events.push(); // UPDATE_WORLD_STATES
    e = GameEvent{};
    e.kind = GameEventKind::WorldStates;
    e.timeMs = nowMs;
    e.id = id;
    e.value[0] = static_cast<i32>(value);
}

bool WorldStates::has(u32 id) const {
    auto it = std::lower_bound(m_states.begin(), m_states.end(), id, byId);
    return it != m_states.end() && it->id == id;
}

u32 WorldStates::value(u32 id, u32 fallback) const {
    auto it = std::lower_bound(m_states.begin(), m_states.end(), id, byId);
    return it != m_states.end() && it->id == id ? it->value : fallback;
}

} // namespace vd::game
