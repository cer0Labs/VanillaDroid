#include "game/Summon.h"

#include "net/PopupMessages.h"

namespace vd::game {

void Summon::leaveWorld() {
    m_inWorld = false;
    m_summoner = 0; // the server forgets it too (another map, logged out)
}

bool Summon::handle(std::span<const u8> payload, u32 nowMs) {
    net::SummonRequest request;
    if (!net::parseSummonRequest(payload, request)) {
        return false;
    }
    m_summoner = request.summoner;
    m_zone = request.zoneId;
    m_expiresAtMs = nowMs + request.autoDeclineMs;
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::SummonRequested;
    e.timeMs = nowMs;
    e.guid = request.summoner;
    e.id = request.zoneId;
    e.value[0] = static_cast<i32>(request.autoDeclineMs);
    return true;
}

u64 Summon::summoner(u32 nowMs) const {
    return remainingMs(nowMs) > 0 ? m_summoner : 0;
}

u32 Summon::remainingMs(u32 nowMs) const {
    if (m_summoner == 0 || static_cast<i32>(m_expiresAtMs - nowMs) <= 0) {
        return 0;
    }
    return m_expiresAtMs - nowMs;
}

bool Summon::accept(u32 nowMs) {
    const u64 summoner = this->summoner(nowMs);
    if (!m_inWorld || summoner == 0) {
        return false;
    }
    m_body.clear();
    net::writeSummonResponse(m_body, summoner);
    m_summoner = 0;
    return m_sink(net::Opcode::CMSG_SUMMON_RESPONSE, m_body);
}

} // namespace vd::game
