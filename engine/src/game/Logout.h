#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <span>

namespace vd::game {

// Where a logout stands (parity pass, popups).
enum class LogoutPhase : u8 {
    None,
    Requested,  // CMSG_LOGOUT_REQUEST sent, no answer yet
    Countdown,  // the server's 20 s run: the player sits, rooted (the CAMP / QUIT popup counts down)
    Instant,    // the server logs out at once (resting, on a taxi, a GM): SMSG_LOGOUT_COMPLETE is on its way
    Cancelling, // CMSG_LOGOUT_CANCEL sent, waiting for SMSG_LOGOUT_CANCEL_ACK
};

// Logging out as the 1.12 client does it (GameMenuFrame's Logout() / Quit(), UIParent's PLAYER_CAMPING /
// PLAYER_QUITING / LOGOUT_CANCEL): the player asks (CMSG_LOGOUT_REQUEST), the server refuses (in combat,
// falling, frozen), logs out at once (resting, on a taxi, a GM account) or starts its 20 s countdown, during
// which the player may cancel (CMSG_LOGOUT_CANCEL / SMSG_LOGOUT_CANCEL_ACK). The end, SMSG_LOGOUT_COMPLETE,
// takes the session back to the character list (app/GameSession). vmangos:
// WorldSession::HandleLogoutRequestOpcode, HandleLogoutCancelOpcode, LogoutPlayer; ShouldLogOut counts the 20
// s. Main thread; nothing allocates.
//
// Events (Gameplay::events()): LogoutRefused, LogoutStarted, LogoutInstant, LogoutCancelled.
class Logout {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kCountdownMs = 20000; // the server's (WorldSession::ShouldLogOut) and CAMP's timeout

    explicit Logout(Events& events) : m_events(events) {}

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld();
    void leaveWorld();

    static bool handles(net::Opcode opcode);
    // SMSG_LOGOUT_RESPONSE, SMSG_LOGOUT_CANCEL_ACK. False when malformed (nothing changes then).
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // CMSG_LOGOUT_REQUEST. `quit`: the game ends once logged out (Quit(): the QUIT popup instead of CAMP).
    // False when not in the world or a logout is already under way.
    bool request(bool quit, u32 nowMs);
    // CMSG_LOGOUT_CANCEL (CancelLogout): while asked or counting down. False when there is nothing to cancel.
    bool cancel();

    LogoutPhase phase() const { return m_phase; }
    bool pending() const { return m_phase != LogoutPhase::None; }
    bool quitting() const { return m_quit; } // the last request was Quit()
    // The countdown's time left (0 when none runs).
    u32 remainingMs(u32 nowMs) const;

private:
    GameEvent& event(GameEventKind kind, u32 nowMs);

    Events& m_events;
    PacketSink m_sink;
    bool m_inWorld = false;
    LogoutPhase m_phase = LogoutPhase::None;
    bool m_quit = false;
    u32 m_endsAtMs = 0;
};

} // namespace vd::game
