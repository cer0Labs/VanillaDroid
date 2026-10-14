#include "game/Logout.h"

#include "net/PopupMessages.h"

namespace vd::game {

using net::Opcode;

void Logout::enterWorld() {
    m_inWorld = true;
    m_phase = LogoutPhase::None;
    m_quit = false;
    m_endsAtMs = 0;
}

void Logout::leaveWorld() {
    m_inWorld = false;
    m_phase = LogoutPhase::None;
    m_endsAtMs = 0; // m_quit stays: the session reads it after SMSG_LOGOUT_COMPLETE
}

bool Logout::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_LOGOUT_RESPONSE || opcode == Opcode::SMSG_LOGOUT_CANCEL_ACK;
}

GameEvent& Logout::event(GameEventKind kind, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = nowMs;
    return e;
}

bool Logout::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode == Opcode::SMSG_LOGOUT_CANCEL_ACK) {
        // LOGOUT_CANCEL: the player stands up and may move again (the server undoes the root and the
        // sitting).
        if (m_phase != LogoutPhase::None) {
            m_phase = LogoutPhase::None;
            event(GameEventKind::LogoutCancelled, nowMs);
        }
        return true;
    }
    net::LogoutResponse response;
    if (!net::parseLogoutResponse(payload, response)) {
        return false;
    }
    if (response.reason != 0) {
        m_phase = LogoutPhase::None;
        event(GameEventKind::LogoutRefused, nowMs).value[0] = static_cast<i32>(response.reason);
    } else if (response.instant) {
        m_phase = LogoutPhase::Instant;
        event(GameEventKind::LogoutInstant, nowMs).value[0] = m_quit ? 1 : 0;
    } else if (m_phase == LogoutPhase::Cancelling) {
        // Cancelled before the answer came: the server started its countdown and its cancel ack follows.
        m_endsAtMs = nowMs + kCountdownMs;
    } else {
        // PLAYER_CAMPING / PLAYER_QUITING: the popup counts the server's 20 s from now.
        m_phase = LogoutPhase::Countdown;
        m_endsAtMs = nowMs + kCountdownMs;
        event(GameEventKind::LogoutStarted, nowMs).value[0] = m_quit ? 1 : 0;
    }
    return true;
}

bool Logout::request(bool quit, u32 nowMs) {
    (void)nowMs;
    if (!m_inWorld || m_phase != LogoutPhase::None) {
        return false;
    }
    if (!m_sink(Opcode::CMSG_LOGOUT_REQUEST, {})) {
        return false;
    }
    m_phase = LogoutPhase::Requested;
    m_quit = quit;
    return true;
}

bool Logout::cancel() {
    if (!m_inWorld || (m_phase != LogoutPhase::Requested && m_phase != LogoutPhase::Countdown)) {
        return false;
    }
    if (!m_sink(Opcode::CMSG_LOGOUT_CANCEL, {})) {
        return false;
    }
    m_phase = LogoutPhase::Cancelling;
    return true;
}

u32 Logout::remainingMs(u32 nowMs) const {
    if (m_phase != LogoutPhase::Countdown || static_cast<i32>(m_endsAtMs - nowMs) <= 0) {
        return 0;
    }
    return m_endsAtMs - nowMs;
}

} // namespace vd::game
