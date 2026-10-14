#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Social packets (phase 8): character creation and deletion, groups, chat channels, text emotes, friends and
// ignore lists, who, guilds and duels, as vmangos-core 4641790 reads and writes them for build 5875
// (Server/Packets/{Character,Group,Channel,Social,Misc,Guild,Duel}.cpp, Group/Group.cpp, Chat/Channel.cpp,
// Handlers/GroupHandler.cpp, Handlers/MiscHandler.cpp, Spells/SpellEffects.cpp).
//
// Same rules as GameMessages: parsers fill a caller-owned struct (reuse it: vectors keep their capacity) and
// return false on a malformed packet, never reading past its end; strings are views into the payload. Builders
// append the body of a client packet (the opcode travels separately). Bodies that are a single full GUID use
// writeGuid(), a single u32 writeU32() and empty bodies nothing (GameMessages.h).
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Characters (Packets/Character.cpp).

// CMSG_CHAR_CREATE (CharCreate::ReadFromWorldPacket).
struct CharCreateRequest {
    std::string_view name;
    u8 race = 0, cls = 0, gender = 0;
    u8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    u8 outfit = 0; // read and ignored by the server
};
void writeCharCreate(std::vector<u8>& out, const CharCreateRequest& c);
// CMSG_CHAR_DELETE: writeGuid(). SMSG_CHAR_CREATE / SMSG_CHAR_DELETE: one u8 ResponseCode (ResponseCodes.gen.h).
bool parseU8(std::span<const u8> payload, u8& value);

// -------------------------------------------------------------------------------------------------------
// Groups (Packets/Group.cpp, Group/Group.h).

// GroupMemberStatus flags (SMSG_GROUP_LIST, SMSG_PARTY_MEMBER_STATS).
constexpr u8 kMemberOnline = 0x01;
constexpr u8 kMemberPvp = 0x02;
constexpr u8 kMemberDead = 0x04;
constexpr u8 kMemberGhost = 0x08;
constexpr u8 kMemberPvpFfa = 0x10;
constexpr u8 kMemberAfk = 0x40;
constexpr u8 kMemberDnd = 0x80;

constexpr u8 kGroupTypeNormal = 0;
constexpr u8 kGroupTypeRaid = 1;
constexpr u32 kMaxGroupSize = 5;  // MAX_GROUP_SIZE
constexpr u32 kMaxRaidSize = 40;  // MAX_RAID_SIZE

// LootMethod (Group/Group.h).
enum class LootMethod : u8 { FreeForAll = 0, RoundRobin = 1, MasterLoot = 2, GroupLoot = 3, NeedBeforeGreed = 4 };

struct GroupList { // SMSG_GROUP_LIST (GroupList::AppendBodyTo): the recipient is not among `members`
    struct Member {
        std::string_view name;
        u64 guid = 0;
        u8 status = 0; // kMember*
        u8 subgroup = 0;
        bool assistant = false;
    };
    u8 groupType = 0; // kGroupType*
    u8 ownSubgroup = 0;
    bool ownAssistant = false;
    std::vector<Member> members;
    u64 leader = 0;
    // Only sent with at least one member (the empty list = not in a group).
    u8 lootMethod = 0;
    u64 looter = 0; // master looter (MASTER_LOOT only)
    u8 lootThreshold = 0;
};
bool parseGroupList(std::span<const u8> payload, GroupList& out);

