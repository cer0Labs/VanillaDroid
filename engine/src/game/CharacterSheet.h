#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "net/CharacterMessages.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Abilities;

// The game side of the character window of phase 15 that no other area owns, as WoW.exe and vmangos-core
// 4641790 do it (net/CharacterMessages):
// - the inspected player's honor (Blizzard_InspectUI's InspectHonorFrame): NotifyInspect's player (0x4C6F70:
// a new
//   one forgets the data), RequestInspectHonorData (0x4C80A0: once, while it has none and none is asked for),
//   MSG_INSPECT_HONOR_STATS' answer (0x4C6E40: only the inspected player's is kept), HasInspectHonorData,
//   GetInspectHonorData and ClearInspectPlayer;
// - the reputation list's own requests: SetWatchedFactionIndex (CMSG_SET_WATCHED_FACTION, only when it
// changes),
//   SetFactionInactive / SetFactionActive (0x4D60F0: the flag changes here too, the server answers nothing)
//   and FactionToggleAtWar's 1.12 rules (0x4D5FD0: not in combat, peace only from Unfriendly up);
// - AbandonSkill (CMSG_UNLEARN_SKILL).
// Main thread; fixed storage.
class CharacterSheet {
public:
    CharacterSheet(const EntityStore& entities, Abilities& abilities);

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, bool newLogin);
    void leaveWorld();
    // MSG_INSPECT_HONOR_STATS. False when malformed.
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload);

    // --- Inspect ---
    // NotifyInspect's player (0 forgets it: ClearInspectPlayer). Another player than the one before drops its
    // data.
    void setInspected(u64 player);
    u64 inspected() const { return m_inspected; }
    // RequestInspectHonorData: false when not sent (nobody inspected, data had or asked for already).
    bool requestInspectHonor();
    // HasInspectHonorData ? GetInspectHonorData : nullptr.
    const net::InspectHonorStats* inspectHonor() const { return m_honorKnown ? &m_honor : nullptr; }

    // --- Reputation ---
    // SetWatchedFactionIndex: `listId` -1 for none. False when not sent (unchanged, not in the world).
    bool setWatchedFaction(i32 listId);
    i32 watchedFaction() const; // PLAYER_FIELD_WATCHED_FACTION_INDEX
    // SetFactionInactive / SetFactionActive.
    bool setFactionInactive(u32 listId, bool inactive);
    // FactionToggleAtWar of a faction whose value (base + standing) is `value`: InCombat when the client
    // refuses with ERR_NOT_IN_COMBAT, Refused when it does nothing.
    enum class WarResult : u8 { Sent, Refused, InCombat };
    WarResult toggleAtWar(u32 listId, i32 value);

    // --- Skills ---
    bool unlearnSkill(u32 skillId); // AbandonSkill

private:
    bool sendBody(net::Opcode opcode);

    const EntityStore& m_entities;
    Abilities& m_abilities;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;

    u64 m_inspected = 0;
    bool m_honorKnown = false;
    bool m_honorAsked = false;
    net::InspectHonorStats m_honor;
};

// The HonorFrame's and InspectHonorFrame's numbers.
struct HonorStats {
    u32 todayHonorable = 0, todayDishonorable = 0;
    u32 yesterdayHonorable = 0, yesterdayHonor = 0;
    u32 thisWeekHonorable = 0, thisWeekHonor = 0;
    u32 lastWeekHonorable = 0, lastWeekHonor = 0, lastWeekStanding = 0;
    u32 lifetimeHonorable = 0, lifetimeDishonorable = 0;
    u32 highestRank = 0;   // HonorMgr's rank (GetPVPRankInfo)
    u32 rank = 0;          // UnitPVPRank
    f32 progress = 0.0f;   // GetPVPRankProgress: 0..1
    bool alliance = false; // UnitFactionGroup: the bar's colour and the rank names' side
};
// The player's own (GetPVPSessionStats, GetPVPYesterdayStats, GetPVPThisWeekStats, GetPVPLastWeekStats,
// GetPVPLifetimeStats, UnitPVPRank, GetPVPRankProgress at 0x51A4B0 .. 0x51AA90): the byte-packed fields as
// HonorMgr writes them (a kill field's low half honorable, high half dishonorable). A highest rank under 5
// reads 0.
HonorStats honorStats(const Entity& player);
// The inspected player's (GetInspectHonorData, GetInspectPVPRankProgress): the answer's numbers, its rank
// from the unit's fields (UnitPVPRank("target")); `unit` may be null (rank 0).
HonorStats honorStats(const net::InspectHonorStats& m, const Entity* unit);

// Which side a race fights for (UnitFactionGroup: ChrRaces' faction template's group).
bool allianceRace(u32 race);
// GetPVPRankInfo (0x51A930): the rank's title key "PVP_RANK_<rank>_<side>" (1 Alliance, 0 Horde; "_FEMALE"
// added for a woman when the table has it, as UnitPVPName does) into `out`, and its number (rank - 4 from
// rank 5 up, rank - 5 below: the dishonored ranks are negative). Empty and 0 for rank 0 (NONE).
std::string_view pvpRankKey(std::span<char> out, u32 rank, bool alliance, bool female = false);
i32 pvpRankNumber(u32 rank);

} // namespace vd::game
