#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <vector>

namespace vd::game {

class EntityStore;

// Phase 13 (combat): which unit this client moves and where its camera looks, as WoW.exe 1.12.1 decides them
// (read-only disassembly):
// - SMSG_CLIENT_CONTROL_UPDATE (handler 0x603EA0 -> CGUnit_C::SetClientControl 0x5FA600) sets the unit's "client
//   control" flag. When the camera follows that unit, being allowed makes it the active mover and not being allowed
//   leaves the client with no mover. For the player it fires PLAYER_CONTROL_LOST / _GAINED (UIParent closes the
//   windows; the OutOfControl flash).
// - PLAYER_FARSIGHT (CGPlayer_C, 0x5EE290) moves the camera to that object (CMSG_FAR_SIGHT 1 when it starts viewing
//   it, 0 when it comes back); a unit with client control becomes the mover (Mind Control, Eyes of the Beast, Eye of
//   Kilrogg); without one, the player when it has control.
// - CGUnit_C::SetActiveMover (0x6006E0): CMSG_MOVE_NOT_ACTIVE_MOVER for the old mover when it is the player and is
//   not following an unfinished server spline, CMSG_SET_ACTIVE_MOVER for the new one (none for "no mover"), then
//   CMSG_MOVE_SPLINE_DONE for the new one's last spline if it ended while another client side mover was active. The
//   server (MovementHandler.cpp) expects exactly that order: NOT_ACTIVE_MOVER is checked against the mover it had.
// The movement itself (who is simulated, what is sent) is app/PlayerController's: it calls switchMover() when
// wantedMover() differs from activeMover(). Main thread; no allocation per frame.
class MoverControl {
public:
    using Events = EventRing<GameEvent, 64>;
    MoverControl(const EntityStore& entities, Events& events) : m_entities(entities), m_events(events) {}

    void setSink(PacketSink sink) { m_sink = sink; }
    // A new world (login, another map): the player has control and is the mover (GameSession sends its
    // CMSG_SET_ACTIVE_MOVER); a possession or a far sight does not outlive a far teleport (BattleGround.cpp: "the
    // effect will be automatically removed by client when the player is teleported").
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    // SMSG_CLIENT_CONTROL_UPDATE. False if malformed.
    bool onControlUpdate(std::span<const u8> payload, u32 nowMs);
    // Once per frame after the packets: the camera's view point from PLAYER_FARSIGHT and the mover it implies;
    // CMSG_FAR_SIGHT when the view starts or stops being the far sight object.
    void update(u32 nowMs);
    // Phase 18 (motion): SMSG_CLEAR_FAR_SIGHT_IMMEDIATE (no payload; vMaNGOS never sends it): WoW.exe (0x5E43F8 ->
    // CGPlayer_C 0x5EE590) takes PLAYER_FARSIGHT again at once when the camera views the far sight object, without
    // waiting for its next update (0x5EE290: the view back to the player and CMSG_FAR_SIGHT 0 once the field is 0).
    void clearFarSightImmediate(u32 nowMs);
    // Phase 18 (motion): the camera views the far sight object (CMSG_FAR_SIGHT 1 sent for it).
    bool viewingFarSight() const { return m_viewingFarSight; }

    u64 player() const { return m_player; }
    // False while feared, confused, charmed or possessed (PLAYER_CONTROL_LOST until PLAYER_CONTROL_GAINED).
    bool playerHasControl() const { return hasControl(m_player); }
    // The unit's client control flag (the player's is on by default).
    bool hasControl(u64 guid) const;
    // What the camera follows: PLAYER_FARSIGHT's object once it is known, else the player.
    u64 viewTarget() const { return m_view; }
    // The unit the client should move now (0: none: control lost) and the one the server was last told.
    u64 wantedMover() const { return m_wanted; }
    u64 activeMover() const { return m_active; }
    bool switchPending() const { return m_wanted != m_active; }

    // The switch to wantedMover(), with what the movement simulation knows: the old mover's last movement and
    // whether it is following an unfinished server spline, the new mover's movement and its last spline when that
    // one ended (0: none, or still running).
    struct Switch {
        const net::MovementInfo* oldInfo = nullptr;
        bool oldOnSpline = false;
        const net::MovementInfo* newInfo = nullptr;
        u32 newFinishedSpline = 0;
    };
    void switchMover(const Switch& s);
    // CMSG_MOVE_TIME_SKIPPED for the active mover: `ms` of its movement were not simulated (CMovement's update steps
    // at most 250 ms, 0x616620: a longer frame, a pause, a loading hitch reports the rest).
    void timeSkipped(u32 ms);
    static constexpr u32 kMaxStepMs = 250;

private:
    struct Flag {
        u64 guid = 0;
        bool allowed = false;
    };
    void setFlag(u64 guid, bool allowed);
    void sendFarSight(bool viewing);

    const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    u64 m_player = 0;
    std::array<Flag, 8> m_flags{}; // the units the server gave or took control of; the player's first
    u64 m_farsight = 0;            // PLAYER_FARSIGHT
    bool m_viewingFarSight = false; // CMSG_FAR_SIGHT 1 sent for it
    u64 m_view = 0;
    u64 m_wanted = 0;
    u64 m_active = 0;
    u32 m_nowMs = 0;
};

} // namespace vd::game