// GroupUpdateFlags (Group/Group.h): which fields an SMSG_PARTY_MEMBER_STATS carries.
constexpr u32 kGroupUpdateStatus = 0x00000001;
constexpr u32 kGroupUpdateCurHp = 0x00000002;
constexpr u32 kGroupUpdateMaxHp = 0x00000004;
constexpr u32 kGroupUpdatePowerType = 0x00000008;
constexpr u32 kGroupUpdateCurPower = 0x00000010;
constexpr u32 kGroupUpdateMaxPower = 0x00000020;
constexpr u32 kGroupUpdateLevel = 0x00000040;
constexpr u32 kGroupUpdateZone = 0x00000080;
constexpr u32 kGroupUpdatePosition = 0x00000100;
constexpr u32 kGroupUpdateAuras = 0x00000200;
constexpr u32 kGroupUpdateAurasNegative = 0x00000400;
constexpr u32 kGroupUpdatePetGuid = 0x00000800;
constexpr u32 kGroupUpdatePetName = 0x00001000;
constexpr u32 kGroupUpdatePetModel = 0x00002000;
constexpr u32 kGroupUpdatePetCurHp = 0x00004000;
constexpr u32 kGroupUpdatePetMaxHp = 0x00008000;
constexpr u32 kGroupUpdatePetPowerType = 0x00010000;
constexpr u32 kGroupUpdatePetCurPower = 0x00020000;
constexpr u32 kGroupUpdatePetMaxPower = 0x00040000;
constexpr u32 kGroupUpdatePetAuras = 0x00080000;
constexpr u32 kGroupUpdatePetAurasNegative = 0x00100000;
constexpr u32 kGroupUpdateFull = 0x001FFFFF;

// SMSG_PARTY_MEMBER_STATS and SMSG_PARTY_MEMBER_STATS_FULL (WorldSession::BuildPartyMemberStatsPacket): a packed
// GUID, the mask, then the fields the mask names in flag order. Only the fields in `mask` are meaningful.
struct PartyMemberStats {
    u64 guid = 0;
    u32 mask = 0;
    u8 status = 0;
    u16 health = 0, maxHealth = 0;
    u8 powerType = 0;
    u16 power = 0, maxPower = 0;
    u16 level = 0;
    u16 zone = 0;
    i16 x = 0, y = 0;
    u32 auraMask = 0;                // positive auras (slots 0..31): one spell id per set bit
    std::array<u16, 32> auras{};     // by slot
    u16 negativeAuraMask = 0;        // slots 32..47
    std::array<u16, 16> negativeAuras{};
    u64 petGuid = 0;
    std::string_view petName;
    u16 petDisplay = 0, petHealth = 0, petMaxHealth = 0;
    u8 petPowerType = 0;
    u16 petPower = 0, petMaxPower = 0;
    u32 petAuraMask = 0; // phase 15 (pets): the pet's auras, as the member's
    std::array<u16, 32> petAuras{};
    u16 petNegativeAuraMask = 0;
    std::array<u16, 16> petNegativeAuras{};
};
bool parsePartyMemberStats(std::span<const u8> payload, PartyMemberStats& out);

// PartyOperation / PartyResult (Server/WorldSession.h).
constexpr u32 kPartyOpInvite = 0;
constexpr u32 kPartyOpLeave = 2;
enum class PartyResult : u32 {
    Ok = 0,
    BadPlayerName = 1,  // "Cannot find '%s'."
    TargetNotInGroup = 2,
    GroupFull = 3,
    AlreadyInGroup = 4,
    NotInGroup = 5,
    NotLeader = 6,
    WrongFaction = 7,
    IgnoringYou = 8,
    InternalBattleground = 10,
};
struct PartyCommandResult { // SMSG_PARTY_COMMAND_RESULT
    u32 operation = 0;
    std::string_view name;
    u32 result = 0;
};
bool parsePartyCommandResult(std::span<const u8> payload, PartyCommandResult& out);
// SMSG_GROUP_INVITE (the inviter), SMSG_GROUP_DECLINE (who declined), SMSG_GROUP_SET_LEADER (the new leader):
// one C string, parseCString(). SMSG_GROUP_UNINVITE and SMSG_GROUP_DESTROYED: empty.

