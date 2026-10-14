#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// The packets behind the client's StaticPopups that no other area parses (parity pass, popups): logging out
// (CAMP / QUIT) and summons (CONFIRM_SUMMON), as vmangos-core 4641790 writes and reads them for build 5875
// (Server/Packets/Misc: LogoutResponse, LogoutComplete, LogoutCancelAck, SummonRequest, SummonResponse;
// handlers in Handlers/MiscHandler.cpp and MovementHandler.cpp). CMSG_LOGOUT_REQUEST, CMSG_LOGOUT_CANCEL and
// CMSG_SELF_RES have empty bodies, SMSG_LOGOUT_COMPLETE and SMSG_LOGOUT_CANCEL_ACK too. Same rules as
// net/GameMessages: parsers return false on a malformed packet and never read past its end.
namespace vd::net {

// SMSG_LOGOUT_RESPONSE (5 bytes): the answer to CMSG_LOGOUT_REQUEST
// (WorldSession::HandleLogoutRequestOpcode). reason 0 and instant 1: logged out at once (resting, on a taxi,
// a GM account), SMSG_LOGOUT_COMPLETE follows; reason 0 and instant 0: the 20 s countdown runs (the player
// sits, rooted); any other reason is a refusal.
struct LogoutResponse {
    u32 reason = 0; // kLogoutRefused*
    bool instant = false;
};
// The server's refusals (vmangos writes these three; its comment calls 2 a guess).
constexpr u32 kLogoutRefusedCombat = 1;  // in combat
constexpr u32 kLogoutRefusedFrozen = 2;  // frozen by a GM (aura 9454)
constexpr u32 kLogoutRefusedFalling = 3; // jumping or falling
bool parseLogoutResponse(std::span<const u8> payload, LogoutResponse& out);

// SMSG_SUMMON_REQUEST (16 bytes): someone summons the player (Player::SendSummonRequest).
struct SummonRequest {
    u64 summoner = 0;      // a full GUID
    u32 zoneId = 0;        // AreaTable: the summoner's zone
    u32 autoDeclineMs = 0; // MAX_PLAYER_SUMMON_DELAY: two minutes
};
bool parseSummonRequest(std::span<const u8> payload, SummonRequest& out);
// CMSG_SUMMON_RESPONSE: the summoner's GUID. Only an acceptance exists in 1.12 (the handler always agrees); a
// declined summon is never answered and expires on the server.
void writeSummonResponse(std::vector<u8>& out, u64 summoner);

} // namespace vd::net
