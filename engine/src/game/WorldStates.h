#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/BattlegroundMessages.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <vector>

namespace vd::game {

// Phase 17 (lead): the world states of the player's map and zone — the variables the server keeps for a
// battleground (flags captured, resources, bases), an outdoor PvP zone (Silithus' silithyst, the Eastern
// Plaguelands' towers) or an event (the Scourge invasion, the war effort), which WorldStateUI.dbc turns into the
// interface's WorldStateAlwaysUpFrame lines, capture bars and score columns (GetNumWorldStateUI /
// GetWorldStateUIInfo: the phase-17 scores package).
//  - SMSG_INIT_WORLD_STATES (Player::SendInitWorldStates, on every zone change and world entry) replaces the whole
//    set with the new map and zone's.
//  - SMSG_UPDATE_WORLD_STATE sets one (adds it when it was not in the set).
// Both push a WorldStates event (UPDATE_WORLD_STATES). Leaving the world empties the set. WoW.exe's handlers are
// in the dispatcher 0x48F690: case 0x2C2 (INIT) calls 0x4C5650 (every value of the hash freed, the map and zone
// kept, the WorldStateUI list rebuilt: game/WorldStateUi), then sets each pair (0x4C5870); case 0x2C3 (UPDATE) sets
// one; both then fire UPDATE_WORLD_STATES (0x20E). A state not in the set reads 0 (0x4C5810). Leaving the world
// (0x491222 -> 0x4C55B0) empties it. Main thread; the set keeps its capacity (no allocation once warmed up).
//
// Events (Gameplay::events()): WorldStates (id = the state that changed, 0 for a whole new set; value[0] = its
// value).
class WorldStates {
public:
    using Events = EventRing<GameEvent, 64>;

    explicit WorldStates(Events& events);

    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // SMSG_INIT_WORLD_STATES / SMSG_UPDATE_WORLD_STATE. False when malformed (nothing changes then).
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // The map and zone of the last SMSG_INIT_WORLD_STATES (0 before one).
    u32 mapId() const { return m_mapId; }
    u32 zoneId() const { return m_zoneId; }
    // Phase 17 (scores): an INIT came since the world entry (WoW.exe's map 0xB71E84 is -1 before: no WorldStateUI
    // list is built until then).
    bool known() const { return m_known; }
    bool has(u32 id) const;
    // The state's value, `fallback` when it is not in the set (WoW.exe reads a missing state as 0).
    u32 value(u32 id, u32 fallback = 0) const;
    // Every state, sorted by id.
    std::span<const net::WorldStatePair> states() const { return m_states; }
    // Packets applied so far (a view rebuilds when it moves).
    u32 changes() const { return m_changes; }

    // Development (gallery pages, VD_* switches): as if the server had sent them.
    void demoInit(u32 mapId, u32 zoneId, std::span<const net::WorldStatePair> states, u32 nowMs);
    void demoSet(u32 id, u32 value, u32 nowMs);

private:
    void set(u32 id, u32 value);
    void event(u32 id, u32 value, u32 nowMs);

    Events& m_events;
    u32 m_mapId = 0;
    u32 m_zoneId = 0;
    bool m_known = false;
    std::vector<net::WorldStatePair> m_states; // sorted by id
    net::InitWorldStates m_init;               // parse scratch (keeps its capacity)
    u32 m_changes = 0;
};

} // namespace vd::game
