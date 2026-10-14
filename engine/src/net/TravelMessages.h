#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <vector>

// Travel packets (phase 8): exploration, area triggers, flight masters, innkeepers, transfers and the end of
// a server-driven spline, as vmangos-core 4641790 reads and writes them for build 5875 (Server/Packets/Misc,
// Taxi, Npc, Movement). Same rules as net/GameMessages: parsers return false on a malformed packet and never
// read past its end; builders append a client packet's body.
namespace vd::net {

// ActivateTaxiReplies (Objects/Player.h).
enum class TaxiReply : u32 {
    Ok = 0,
    UnspecifiedServerError = 1,
    NoSuchPath = 2,
    NotEnoughMoney = 3,
    TooFarAway = 4,
    NoVendorNearby = 5,
    NotVisited = 6,
    PlayerBusy = 7,
    PlayerAlreadyMounted = 8,
    PlayerShapeshifted = 9,
    PlayerMoving = 10,
    SameNode = 11,
    NotStanding = 12,
};
const char* taxiReplyName(u32 reply);

// TransferAbortReason (Objects/Player.h).
constexpr u8 kTransferAbortMaxPlayers = 1;
constexpr u8 kTransferAbortNotFound = 2;
constexpr u8 kTransferAbortTooManyInstances = 3;
constexpr u8 kTransferAbortSilently = 4;
constexpr u8 kTransferAbortZoneInCombat = 5;

// SMSG_EXPLORATION_EXPERIENCE (Misc::ExplorationExperience): an area discovered, and the experience it gave.
struct ExplorationExperience {
    u32 areaId = 0;
    u32 experience = 0;
};
bool parseExplorationExperience(std::span<const u8> payload, ExplorationExperience& out);

// SMSG_SHOWTAXINODES (Taxi::ShowTaxiNodes): the taxi map of a flight master.
struct ShowTaxiNodes {
    u32 showUi = 0; // 1 = show the map (0: debug)
    u64 flightMaster = 0;
    u32 currentNode = 0;
    std::array<u32, 8> knownNodes{}; // TaxiMask
};
bool parseShowTaxiNodes(std::span<const u8> payload, ShowTaxiNodes& out);

// SMSG_TAXINODE_STATUS (Taxi::TaxiNodeStatus): whether the player knows a flight master's node.
bool parseTaxiNodeStatus(std::span<const u8> payload, u64& guid, bool& known);

// SMSG_ACTIVATETAXIREPLY: a TaxiReply.
bool parseActivateTaxiReply(std::span<const u8> payload, u32& reply);

// SMSG_BINDPOINTUPDATE (Misc::BindpointUpdate): the hearthstone's destination.
struct BindPointUpdate {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    u32 map = 0;
    u32 areaId = 0;
};
bool parseBindPointUpdate(std::span<const u8> payload, BindPointUpdate& out);

// SMSG_PLAYERBOUND (Misc::PlayerBound): the innkeeper bound the player in `areaId`.
bool parsePlayerBound(std::span<const u8> payload, u64& binder, u32& areaId);

// SMSG_TRANSFER_PENDING (Misc::TransferPending): the map of the far teleport under way; on a transport, its
// entry and the map left.
struct TransferPending {
    u32 map = 0;
    bool onTransport = false;
    u32 transportEntry = 0;
    u32 oldMap = 0;
};
bool parseTransferPending(std::span<const u8> payload, TransferPending& out);

// SMSG_TRANSFER_ABORTED: a TransferAbortReason.
bool parseTransferAborted(std::span<const u8> payload, u8& reason);

// --- Instances (phase 12): Server/Packets/Misc, Player::SendSavedInstances / SendRaidInfo /
// SendInstanceResetWarning / SendResetInstanceSuccess / SendResetInstanceFailed / SendRaidGroupOnlyError ---

// SMSG_INSTANCE_SAVE_CREATED (a u32, always 0), SMSG_UPDATE_INSTANCE_OWNERSHIP (u32 hasBeenSaved: the player has a
// permanent bind), SMSG_UPDATE_LAST_INSTANCE (u32 map) and SMSG_INSTANCE_RESET (u32 map): one u32 (parseU32).

// InstanceResetWarningType (Objects/Player.h): SMSG_RAID_INSTANCE_MESSAGE's type.
constexpr u32 kRaidInstanceWarningHours = 1;   // RAID_INSTANCE_WARNING_HOURS
constexpr u32 kRaidInstanceWarningMin = 2;     // RAID_INSTANCE_WARNING_MIN
constexpr u32 kRaidInstanceWarningMinSoon = 3; // RAID_INSTANCE_WARNING_MIN_SOON
constexpr u32 kRaidInstanceWelcome = 4;        // RAID_INSTANCE_WELCOME

// SMSG_RAID_INSTANCE_MESSAGE: a raid's reset is near (or the welcome on entering it).
struct RaidInstanceMessage {
    u32 type = 0;
    u32 map = 0;
    u32 secondsLeft = 0; // until the reset
};
bool parseRaidInstanceMessage(std::span<const u8> payload, RaidInstanceMessage& out);

// InstanceResetFailReason (Maps/MapPersistentStateMgr.h): SMSG_INSTANCE_RESET_FAILED's reason (3 and above: no
// message).
constexpr u32 kInstanceResetFailedGeneral = 0; // INSTANCE_RESET_FAILED: players still inside
constexpr u32 kInstanceResetFailedOffline = 1; // INSTANCE_RESET_FAILED_OFFLINE
constexpr u32 kInstanceResetFailedZoning = 2;  // INSTANCE_RESET_FAILED_ZONING
bool parseInstanceResetFailed(std::span<const u8> payload, u32& reason, u32& map);

// SMSG_RAID_INSTANCE_INFO: the player's permanent binds (raids), the answer to CMSG_REQUEST_RAID_INFO.
struct SavedInstanceInfo {
    u32 map = 0;
    u32 secondsLeft = 0; // until its reset
    u32 instanceId = 0;
};
bool parseRaidInstanceInfo(std::span<const u8> payload, std::vector<SavedInstanceInfo>& out);

// RaidGroupError (Objects/Player.h).
constexpr u32 kRaidGroupRequired = 1; // ERR_RAID_GROUP_ONLY
constexpr u32 kRaidGroupFull = 2;     // ERR_RAID_GROUP_FULL
// SMSG_RAID_GROUP_ONLY: with a time, the player is in an instance that is not its group's and goes to its bind
// point when it runs out (INSTANCE_BOOT); with 0, an entry refused (or that countdown cancelled) and its error.
bool parseRaidGroupOnly(std::span<const u8> payload, u32& timerMs, u32& error);

// --- Client packets ---

void writeAreaTrigger(std::vector<u8>& out, u32 triggerId);                       // CMSG_AREATRIGGER
void writeActivateTaxi(std::vector<u8>& out, u64 flightMaster, u32 from, u32 to); // CMSG_ACTIVATETAXI
// CMSG_ACTIVATETAXIEXPRESS: the whole route (start, hops, destination); the server checks it and charges each
// path itself (`totalCost` is read and ignored).
void writeActivateTaxiExpress(std::vector<u8>& out, u64 flightMaster, u32 totalCost,
                              std::span<const u32> nodes);
// CMSG_MOVE_SPLINE_DONE (Movement::MoveSplineDone): where the client's copy of the server's spline ended, its
// id, and a float the server skips.
void writeMoveSplineDone(std::vector<u8>& out, const MovementInfo& info, u32 splineId);
// CMSG_TAXINODE_STATUS_QUERY, CMSG_TAXIQUERYAVAILABLENODES, CMSG_BINDER_ACTIVATE: a full GUID
// (net::writeGuid).

} // namespace vd::net
