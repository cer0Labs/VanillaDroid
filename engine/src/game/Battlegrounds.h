#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/BattlegroundMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Travel;

// GetBattlefieldStatus' first result, from SMSG_BATTLEFIELD_STATUS's status (WoW.exe keeps the u32 as it came:
// 0 "none", 1 "queued", 2 "confirm", 3 "active", anything else "error").
enum class BattlefieldState : u8 { None, Queued, Confirm, Active, Error };
// Its Lua string ("none", "queued", "confirm", "active", "error").
const char* battlefieldStateName(BattlefieldState state);

// One of the three queues (WoW.exe 0xB6E9D0 + slot * 32, filled by the handler 0x4AA850).
struct BattlefieldQueue {
    BattlefieldState state = BattlefieldState::None;
    u32 mapId = 0;      // the battleground's Map.dbc id (GetBattlefieldStatus' mapName: its name)
    u8 bracketId = 0;   // the player's level bracket in it
    u32 instanceId = 0; // GetBattlefieldStatus' instanceID (0: "first available", not assigned yet)
    // Queued: GetBattlefieldEstimatedWaitTime (the server's average, ms; 0 = QUEUE_TIME_UNAVAILABLE) and the client
    // clock the wait started at (now - time2: GetBattlefieldTimeWaited).
    u32 averageWaitMs = 0;
    u32 queuedAtMs = 0;
    // Confirm: the client clock the invitation expires at (now + time1: GetBattlefieldPortExpiration); without one
    // (time1 0) the expiration reads 0, as WoW.exe stores 0.
    bool portExpires = false;
    u32 portExpiresAtMs = 0;
};

// Phase 17 (lead): the battleground queues as WoW.exe 1.12.1 keeps them (read-only disassembly; the Lua API of
// BattlefieldFrame.lua / WorldStateFrame.lua in brackets) against vMaNGOS' BattleGroundMgr and handlers:
//  - Every world entry (the PLAYER_ENTERING_WORLD sequence 0x4908C0 calls 0x4A9DB0) clears the battlefield list
//    and the score and sends CMSG_BATTLEFIELD_STATUS (empty); the server answers one SMSG_BATTLEFIELD_STATUS per
//    queue the player is in. A new character starts with the three queues empty.
//  - SMSG_BATTLEFIELD_STATUS (0x4AA850): a slot >= 3 is dropped. Map 0 empties the slot (and, when it was the
//    active one, the instance's times). Otherwise the slot takes the map, bracket, instance and status; "confirm"
//    keeps its expiration (now + time1, 0 for none), "queued" its average wait and its start (now - time2),
//    "active" makes it the active slot (0x8457CC) and keeps the instance's expiration (now + time1, 0 while it
//    runs) and start (now - time2) globally (0xB6EBB8 / 0xB6EBBC); another status on the active slot drops it.
//    Always UPDATE_BATTLEFIELD_STATUS (0x1A3) after: a BattlefieldStatus event.
//  - AcceptBattlefieldPort(i, accept): CMSG_BATTLEFIELD_PORT with the queue's map; LeaveBattlefield (0x4ABE60):
//    CMSG_LEAVE_BATTLEFIELD with the active queue's map (0 without one), and only once the score said the battle
//    ended (0xB6EBCC, the MSG_PVP_LOG_DATA handler's: setEnded).
// While a queue is active WoW.exe also asks MSG_BATTLEGROUND_PLAYER_POSITIONS every 5 s (0x4AA5C0) and the
// score frame MSG_PVP_LOG_DATA: the phase-17 packages own those (setWinner below is the score's hook).
// Main thread; nothing allocates after construction.
//
// Events (Gameplay::events()): BattlefieldStatus (id = the slot, value[0] = its BattlefieldState, value[1] = the
// state before).
class Battlegrounds {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kQueues = net::kBattlefieldQueueSlots;

    Battlegrounds(const Travel& travel, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    // Every world entry (also a new map): CMSG_BATTLEFIELD_STATUS. `newCharacter` empties the queues first.
    void enterWorld(u64 player, u32 map, bool newCharacter, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // SMSG_BATTLEFIELD_STATUS. False when malformed (nothing changes then).
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // GetBattlefieldStatus(slot + 1) and its map's name (Map.dbc; empty when unknown).
    const BattlefieldQueue& queue(u32 slot) const;
    std::string_view mapName(u32 slot) const;
    // How many queues are not "none" (MiniMapBattlefieldFrame shows while there is one).
    u32 queueCount() const;
    // GetBattlefieldTimeWaited (ms, queued) and GetBattlefieldPortExpiration (ms left, confirm; 0 when none).
    u32 timeWaitedMs(u32 slot, u32 nowMs) const;
    u32 portExpirationMs(u32 slot, u32 nowMs) const;
    // The "active" queue (0x8457CC), -1 when none.
    i32 activeSlot() const { return m_activeSlot; }
    // GetBattlefieldInstanceExpiration (ms until the instance closes after a victory; 0 while it runs) and
    // GetBattlefieldInstanceRunTime (ms since it started; 0 when unknown).
    u32 instanceExpirationMs(u32 nowMs) const;
    u32 instanceRunTimeMs(u32 nowMs) const;
    // IsInInstance's "pvp": the player's map is a battleground (Map.dbc instance type 3).
    bool inBattleground() const;
    u32 map() const { return m_map; }

    // AcceptBattlefieldPort(slot + 1, accept): enter the battle (confirm) or leave the queue. False when not sent
    // (an empty slot, not in the world).
    bool acceptPort(u32 slot, bool accept);
    // LeaveBattlefield. False when not sent (the battle has not ended, not in the world).
    bool leave();

    // GetBattlefieldWinner: -1 none, 0 Horde, 1 Alliance. The score handler (MSG_PVP_LOG_DATA) sets it; a new world
    // clears it.
    i32 winner() const { return m_winner; }
    void setWinner(i32 winner) { m_winner = winner; }
    // Phase 17 (scores): the score's "ended" (0xB6EBCC: MSG_PVP_LOG_DATA's first byte), LeaveBattlefield's
    // condition; a new world clears it. A winner of 2 is vMaNGOS' WINNER_NONE (a premature end without a side).
    bool ended() const { return m_ended; }
    void setEnded(bool ended) { m_ended = ended; }

    // Statuses applied so far (a view rebuilds when it moves).
    u32 changes() const { return m_changes; }

    // Development (gallery pages, VD_* switches): as if the server had sent this status.
    void demoStatus(const net::BattlefieldStatus& status, u32 nowMs);

private:
    void apply(const net::BattlefieldStatus& status, u32 nowMs);
    bool sendBody(net::Opcode opcode);

    const Travel& m_travel;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_map = 0;
    std::array<BattlefieldQueue, kQueues> m_queues{};
    i32 m_activeSlot = -1;
    bool m_instanceExpires = false; // 0xB6EBB8 != 0
    u32 m_instanceExpiresAtMs = 0;
    bool m_instanceStarted = false; // 0xB6EBBC != 0
    u32 m_instanceStartedAtMs = 0;
    i32 m_winner = -1;
    bool m_ended = false; // phase 17 (scores)
    u32 m_changes = 0;
};

} // namespace vd::game