// A single C string: CMSG_GROUP_INVITE, CMSG_GROUP_UNINVITE, CMSG_LEAVE_CHANNEL, CMSG_CHANNEL_LIST,
// CMSG_ADD_FRIEND, CMSG_ADD_IGNORE, CMSG_GUILD_INVITE, CMSG_GUILD_PROMOTE, CMSG_GUILD_DEMOTE, CMSG_GUILD_REMOVE,
// CMSG_GUILD_LEADER, CMSG_GUILD_MOTD.
void writeCString(std::vector<u8>& out, std::string_view text);
// CMSG_LOOT_METHOD: method, master looter (only kept for MASTER_LOOT), item quality threshold.
void writeLootMethod(std::vector<u8>& out, u32 method, u64 master, u32 threshold);
// Phase 16: the raid's subgroups (GroupHandler.cpp; only the leader and assistants are obeyed). SetRaidSubgroup:
// CMSG_GROUP_CHANGE_SUB_GROUP, the member's name and the subgroup 0..7 (ignored when it is full); SwapRaidSubgroup:
// CMSG_GROUP_SWAP_SUB_GROUP, two members' names.
constexpr u32 kRaidSubgroups = 8; // MAX_RAID_SUBGROUPS
void writeGroupChangeSubGroup(std::vector<u8>& out, std::string_view name, u8 subgroup);
void writeGroupSwapSubGroup(std::vector<u8>& out, std::string_view name, std::string_view other);
// CMSG_GROUP_SET_LEADER, CMSG_GROUP_UNINVITE_GUID, CMSG_REQUEST_PARTY_MEMBER_STATS: writeGuid(). CMSG_GROUP_ACCEPT,
// CMSG_GROUP_DECLINE, CMSG_GROUP_DISBAND (leave), CMSG_GROUP_RAID_CONVERT: empty.

// MSG_RANDOM_ROLL (HandleRandomRollOpcode): the client asks for a roll between two numbers; the group (or the player
// alone) gets the result.
void writeRandomRoll(std::vector<u8>& out, u32 minimum, u32 maximum);
struct RandomRoll {
    u32 minimum = 0, maximum = 0, roll = 0;
    u64 guid = 0; // who rolled
};
bool parseRandomRoll(std::span<const u8> payload, RandomRoll& out);

// -------------------------------------------------------------------------------------------------------
// Chat channels (Packets/Channel.cpp, Chat/Channel.cpp).

// ChatNotify (Chat/Channel.h).
enum class ChannelNotice : u8 {
    Joined = 0x00,          // + GUID
    Left = 0x01,            // + GUID
    YouJoined = 0x02,       // + u32 channel flags + u32 (0)
    YouLeft = 0x03,
    WrongPassword = 0x04,
    NotMember = 0x05,
    NotModerator = 0x06,
    PasswordChanged = 0x07, // + GUID
    OwnerChanged = 0x08,    // + GUID
    PlayerNotFound = 0x09,  // + name
    NotOwner = 0x0A,
    ChannelOwner = 0x0B,    // + name ("Nobody" for constant channels)
    ModeChange = 0x0C,      // + GUID + u8 old flags + u8 new flags
    AnnouncementsOn = 0x0D, // + GUID
    AnnouncementsOff = 0x0E,
    ModerationOn = 0x0F,
    ModerationOff = 0x10,
    Muted = 0x11,
    PlayerKicked = 0x12,     // + target GUID + source GUID
    Banned = 0x13,
    PlayerBanned = 0x14,     // + target GUID + source GUID
    PlayerUnbanned = 0x15,   // + target GUID + source GUID
    PlayerNotBanned = 0x16,  // + name
    PlayerAlreadyMember = 0x17, // + GUID
    Invite = 0x18,           // + GUID (who invites)
    InviteWrongFaction = 0x19,
    WrongFaction = 0x1A,
    InvalidName = 0x1B,
    NotModerated = 0x1C,
    PlayerInvited = 0x1D,    // + name
    PlayerInviteBanned = 0x1E, // + name
    Throttled = 0x1F,
};
// Channel flags of a YouJoined notice and SMSG_CHANNEL_LIST (Channel::ChannelFlags).
constexpr u32 kChannelFlagCustom = 0x01;
constexpr u32 kChannelFlagTrade = 0x04;
constexpr u32 kChannelFlagNotLfg = 0x08;
constexpr u32 kChannelFlagGeneral = 0x10;
constexpr u32 kChannelFlagCity = 0x20;
constexpr u32 kChannelFlagLfg = 0x40;
// Member flags (Channel::ChannelMemberFlags).
constexpr u8 kChannelMemberOwner = 0x01;
constexpr u8 kChannelMemberModerator = 0x02;
constexpr u8 kChannelMemberMuted = 0x08;

