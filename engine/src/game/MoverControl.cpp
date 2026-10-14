#include "game/MoverControl.h"

#include "core/Log.h"
#include "game/Entities.h"
#include "game/UnitInfo.h"
#include "net/ControlMessages.h"
#include "net/TravelMessages.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {

using net::Opcode;

void MoverControl::enterWorld(u64 player, u32 nowMs) {
    m_nowMs = nowMs;
    const bool regained = m_player == player && !playerHasControl();
    m_player = player;
    m_flags.fill(Flag{});
    m_flags[0] = {player, true};
    m_farsight = 0;
    m_viewingFarSight = false;
    m_view = m_wanted = m_active = player;
    m_body.reserve(64);
    if (regained) { // control taken before a far teleport (the end of a battleground) comes back with it
        GameEvent& e = m_events.push();
        e = GameEvent{};
        e.kind = GameEventKind::ControlGained;
        e.timeMs = nowMs;
    }
}

void MoverControl::leaveWorld() {
    m_player = 0;
    m_flags.fill(Flag{});
    m_farsight = 0;
    m_viewingFarSight = false;
    m_view = m_wanted = m_active = 0;
}

bool MoverControl::hasControl(u64 guid) const {
    for (const Flag& f : m_flags) {
        if (f.guid == guid && guid != 0) {
            return f.allowed;
        }
    }
    return false;
}

void MoverControl::setFlag(u64 guid, bool allowed) {
    Flag* slot = nullptr;
    for (Flag& f : m_flags) {
        if (f.guid == guid) {
            slot = &f;
            break;
        }
        if (!slot && f.guid == 0) {
            slot = &f;
        }
    }
    if (!slot) {
        // Full (eight units handed over in one world): forget the oldest other than the player.
        for (u32 i = 1; i + 1 < m_flags.size(); ++i) {
            m_flags[i] = m_flags[i + 1];
        }
        slot = &m_flags.back();
    }
    slot->guid = guid;
    slot->allowed = allowed;
}

bool MoverControl::onControlUpdate(std::span<const u8> payload, u32 nowMs) {
    net::ClientControlUpdate m;
    if (!net::parseClientControlUpdate(payload, m)) {
        return false;
    }
    m_nowMs = nowMs;
    const bool before = hasControl(m.guid);
    setFlag(m.guid, m.allowMove);
    VD_LOG_INFO("Client control of %llx: %s", static_cast<unsigned long long>(m.guid),
                m.allowMove ? "allowed" : "taken");
    // The mover it implies (the camera's unit when allowed, else no unit) is settled by update() after the packets.
    if (m.guid == m_player && before != m.allowMove) {
        GameEvent& e = m_events.push();
        e = GameEvent{};
        e.kind = m.allowMove ? GameEventKind::ControlGained : GameEventKind::ControlLost;
        e.timeMs = nowMs;
        e.guid = m.guid;
    }
    return true;
}

void MoverControl::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (m_player == 0) {
        return;
    }
    const Entity* me = m_entities.find(m_player);
    const u64 farsight = me ? static_cast<u64>(me->value(net::uf::PLAYER_FARSIGHT)) |
                                  (static_cast<u64>(me->value(static_cast<u16>(net::uf::PLAYER_FARSIGHT + 1))) << 32)
                            : 0;
    m_farsight = farsight;
    if (farsight == 0 && m_viewingFarSight) { // back to the player
        m_viewingFarSight = false;
        sendFarSight(false);
    }
    const Entity* far = farsight ? m_entities.find(farsight) : nullptr;
    if (far) {
        if (!m_viewingFarSight) { // once per far sight, whatever objects it goes through
            m_viewingFarSight = true;
            sendFarSight(true);
        }
        m_view = farsight;
        m_wanted = isUnit(*far) && hasControl(farsight) ? farsight : (playerHasControl() ? m_player : 0);
    } else {
        m_view = m_player; // also while the far sight object is not here yet
        m_wanted = playerHasControl() ? m_player : 0;
    }
}

void MoverControl::clearFarSightImmediate(u32 nowMs) {
    if (m_viewingFarSight) {
        update(nowMs);
    }
}

void MoverControl::sendFarSight(bool viewing) {
    m_body.clear();
    net::writeFarSight(m_body, viewing);
    m_sink(Opcode::CMSG_FAR_SIGHT, m_body);
}

void MoverControl::switchMover(const Switch& s) {
    if (m_active == m_player && m_active != 0 && s.oldInfo && !s.oldOnSpline) {
        m_body.clear();
        net::writeMoveNotActiveMover(m_body, m_active, *s.oldInfo);
        m_sink(Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER, m_body);
    }
    VD_LOG_INFO("Active mover %llx -> %llx", static_cast<unsigned long long>(m_active),
                static_cast<unsigned long long>(m_wanted));
    m_active = m_wanted;
    if (m_active == 0) {
        return;
    }
    m_body.clear();
    net::writeSetActiveMover(m_body, m_active);
    m_sink(Opcode::CMSG_SET_ACTIVE_MOVER, m_body);
    if (s.newFinishedSpline != 0 && s.newInfo) {
        m_body.clear();
        net::writeMoveSplineDone(m_body, *s.newInfo, s.newFinishedSpline);
        m_sink(Opcode::CMSG_MOVE_SPLINE_DONE, m_body);
    }
}

void MoverControl::timeSkipped(u32 ms) {
    if (m_active == 0 || ms == 0) {
        return;
    }
    m_body.clear();
    net::writeMoveTimeSkipped(m_body, m_active, ms);
    m_sink(Opcode::CMSG_MOVE_TIME_SKIPPED, m_body);
}

} // namespace vd::game
