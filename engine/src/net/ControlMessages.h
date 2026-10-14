#pragma once

#include "core/Types.h"
#include "net/Messages.h"

#include <span>
#include <vector>

// Phase 13 (combat): who the client moves, as vmangos-core 4641790 writes and reads it for build 5875
// (Server/Packets/Misc: ClientControlUpdate, SetActiveMover, FarSight; Server/Packets/Movement: MoveTimeSkipped,
// MoveNotActiveMover; Handlers/MovementHandler.cpp, MiscHandler.cpp). Same rules as net/GameMessages: parsers return
// false on a malformed packet and never read past its end.
namespace vd::net {

// SMSG_CLIENT_CONTROL_UPDATE (Player::SetClientControl, sent by Unit::UpdateControl when a unit is feared,
// confused, charmed, possessed or freed, and by ModPossess when a possession ends): packed GUID + u8 whether this
// client may move that unit.
struct ClientControlUpdate {
    u64 guid = 0;
    bool allowMove = false;
};
bool parseClientControlUpdate(std::span<const u8> payload, ClientControlUpdate& out);

// MSG_MOVE_TIME_SKIPPED from the server (HandleMoveTimeSkippedOpcode relays a unit's skipped time to the others):
// packed GUID + u32 milliseconds.
struct MoveTimeSkipped {
    u64 guid = 0;
    u32 lagMs = 0;
};
bool parseMoveTimeSkipped(std::span<const u8> payload, MoveTimeSkipped& out);

// CMSG_MOVE_TIME_SKIPPED: the mover's full GUID + the milliseconds its movement did not simulate (the server
// shifts the mover's last client and server times by them, so the next packet's clock stays consistent).
void writeMoveTimeSkipped(std::vector<u8>& out, u64 mover, u32 lagMs);
// CMSG_MOVE_NOT_ACTIVE_MOVER (builds > 1.9.4): the full GUID of the unit the client stops moving + its last
// MovementInfo.
void writeMoveNotActiveMover(std::vector<u8>& out, u64 oldMover, const MovementInfo& info);
// CMSG_SET_ACTIVE_MOVER: the unit the client moves from now on (full GUID; 0 is not sent).
void writeSetActiveMover(std::vector<u8>& out, u64 mover);
// CMSG_FAR_SIGHT: u8 1 when the camera starts viewing PLAYER_FARSIGHT's object, 0 when it comes back.
void writeFarSight(std::vector<u8>& out, bool viewing);

} // namespace vd::net