struct ChannelNotify { // SMSG_CHANNEL_NOTIFY: u8 notice, the channel's name, then what the notice carries
    ChannelNotice notice = ChannelNotice::Joined;
    std::string_view channel;
    u64 guid = 0;          // the player concerned (or the target of a kick / ban)
    u64 source = 0;        // kick / ban: who did it
    std::string_view name; // PlayerNotFound, ChannelOwner, PlayerNotBanned, PlayerInvited, PlayerInviteBanned
    u32 flags = 0;         // YouJoined: the channel's flags
    u8 oldFlags = 0, newFlags = 0; // ModeChange
};
bool parseChannelNotify(std::span<const u8> payload, ChannelNotify& out);

struct ChannelList { // SMSG_CHANNEL_LIST (Channel::List)
    struct Member {
        u64 guid = 0;
        u8 flags = 0; // kChannelMember*
    };
    std::string_view channel;
    u8 flags = 0;
    std::vector<Member> members;
};
bool parseChannelList(std::span<const u8> payload, ChannelList& out);

// CMSG_JOIN_CHANNEL: name and password (empty for none).
void writeJoinChannel(std::vector<u8>& out, std::string_view channel, std::string_view password);
// CMSG_TEXT_EMOTE: EmotesText.dbc id, a number the server echoes back, the target (0: none).
void writeTextEmote(std::vector<u8>& out, u32 textEmote, u32 emoteNum, u64 target);

// -------------------------------------------------------------------------------------------------------
// Friends and ignore list (Packets/Social.cpp, SocialMgr.h).

// FriendStatus.
constexpr u8 kFriendOffline = 0;
constexpr u8 kFriendOnline = 1;
constexpr u8 kFriendAfk = 2;
constexpr u8 kFriendDnd = 4;

// FriendsResult (SocialMgr.h).
enum class FriendResult : u8 {
    DbError = 0x00,
    ListFull = 0x01,
    Online = 0x02,
    Offline = 0x03,
    NotFound = 0x04,
    Removed = 0x05,
    AddedOnline = 0x06,
    AddedOffline = 0x07,
    Already = 0x08,
    Self = 0x09,
    Enemy = 0x0A,
    IgnoreFull = 0x0B,
    IgnoreSelf = 0x0C,
    IgnoreNotFound = 0x0D,
    IgnoreAlready = 0x0E,
    IgnoreAdded = 0x0F,
    IgnoreRemoved = 0x10,
    IgnoreAmbiguous = 0x11,
};

struct FriendEntry {
    u64 guid = 0;
    u8 status = 0;               // kFriend*
    u32 area = 0, level = 0, cls = 0; // only for online friends
};
// SMSG_FRIEND_LIST: u8 count, then GUID + status (+ area, level, class when online).
bool parseFriendList(std::span<const u8> payload, std::vector<FriendEntry>& out);
// SMSG_IGNORE_LIST: u8 count, then GUIDs.
bool parseIgnoreList(std::span<const u8> payload, std::vector<u64>& out);

