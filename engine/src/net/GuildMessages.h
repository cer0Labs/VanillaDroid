#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Phase 15 (guild): the packets of guild management, guild charters and the tabard designer, as vmangos-core
// 4641790 reads and writes them for build 5875 (Server/Packets/Guild.cpp, Server/Packets/Petition.cpp,
// Handlers/GuildHandler.cpp, Handlers/PetitionsHandler.cpp, Guild/GuildMgr.cpp Petition::BuildSignatureData)
// and as WoW.exe 1.12.1 sends them (BuyGuildCharter 0x4F5050, TabardModel:Save 0x5E03F0).
//
// Same rules as net/SocialMessages: parsers fill a caller-owned struct and return false on a malformed packet
// (never reading past its end); strings are views into the payload. Builders append the body of a client
// packet. Bodies covered elsewhere: CMSG_GUILD_ADD_RANK and CMSG_GUILD_INFO_TEXT (writeCString),
// CMSG_GUILD_DEL_RANK (empty), CMSG_PETITION_SHOWLIST, CMSG_PETITION_SHOW_SIGNATURES, MSG_PETITION_DECLINE
// (client) and CMSG_TURN_IN_PETITION (writeGuid).
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Guild management (Guild/Guild.h).

// GuildRankRights: every right carries GR_RIGHT_EMPTY (0x40); a rank "has" one when its other bits are set.
constexpr u32 kGuildRightEmpty = 0x00000040;
constexpr u32 kGuildRightChatListen = 0x00000041;
constexpr u32 kGuildRightChatSpeak = 0x00000042;
constexpr u32 kGuildRightOfficerListen = 0x00000044;
constexpr u32 kGuildRightOfficerSpeak = 0x00000048;
constexpr u32 kGuildRightEditPublicNote = 0x00002040;
constexpr u32 kGuildRightViewOfficerNote = 0x00004040;
constexpr u32 kGuildRightEditOfficerNote = 0x00008040;
constexpr u32 kGuildRightModifyInfo = 0x00010040; // GR_RIGHT_MODIFY_GUILD_INFO (CMSG_GUILD_INFO_TEXT)
constexpr u32 kGuildRightAll = 0x000FF1FF; // the guild master's (HandleGuildRankOpcode forces it on rank 0)
constexpr u32 kGuildRanksMin = 5;          // GUILD_RANKS_MIN_COUNT: DelRank stops there
constexpr u32 kGuildRanksMax = 10;         // GUILD_RANKS_MAX_COUNT
// The lengths the server accepts (longer ones are an anticheat kick: the client's edit boxes stop there).
constexpr u32 kGuildRankNameMax = 15; // GUILD_RANK_MAX_LENGTH
constexpr u32 kGuildNameMax = 24;     // GUILD_NAME_MAX_LENGTH
constexpr u32 kGuildNoteMax = 31;     // GUILD_NOTE_MAX_LENGTH
constexpr u32 kGuildInfoMax = 500;    // GUILD_INFO_MAX_LENGTH
constexpr u32 kGuildMotdMax = 128;    // GUILD_MOTD_MAX_LENGTH

// CMSG_GUILD_SET_PUBLIC_NOTE / CMSG_GUILD_SET_OFFICER_NOTE (GuildSetPublicNote / GuildSetOfficerNote): the
// member's name, then the note.
void writeGuildSetNote(std::vector<u8>& out, std::string_view member, std::string_view note);
// CMSG_GUILD_RANK (GuildRank::ReadFromWorldPacket): rank id, rights, the rank's name (the guild master only).
void writeGuildRank(std::vector<u8>& out, u32 rankId, u32 rights, std::string_view name);

// -------------------------------------------------------------------------------------------------------
// Tabard designer (HandleSaveGuildEmblemOpcode).

// MSG_SAVE_GUILD_EMBLEM from the client (SaveGuildEmblem::ReadFromWorldPacket): the vendor, then the five
// choices.
struct GuildEmblemChoice {
    i32 emblemStyle = 0, emblemColor = 0, borderStyle = 0, borderColor = 0, backgroundColor = 0;
    bool operator==(const GuildEmblemChoice&) const = default;
};
void writeSaveGuildEmblem(std::vector<u8>& out, u64 vendor, const GuildEmblemChoice& emblem);
// MSG_SAVE_GUILD_EMBLEM from the server (SaveGuildEmblemResult): one u32, GuildEmblem — parseU32().
constexpr u32 kEmblemSuccess = 0;        // ERR_GUILDEMBLEM_SUCCESS
constexpr u32 kEmblemInvalidColors = 1;  // ERR_GUILDEMBLEM_INVALID_TABARD_COLORS
constexpr u32 kEmblemNoGuild = 2;        // ERR_GUILDEMBLEM_NOGUILD
constexpr u32 kEmblemNotGuildMaster = 3; // ERR_GUILDEMBLEM_NOTGUILDMASTER
constexpr u32 kEmblemNotEnoughMoney = 4; // ERR_GUILDEMBLEM_NOTENOUGHMONEY
constexpr u32 kEmblemFailNoMessage = 5;  // nothing shown
// MSG_TABARDVENDOR_ACTIVATE from the server (WorldSession::SendTabardVendorActivate): the vendor's full GUID
// — parseGuid(). The client sends the same to open it (a tabard designer without a gossip menu): writeGuid().

