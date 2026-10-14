#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// The packets of the character window of phase 15 (CharacterFrame, ReputationFrame, SkillFrame and
// Blizzard_InspectUI's honor tab) that no other area writes or reads, as vmangos-core 4641790 handles them
// for build 5875: the inspected player's honor (Server/Packets/Misc.{h,cpp} InspectHonorStats /
// InspectHonorStatsResponse, Handlers/MiscHandler.cpp HandleInspectHonorStatsOpcode), the watched faction and
// the inactive ones (Misc.cpp SetWatchedFaction / SetFactionInactive, Handlers/CharacterHandler.cpp
// HandleSetWatchedFactionOpcode / HandleSetFactionInactiveOpcode) and unlearning a profession
// (Packets/Skill.cpp UnlearnSkill, Handlers/SkillHandler.cpp HandleUnlearnSkillOpcode). Same rules as
// net/GameMessages: parsers fill a caller-owned struct and return false on a malformed packet, never reading
// past its end; builders append the body of a client packet (the opcode travels separately).
//
// Bodies covered elsewhere: CMSG_SET_FACTION_ATWAR (net/CombatMessages writeSetFactionAtWar).
namespace vd::net {

// --- The inspected player's honor (MSG_INSPECT_HONOR_STATS)
// ---------------------------------------------------

// Client: RequestInspectHonorData: the inspected player's full GUID. The server answers only within
// INSPECT_DISTANCE and for a player it may not attack.
void writeInspectHonorRequest(std::vector<u8>& out, u64 player);

// Server: what InspectHonorStatsResponse writes (50 bytes), read by WoW.exe 0x4C6E40 into the fields
// GetInspectHonorData returns. The "unknown" u16 after each honorable kill count is written as 0 by vMaNGOS
// (the dishonorable kills of old builds); WoW.exe reads them and never shows them.
struct InspectHonorStats {
    u64 player = 0;
    u8 highestRank =
        0; // HonorMgr's rank (0 none, 1..4 dishonored, 5..18 Private / Scout .. Grand Marshal / ...)
    u16 todayHonorable = 0; // PLAYER_FIELD_SESSION_KILLS: honorable, then dishonorable
    u16 todayDishonorable = 0;
    u16 yesterdayHonorable = 0; // PLAYER_FIELD_YESTERDAY_KILLS' low half
    u16 yesterdayUnused = 0;
    u16 lastWeekHonorable = 0;
    u16 lastWeekUnused = 0;
    u16 thisWeekHonorable = 0;
    u16 thisWeekUnused = 0;
    u32 lifetimeHonorable = 0;
    u32 lifetimeDishonorable = 0;
    u32 yesterdayHonor = 0; // contribution points
    u32 lastWeekHonor = 0;
    u32 thisWeekHonor = 0;
    u32 lastWeekStanding = 0;
    u8 rankProgress = 0; // PLAYER_FIELD_BYTES2 byte 0: 0..255 of the way to the next rank
};
bool parseInspectHonorStats(std::span<const u8> payload, InspectHonorStats& out);

// --- Reputation
// -------------------------------------------------------------------------------------------------

// SetWatchedFactionIndex: the reputation list id shown as the experience bar, -1 for none (the server writes
// it to PLAYER_FIELD_WATCHED_FACTION_INDEX as it comes).
void writeSetWatchedFaction(std::vector<u8>& out, i32 listId);
// SetFactionInactive / SetFactionActive: the reputation list id and 1 to move it to the Inactive header, 0 to
// bring it back (ReputationMgr::SetInactive refuses factions that are not visible). The server answers
// nothing.
void writeSetFactionInactive(std::vector<u8>& out, u32 listId, bool inactive);

// --- Skills
// -----------------------------------------------------------------------------------------------------

// AbandonSkill: the skill line (SkillLine.dbc). Obeyed only when SkillRaceClassInfo flags it
// SKILL_FLAG_UNLEARNABLE (0x20) for the player's race and class; the skill's fields then go to 0.
void writeUnlearnSkill(std::vector<u8>& out, u32 skillId);

} // namespace vd::net