struct FriendStatus { // SMSG_FRIEND_STATUS: result, GUID (+ status, area, level, class when someone came online)
    u8 result = 0;    // FriendResult
    u64 guid = 0;
    bool hasInfo = false;
    u8 status = 0;
    u32 area = 0, level = 0, cls = 0;
};
bool parseFriendStatus(std::span<const u8> payload, FriendStatus& out);
// CMSG_FRIEND_LIST: empty. CMSG_ADD_FRIEND / CMSG_ADD_IGNORE: writeCString(name). CMSG_DEL_FRIEND /
// CMSG_DEL_IGNORE: writeGuid().

// -------------------------------------------------------------------------------------------------------
// Who (Packets/Misc.cpp Who, Handlers/MiscHandler.cpp WhoListClientQueryTask).

struct WhoRequest {
    u32 minLevel = 0;
    u32 maxLevel = 100;
    std::string_view name;  // part of the name (empty: any)
    std::string_view guild; // part of the guild's name
    u32 raceMask = 0xFFFFFFFFu; // 1 << race
    u32 classMask = 0xFFFFFFFFu; // 1 << class
    std::span<const u32> zones; // AreaTable zone ids (at most 10)
    std::span<const std::string_view> terms; // name, guild or zone contains one of them (at most 4)
};
void writeWho(std::vector<u8>& out, const WhoRequest& request);

struct WhoList { // SMSG_WHO: listed count, online count, then the players (at most 49)
    struct Entry {
        std::string_view name, guild;
        u32 level = 0, cls = 0, race = 0, zone = 0;
    };
    u32 online = 0; // players matching (the server says more than it lists when there are > 49 online)
    std::vector<Entry> entries;
};
bool parseWhoList(std::span<const u8> payload, WhoList& out);

// -------------------------------------------------------------------------------------------------------
// Guilds (Packets/Guild.cpp, Guild/Guild.h).

constexpr u32 kGuildRankCount = 10; // GuildQueryResponse: always 10 names (unused ones empty)
// GuildRankRights.
constexpr u32 kGuildRightInvite = 0x00000050;
constexpr u32 kGuildRightRemove = 0x00000060;
constexpr u32 kGuildRightPromote = 0x000000C0;
constexpr u32 kGuildRightDemote = 0x00000140;
constexpr u32 kGuildRightSetMotd = 0x00001040;
// GuildRosterPresenceFlags.
constexpr u8 kGuildMemberOnline = 0x01;
constexpr u8 kGuildMemberAfk = 0x02;
constexpr u8 kGuildMemberDnd = 0x04;
// GuildEvents.
enum class GuildEventKind : u8 {
    Promotion = 0x00,     // officer, member, rank name
    Demotion = 0x01,      // officer, member, rank name
    Motd = 0x02,          // the message
    Joined = 0x03,        // name (+ GUID)
    Left = 0x04,          // name (+ GUID)
    Removed = 0x05,       // removed, by whom
    LeaderIs = 0x06,      // name
    LeaderChanged = 0x07, // old, new
    Disbanded = 0x08,
    TabardChange = 0x09,
    RankNameUpdate = 0x0A,
    RosterUpdate = 0x0B,
    SignedOn = 0x0C,      // name (+ GUID)
    SignedOff = 0x0D,     // name (+ GUID)
};
// Typecommand / CommandErrors of SMSG_GUILD_COMMAND_RESULT.
constexpr u32 kGuildCommandCreate = 0x00;
constexpr u32 kGuildCommandInvite = 0x01;
constexpr u32 kGuildCommandQuit = 0x03;
constexpr u32 kGuildCommandFounder = 0x0E;
enum class GuildError : u32 {
    Ok = 0x00, // ERR_PLAYER_NO_MORE_IN_GUILD: no message
    Internal = 0x01,
    AlreadyInGuild = 0x02,
    AlreadyInGuildS = 0x03,
    InvitedToGuild = 0x04,
    AlreadyInvitedS = 0x05,
    NameInvalid = 0x06,
    NameExistsS = 0x07,
    Permissions = 0x08, // also ERR_GUILD_LEADER_LEAVE for the quit command
    PlayerNotInGuild = 0x09,
    PlayerNotInGuildS = 0x0A,
    PlayerNotFoundS = 0x0B,
    NotAllied = 0x0C,
    RankTooHighS = 0x0D,
    RankTooLowS = 0x0E,
    RanksLocked = 0x11,
    RankInUse = 0x12,
    IgnoringYouS = 0x13,
};