// -------------------------------------------------------------------------------------------------------
// Guild charters (Petition.h, PetitionsHandler.cpp).

constexpr u32 kGuildCharterEntry = 5863; // GUILD_CHARTER (item_template)
constexpr u32 kPetitionSignaturesMax =
    9; // MAX_PETITION_SIGNATURES: the client's hard limit, minimum and maximum
constexpr u32 kPetitionShowListMax = 10; // more entries than the client could show: malformed

// SMSG_PETITION_SHOWLIST (PetitionShowList::AppendBodyTo): the registrar, a u8 count, then each entry.
struct PetitionShowList {
    struct Entry {
        u32 index = 0;
        u32 charterEntry = 0;
        u32 charterDisplay = 0;
        i32 cost = 0;  // copper
        i32 flags = 0; // & 1: shown (the client opens GuildRegistrarFrame only then)
    };
    u64 npc = 0;
    u32 count = 0;
    std::array<Entry, kPetitionShowListMax> entries{};
};
bool parsePetitionShowList(std::span<const u8> payload, PetitionShowList& out);

// CMSG_PETITION_BUY (PetitionBuy::ReadFromWorldPacket): the registrar, u32 0, u64 0, the name, ten u32 0, u16
// 0, u8 0, the list's index, u32 0. The zeros are the petition's options the 1.12 client never fills.
void writePetitionBuy(std::vector<u8>& out, u64 npc, std::string_view name, u32 index);

// SMSG_PETITION_SHOW_SIGNATURES (HandlePetitionShowSignOpcode / HandleOfferPetitionOpcode): the charter item,
// its owner, the petition id, a u8 count, then a GUID and a u32 0 per signature.
struct PetitionSignatures {
    u64 item = 0;
    u64 owner = 0;
    u32 petition = 0;
    u32 count = 0;
    std::array<u64, kPetitionSignaturesMax> signers{};
};
bool parsePetitionShowSignatures(std::span<const u8> payload, PetitionSignatures& out);

// CMSG_PETITION_SIGN (PetitionSign::ReadFromWorldPacket): the charter item and a u8, SignPetition's argument
// (1 when called without one, as PetitionFrame does; the server skips it).
void writePetitionSign(std::vector<u8>& out, u64 item);
// SMSG_PETITION_SIGN_RESULTS (PetitionSignResults): the charter item, the signer, PetitionSigns.
constexpr u32 kPetitionSignOk = 0;         // PETITION_SIGN_OK
constexpr u32 kPetitionAlreadySigned = 1;  // PETITION_SIGN_ALREADY_SIGNED
constexpr u32 kPetitionAlreadyInGuild = 2; // PETITION_SIGN_ALREADY_IN_GUILD
constexpr u32 kPetitionCantSignOwn = 3;    // PETITION_SIGN_CANT_SIGN_OWN
constexpr u32 kPetitionNeedMore = 4;       // PETITION_SIGN_NEED_MORE (turn-in)
constexpr u32 kPetitionNotServer = 5;      // PETITION_SIGN_NOT_SERVER
struct PetitionSignResult {
    u64 item = 0;
    u64 signer = 0;
    u32 result = 0;
};
bool parsePetitionSignResults(std::span<const u8> payload, PetitionSignResult& out);

// CMSG_PETITION_QUERY (QueryPetition::ReadFromWorldPacket): the petition id, then the charter item.
void writePetitionQuery(std::vector<u8>& out, u32 petition, u64 item);
// SMSG_PETITION_QUERY_RESPONSE (PetitionQueryResponse::AppendBodyTo).
struct PetitionQueryResponse {
    u32 petition = 0;
    u64 owner = 0;
    std::string_view name, body;
    u32 flags = 0;
    u32 minSignatures = 0, maxSignatures = 0;
    u32 deadline = 0, created = 0;
    u32 allowedGuild = 0, allowedClasses = 0, allowedRaces = 0;
    u16 allowedGender = 0;
    u32 allowedMinLevel = 0, allowedMaxLevel = 0;
    u32 choices = 0; // the choice strings are skipped
    u32 defaultChoice = 0;
};
bool parsePetitionQueryResponse(std::span<const u8> payload, PetitionQueryResponse& out);

// CMSG_OFFER_PETITION (OfferPetition::ReadFromWorldPacket): the charter item, then the player offered it.
void writeOfferPetition(std::vector<u8>& out, u64 item, u64 player);
// MSG_PETITION_DECLINE from the server (PetitionDeclineResult): who declined — parseGuid().
// SMSG_TURN_IN_PETITION_RESULTS (TurnInPetitionResults): one u32, PetitionSigns — parseU32().

// MSG_PETITION_RENAME from the client (PetitionRename::ReadFromWorldPacket) and from the server
// (PetitionRenameResult): the charter item, then the new name.
void writePetitionRename(std::vector<u8>& out, u64 item, std::string_view name);
struct PetitionRename {
    u64 item = 0;
    std::string_view name;
};
bool parsePetitionRename(std::span<const u8> payload, PetitionRename& out);

} // namespace vd::net
