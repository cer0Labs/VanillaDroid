#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// Minimap packets (parity pass): the group's minimap ping, as vmangos-core 4641790 reads and writes it for
// build 5875 (Server/Packets/Group.cpp, Handlers/GroupHandler.cpp HandleMinimapPingOpcode,
// Objects/Creature.cpp). Same rules as net/GameMessages: parsers return false on a malformed packet and never
// read past its end; builders append a client packet's body.
namespace vd::net {

// MSG_MINIMAP_PING from the server: who pinged (a raw u64 GUID: a group member, or a creature of the player's
// that pings where it fights, CREATURE_STATIC_FLAG_COMBAT_PING) and the WORLD point (x, y) pinged. The server
// relays a member's ping to the rest of the group (never back to the sender) and drops it when the sender is
// in no group.
struct MinimapPing {
    u64 sender = 0;
    f32 x = 0.0f;
    f32 y = 0.0f;
};
// False when truncated or when a coordinate is not a finite number.
bool parseMinimapPing(std::span<const u8> payload, MinimapPing& out);
// MSG_MINIMAP_PING from the client: the world point (x, y) the player pinged.
void writeMinimapPing(std::vector<u8>& out, f32 x, f32 y);

} // namespace vd::net