struct GuildQueryResponse { // SMSG_GUILD_QUERY_RESPONSE
    u32 guildId = 0;
    std::string_view name;
    std::array<std::string_view, kGuildRankCount> ranks{};
    i32 emblemStyle = 0, emblemColor = 0, borderStyle = 0, borderColor = 0, backgroundColor = 0;
};
bool parseGuildQueryResponse(std::span<const u8> payload, GuildQueryResponse& out);

struct GuildRoster { // SMSG_GUILD_ROSTER
    struct Member {
        u64 guid = 0;
        u8 presence = 0; // kGuildMember* (0: offline)
        std::string_view name;
        u32 rank = 0;
        u8 level = 0, cls = 0;
        u32 zone = 0;
        f32 offlineDays = 0.0f; // offline members only
        std::string_view note, officerNote; // the officer note is empty without the right to see it
    };
    std::string_view motd, info;
    std::vector<u32> rankRights; // by rank
    std::vector<Member> members;
};
bool parseGuildRoster(std::span<const u8> payload, GuildRoster& out);

struct GuildEvent { // SMSG_GUILD_EVENT: event, u8 count, the strings, then the GUID of the player concerned if any
    u8 event = 0; // GuildEventKind
    u32 count = 0;
    std::array<std::string_view, 4> params{}; // the first four (more are read and skipped)
    u64 guid = 0;
};
bool parseGuildEvent(std::span<const u8> payload, GuildEvent& out);

struct GuildInvite { // SMSG_GUILD_INVITE
    std::string_view inviter, guild;
};
bool parseGuildInvite(std::span<const u8> payload, GuildInvite& out);

struct GuildCommandResult { // SMSG_GUILD_COMMAND_RESULT
    u32 command = 0;
    std::string_view name;
    u32 result = 0; // GuildError
};
bool parseGuildCommandResult(std::span<const u8> payload, GuildCommandResult& out);

struct GuildInfo { // SMSG_GUILD_INFO
    std::string_view name;
    u32 day = 0, month = 0, year = 0;
    u32 members = 0, accounts = 0;
};
bool parseGuildInfo(std::span<const u8> payload, GuildInfo& out);
// SMSG_GUILD_DECLINE: parseCString(). CMSG_GUILD_QUERY: writeU32(guild id). CMSG_GUILD_ROSTER, CMSG_GUILD_ACCEPT,
// CMSG_GUILD_DECLINE, CMSG_GUILD_LEAVE, CMSG_GUILD_INFO: empty.

// -------------------------------------------------------------------------------------------------------
// Duels (Packets/Duel.cpp, Spell::EffectDuel).

struct DuelRequested { // SMSG_DUEL_REQUESTED (to both): the flag object and who challenged, full GUIDs
    u64 arbiter = 0;
    u64 initiator = 0;
};
bool parseDuelRequested(std::span<const u8> payload, DuelRequested& out);
struct DuelWinner { // SMSG_DUEL_WINNER (to everyone around)
    bool fled = false; // the loser ran away instead of being beaten
    std::string_view winner, loser;
};
bool parseDuelWinner(std::span<const u8> payload, DuelWinner& out);
// SMSG_DUEL_COMPLETE: u8 (1 the duel was fought, 0 cancelled) — parseU8(). SMSG_DUEL_COUNTDOWN: u32 ms —
// parseU32(). SMSG_DUEL_OUTOFBOUNDS / SMSG_DUEL_INBOUNDS: empty. CMSG_DUEL_ACCEPTED / CMSG_DUEL_CANCELLED:
// writeGuid(the arbiter).

} // namespace vd::net
