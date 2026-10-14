#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <vector>

namespace vd::game {

// A summon offered to the player (parity pass, popups): SMSG_SUMMON_REQUEST (a warlock's Ritual of Summoning,
// a meeting stone, a GM), which the client asks about in its CONFIRM_SUMMON popup until the offer expires.
// Only an acceptance exists in 1.12 (CMSG_SUMMON_RESPONSE; vmangos' handler always agrees, and refuses a dead
// player or one in combat): declining just lets it expire. Main thread.
//
// Events (Gameplay::events()): SummonRequested.
class Summon {
public:
    using Events = EventRing<GameEvent, 64>;

    explicit Summon(Events& events) : m_events(events) { m_body.reserve(16); }

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld() { m_inWorld = true; }
    void leaveWorld();

    // SMSG_SUMMON_REQUEST. False when malformed.
    bool handle(std::span<const u8> payload, u32 nowMs);

    // The offer (0: none, or it expired).
    u64 summoner(u32 nowMs) const;
    u32 zone() const { return m_zone; } // AreaTable id of the summoner's zone
    u32 remainingMs(u32 nowMs) const;   // until it expires
    // ConfirmSummon: CMSG_SUMMON_RESPONSE. False when there is no offer.
    bool accept(u32 nowMs);
    void decline() { m_summoner = 0; } // CONFIRM_SUMMON's cancel: forgotten (the server lets it expire)

private:
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_summoner = 0;
    u32 m_zone = 0;
    u32 m_expiresAtMs = 0;
};

} // namespace vd::game
