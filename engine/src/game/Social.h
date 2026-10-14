#pragma once

#include "core/Types.h"
#include "game/Auras.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/GuildEmblem.h"
#include "game/Queries.h"
#include "net/Opcodes.gen.h"
#include "net/SocialMessages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::game {

// "Duel" (SPELL_EFFECT_DUEL, Spell.dbc): the client challenges a player by casting it at them.
constexpr u32 kDuelSpell = 7266;
// The chat's numbered channels (/1 .. /10).
constexpr u32 kMaxChatChannels = 10;
// PLAYER_FLAGS (Objects/Player.h).
constexpr u32 kPlayerFlagAfk = 0x02;
constexpr u32 kPlayerFlagDnd = 0x04;

// Something the social side reports once (the HUD shows it, the chat prints it): read Social::events() with
// your own cursor. Strings and values that do not apply stay empty / 0.
enum class SocialEventKind : u8 {
    // Groups.
    GroupInvite,       // name = the inviter: answer with acceptGroupInvite() / declineGroupInvite()
    PartyResult,       // value[0] = operation (net::kPartyOp*), value[1] = net::PartyResult, name
    GroupDeclined,     // name = who declined the player's invitation
    GroupJoined,       // the player is in a group now
    GroupLeft,         // the player is not in a group any more
    GroupUninvited,    // the player was removed from its group (GroupLeft follows)
    GroupDestroyed,    // the group was disbanded (GroupLeft follows)
    MemberJoined,      // guid, name: someone joined the player's group
    MemberLeft,        // guid, name
    LeaderChanged,     // name = the new leader
    LootMethodChanged, // value[0] = net::LootMethod, value[1] = threshold, other = master looter
    // Channels.
    // value[0] = net::ChannelNotice, value[1] = the channel's number (0: none), name = channel, guid / other =
    // the players concerned, text = the name some notices carry.
    ChannelNotice,
    ChannelMembers, // name = channel, value[0] = members: channelMembers()
    // Friends, ignore list, who.
    // value[0] = net::FriendResult, guid, name (when known), value[1] = status, value[2] = area.
    FriendStatus,
    WhoResults, // value[0] = listed, value[1] = online: who()
    // Guild.
    GuildInvite,        // name = inviter, text = guild: acceptGuildInvite() / declineGuildInvite()
    GuildEvent,         // value[0] = net::GuildEventKind, name / text / extra = its strings, guid
    GuildCommandResult, // value[0] = command, value[1] = net::GuildError, name
    GuildDeclined,      // name
    // Duels.
    DuelRequested,   // guid = the challenger, other = the flag; value[0] = 1 when the player challenged
    DuelCountdown,   // value[0] = ms until it starts
    DuelOutOfBounds, // the player left the duel area: back within 10 s or lose
    DuelInBounds,
    DuelComplete, // value[0] = 1 fought, 0 cancelled
    DuelWinner,   // name = winner, text = loser, value[0] = 1 fled, value[1] = 1 the player fought it
    // The player.
    AwayChanged, // value[0] = 1 AFK, value[1] = 1 DND (PLAYER_FLAGS changed)
    Roll,        // guid = who rolled (name when known), value = roll, minimum, maximum (MSG_RANDOM_ROLL)
    // Phase 13 (social): SMSG_GUILD_INFO came: name = the guild, value = month, day, year it was created
    // (GuildState has the members and accounts).
    GuildInfo,
};

struct SocialEvent {
    SocialEventKind kind = SocialEventKind::GroupInvite;
    u32 timeMs = 0;
    u64 guid = 0;
    u64 other = 0;
    std::array<i32, 3> value{};
    std::string name;
    std::string text;
    std::string extra;
};

// --- Groups -------------------------------------------------------------------------------------------------

struct GroupMember {
    u64 guid = 0;
    std::string name;
    u8 status = 0; // net::kMember*
    u8 subgroup = 0;
    bool assistant = false;
    // Its last SMSG_PARTY_MEMBER_STATS (the server sends them while the member is out of sight).
    u32 statsMask = 0; // every net::kGroupUpdate* field that has arrived
    u16 health = 0, maxHealth = 0;
    u8 powerType = 0;
    u16 power = 0, maxPower = 0;
    u16 level = 0;
    u16 zone = 0;
    i16 x = 0, y = 0;
    bool statsRequested = false;
    // Parity (auras): the spell in each aura slot (0: none), from the stats while out of sight, copied from its
    // update fields while in sight (the server then only sends the slots that change).
    std::array<u16, kMaxAuras> auras{};
    // Phase 15 (pets): its pet from the same stats (the GROUP_UPDATE_FLAG_PET_* fields; GUID 0: none).
    u64 petGuid = 0;
    std::string petName;
    u16 petDisplay = 0;
    u16 petHealth = 0, petMaxHealth = 0;
    u8 petPowerType = 0;
    u16 petPower = 0, petMaxPower = 0;
    std::array<u16, kMaxAuras> petAuras{};
};

struct GroupState {
    bool active = false; // the player is in a group
    bool raid = false;
    u64 leader = 0;
    u8 lootMethod = 0; // net::LootMethod
    u64 looter = 0;
    u8 lootThreshold = 0;
    u8 ownSubgroup = 0;
    bool ownAssistant = false;
    std::vector<GroupMember> members; // the others
    std::string pendingInviter;       // an SMSG_GROUP_INVITE not answered yet (empty: none)
};

// What a party frame shows: the member's update fields while it is in sight, its last stats otherwise.
struct PartyMemberView {
    u64 guid = 0;
    std::string_view name;
    u32 level = 0;
    u32 health = 0, maxHealth = 0;
    u32 power = 0, maxPower = 0;
    u8 powerType = 0;
    u8 race = 0, cls = 0, gender = 0; // 0 while unknown (out of sight)
    u32 zone = 0;
    bool online = false, dead = false, ghost = false, afk = false, dnd = false;
    bool leader = false, assistant = false;
    bool inSight = false; // an entity the client sees (targetable)
    bool hasPosition = false;
    f32 x = 0.0f, y = 0.0f; // world position (server coordinates)
    // Phase 15 (pets): its pet ("partypetN"): the member's UNIT_FIELD_SUMMON while in sight, its stats otherwise; the
    // pet's fields while the client sees it, its stats otherwise. petGuid 0: none (or nothing known of it).
    u64 petGuid = 0;
    std::string_view petName;
    u32 petHealth = 0, petMaxHealth = 0;
    bool petInSight = false;
    // Phase 16 (raids): GetRaidRosterInfo's subgroup - 1 (0..7) and the player's own entry (raidMembers() only).
    // `cls` / `race` / `gender` also come from the name query's answer while out of sight (0 until it arrives).
    u8 subgroup = 0;
    bool self = false;
    // GetRaidRosterInfo's rank: 2 leader, 1 assistant, 0 member.
    u8 rank() const { return leader ? 2 : (assistant ? 1 : 0); }
};

// --- Channels -----------------------------------------------------------------------------------------------

struct ChatChannel {
    std::string name;   // empty: a free number
    u32 zoneChannel = 0; // ChatChannels.dbc id of an automatic zone channel (General, Trade...), 0 custom
    u32 flags = 0;       // net::kChannelFlag* (from the server's YouJoined)
    bool joined = false; // confirmed by the server
};

// A zone channel the player should be in (General - Elwynn Forest...); an empty name only keeps the kind's number
// (Trade outside the cities).
struct ZoneChannel {
    u32 id = 0; // ChatChannels.dbc
    std::string_view name;
};

// --- Friends, ignore list, who --------------------------------------------------------------------------------

struct FriendInfo {
    u64 guid = 0;
    u8 status = 0; // net::kFriend*: offline, online, AFK, DND
    u32 area = 0, level = 0, cls = 0; // only while online
};

struct WhoEntry {
    std::string name, guild;
    u32 level = 0, cls = 0, race = 0, zone = 0;
};

struct WhoResults {
    std::vector<WhoEntry> entries;
    u32 online = 0;
    u32 version = 0; // bumps with every answer
    bool pending = false;
};

// --- Guild ----------------------------------------------------------------------------------------------------

struct GuildMember {
    u64 guid = 0;
    std::string name;
    u8 presence = 0; // net::kGuildMember* (0: offline)
    u32 rank = 0;
    u8 level = 0, cls = 0;
    u32 zone = 0;
    f32 offlineDays = 0.0f;
    std::string note, officerNote;
};

struct GuildState {
    u32 id = 0;   // PLAYER_GUILDID (0: no guild)
    u32 rank = 0; // PLAYER_GUILDRANK: 0 guild master
    std::string name;
    std::array<std::string, net::kGuildRankCount> ranks;
    bool known = false; // name and ranks arrived
    // The roster (CMSG_GUILD_ROSTER).
    std::string motd, info;
    std::vector<u32> rankRights;
    std::vector<GuildMember> members;
    bool rosterKnown = false;
    u32 rosterVersion = 0;
    // SMSG_GUILD_INFO.
    u32 createdDay = 0, createdMonth = 0, createdYear = 0, accounts = 0;
    u32 memberCount = 0; // phase 13: its players (/ginfo)
    // An invitation not answered yet.
    std::string pendingInviter, pendingGuild;

    // The player's rank has this right (net::kGuildRight*; unknown before the roster).
    bool can(u32 right) const { return rank < rankRights.size() && (rankRights[rank] & right) == right; }
    std::string_view rankName(u32 r) const { return r < ranks.size() ? std::string_view(ranks[r]) : std::string_view{}; }
};

// --- Duels ----------------------------------------------------------------------------------------------------

enum class DuelPhase : u8 {
    None,
    Challenged,  // someone challenged the player: acceptDuel() / declineDuel()
    Challenging, // the player challenged someone, waiting for the answer
    Countdown,   // accepted: it starts at startsAtMs
    Fighting,
};

struct DuelState {
    DuelPhase phase = DuelPhase::None;
    u64 arbiter = 0;   // the flag object
    u64 initiator = 0; // who challenged
    u64 opponent = 0;  // the other player
    u32 startsAtMs = 0;
    bool outOfBounds = false;
};

// The social side of the world session (phase 8): groups, chat channels, friends and ignore list, who, the
// guild, duels, AFK / DND and text emotes. Like Gameplay it parses its packets (handle), keeps their state for the
// interface and sends the player's requests through a PacketSink; names of players it only knows by GUID come from
// the QueryCache. Main thread only; nothing allocates per frame once warmed up.
class Social {
public:
    enum class Handled : u8 { No, Yes, Malformed };
    using Events = EventRing<SocialEvent, 64>;

    Social(const EntityStore& entities, QueryCache& queries);

    void setSink(PacketSink sink) { m_sink = sink; }
    // `newLogin`: another character (everything starts over); false for a new map of the same one.
    void enterWorld(u64 player, bool newLogin, u32 nowMs);
    void leaveWorld();
    void update(u32 nowMs);
    Handled handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    const Events& events() const { return m_events; }

    // --- Groups ---
    const GroupState& group() const { return m_group; }
    bool isLeader() const { return m_group.active && m_group.leader == m_player; }
    bool inGroupWith(u64 guid) const;
    bool inviteToGroup(std::string_view name); // CMSG_GROUP_INVITE
    void acceptGroupInvite();
    void declineGroupInvite();
    void uninvite(u64 guid); // CMSG_GROUP_UNINVITE_GUID (leader or assistant)
    void leaveGroup();       // CMSG_GROUP_DISBAND: the player leaves
    void promoteToLeader(u64 guid);
    void setLootMethod(net::LootMethod method, u64 master, u32 threshold);
    void convertToRaid();
    // Phase 16 (pullouts): the player's party, "party1".."party4" (GetPartyMember; WoW.exe's SMSG_GROUP_LIST handler
    // 0x5E6A40 fills its four slots with the members whose subgroup is the player's, in the list's order): in a raid
    // the others of the player's subgroup (none when it is alone in it), in a party everybody (all in subgroup 0).
    // What the party frames, their pets and the minimap show. Returns how many were written (at most four).
    u32 partyMembers(std::span<PartyMemberView> out) const;
    // GetNumPartyMembers (0x4E86D0): partyMembers' count.
    u32 partyMemberCount() const;
    // UnitInParty (0x4E7F70) for another player: one of partyMembers (a raid's other subgroups are not).
    bool inPartyWith(u64 guid) const;
    // Every other member of the group in the list's order (the 39 others of a full raid): the world map, the social
    // window's group list. Returns how many were written.
    u32 groupMembers(std::span<PartyMemberView> out) const;
    // --- Raids (phase 16) ---
    // GetNumRaidMembers: the raid's members with the player (0 when not in a raid).
    u32 raidMemberCount() const { return m_group.active && m_group.raid ? static_cast<u32>(m_group.members.size()) + 1 : 0; }
    // GetRaidRosterInfo(1..n): the whole raid, the player included (`self`), in raid index order ("raidN" = out[N-1]).
    // Order (phase 16, WoW.exe): the SMSG_GROUP_LIST handler (0x5E6A40) keeps the members in the list's order
    // (the server's join order, Group::SendUpdate) and appends the player last; 0x4BA5F0 makes that the
    // raid's array (0xB712A8, count at 0xB713E0) that GetRaidRosterInfo (0x4BB560) indexes. Returns how many
    // were written (0 when not in a raid).
    u32 raidMembers(std::span<PartyMemberView> out) const;
    // IsRaidLeader() / IsRaidOfficer() (leader or assistant): what moving members and ready checks need.
    bool isRaidLeader() const { return m_group.raid && isLeader(); }
    bool isRaidOfficer() const { return m_group.raid && (isLeader() || m_group.ownAssistant); }
    // SetRaidSubgroup(index, subgroup): moves a member (the player too) to an empty place of subgroup 0..7 (the
    // server ignores a full one). SwapRaidSubgroup(index1, index2): exchanges two members' subgroups. Only the
    // leader and assistants are obeyed. False when not sent (not allowed, unknown member, same subgroup).
    bool setRaidSubgroup(u64 member, u8 subgroup);
    bool swapRaidSubgroups(u64 member, u64 other);
    // A member's subgroup 0..7 (the player's too), -1 when not in the group.
    i32 subgroupOf(u64 guid) const;
    // Parity (auras): a member's auras from its stats (spell and slot only), for when its unit is out of sight.
    // Returns how many were written (0: not a member).
    u32 memberAuras(u64 guid, std::span<AuraInfo> out) const;

    // --- Chat channels ---
    // Channel numbers 1..10 (index + 1); empty names are free numbers.
    const std::array<ChatChannel, kMaxChatChannels>& channels() const { return m_channels; }
    // Number of a joined channel (case insensitive), 0 if not in it.
    u32 channelNumber(std::string_view name) const;
    const ChatChannel* channelByNumber(u32 number) const;
    bool joinChannel(std::string_view name, std::string_view password = {});
    bool leaveChannel(std::string_view name);
    void listChannel(std::string_view name); // CMSG_CHANNEL_LIST: answered as a ChannelMembers event
    const net::ChannelList& channelMembers() const { return m_channelList; }
    // The zone the player stands in (AreaTable zone id) and the zone channels it has: joins the missing ones and
    // leaves those of the last zone, keeping each kind of channel on its number (General stays /1). Call it when
    // zone() differs (0 after a new login: the channels are joined anew).
    void setZone(u32 zoneId, std::span<const ZoneChannel> channels);
    u32 zone() const { return m_zoneId; }

    // --- AFK / DND / emotes ---
    bool afk() const;
    bool dnd() const;
    // Toggle (an empty message switches it off when on; a message while on just changes it).
    void setAfk(std::string_view message);
    void setDnd(std::string_view message);
    // CMSG_TEXT_EMOTE (EmotesText.dbc id) at `target` (0: none).
    void textEmote(u32 textEmote, u64 target);
    // MSG_RANDOM_ROLL: a number between the two (the group sees it).
    void roll(u32 minimum, u32 maximum);

    // --- Friends and ignore list ---
    const std::vector<FriendInfo>& friends() const { return m_friends; }
    const std::vector<u64>& ignored() const { return m_ignored; }
    bool isFriend(u64 guid) const;
    bool isIgnored(u64 guid) const;
    void addFriend(std::string_view name);
    void removeFriend(u64 guid);
    void addIgnore(std::string_view name);
    void removeIgnore(u64 guid);
    void refreshFriends(); // CMSG_FRIEND_LIST

    // --- Who ---
    void who(const net::WhoRequest& request);
    const WhoResults& who() const { return m_who; }

    // --- Guild ---
    const GuildState& guild() const { return m_guild; }
    // Name of any guild by id (other players' PLAYER_GUILDID): asked for when missing, empty until it arrives.
    std::string_view guildName(u32 guildId);
    // Phase 15 (guild): the tabard design of a guild whose query answer came (nullptr until then: guildName() asks),
    // and a number that bumps with every design that came or changed (looks built from them rebuild).
    const GuildEmblem* guildEmblem(u32 guildId) const;
    u32 emblemVersion() const { return m_emblemVersion; }
    void requestRoster(); // CMSG_GUILD_ROSTER (+ CMSG_GUILD_INFO the first time)
    void guildInvite(std::string_view name);
    void acceptGuildInvite();
    void declineGuildInvite();
    void leaveGuild();
    void guildPromote(std::string_view name);
    void guildDemote(std::string_view name);
    void guildRemove(std::string_view name);
    void setGuildMotd(std::string_view motd);

    // --- Duels ---
    const DuelState& duel() const { return m_duel; }
    // The player cast kDuelSpell at `target` (Gameplay::castSpell): remembered as the opponent.
    void challengeSent(u64 target) { m_challengeTarget = target; }
    void acceptDuel();  // CMSG_DUEL_ACCEPTED
    void declineDuel(); // CMSG_DUEL_CANCELLED: refuses a challenge, withdraws one or forfeits

private:
    bool send(net::Opcode opcode);
    bool sendBody(net::Opcode opcode);
    bool sendName(net::Opcode opcode, std::string_view name);
    bool sendGuid(net::Opcode opcode, u64 guid);
    SocialEvent& event(SocialEventKind kind);
    const Entity* me() const { return m_entities.find(m_player); }

    void onGroupList(const net::GroupList& list);
    void onPartyStats(const net::PartyMemberStats& stats, bool full = false); // full: SMSG_PARTY_MEMBER_STATS_FULL
    // Phase 15 (pets): the member's pet in its stats, and in its party frame's view (`member`: its entity, if seen).
    void onPartyPetStats(GroupMember& m, const net::PartyMemberStats& stats, bool full);
    void partyPet(const GroupMember& m, const Entity* member, PartyMemberView& v) const;
    void memberView(const GroupMember& m, PartyMemberView& v) const; // phase 16: one member's party / raid view
    void selfView(PartyMemberView& v) const;                         // phase 16: the player's raid entry
    std::string_view memberName(u64 guid) const;                     // phase 16: the player's or a member's name
    void leftGroup();
    void onChannelNotify(const net::ChannelNotify& n);
    i32 findChannel(std::string_view name) const; // index, -1 none
    i32 allocateChannel(u32 zoneChannel);        // a free number (zone kinds keep theirs), -1 none
    void freeChannel(u32 index);
    void onGuildRoster(const net::GuildRoster& roster);
    void updateDuel(u32 nowMs);
    void updateGuild();
    void updateAway();

    const EntityStore& m_entities;
    QueryCache& m_queries;
    PacketSink m_sink;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;
    std::vector<u8> m_body;
    Events m_events;

    GroupState m_group;
    std::vector<u64> m_previousMembers; // scratch for the joined / left diff
    std::array<ChatChannel, kMaxChatChannels> m_channels;
    std::array<u32, kMaxChatChannels> m_reserved{}; // zone channel kind kept on each number (0: none)
    u32 m_zoneId = 0;
    net::ChannelList m_channelList;
    std::vector<FriendInfo> m_friends;
    std::vector<u64> m_ignored;
    std::string m_lastFriendName; // the name the player asked to add (FriendStatus has only the GUID)
    WhoResults m_who;
    GuildState m_guild;
    bool m_rosterAsked = false;
    bool m_guildInfoAsked = false;
    std::unordered_map<u32, std::string> m_guildNames;
    std::unordered_map<u32, bool> m_guildAsked;
    std::unordered_map<u32, GuildEmblem> m_guildEmblems; // phase 15 (guild)
    u32 m_emblemVersion = 0;
    DuelState m_duel;
    u64 m_challengeTarget = 0;
    u32 m_playerFlags = 0;
    bool m_flagsKnown = false;

    // Parsed messages, reused.
    net::GroupList m_groupList;
    net::PartyMemberStats m_partyStats;
    net::GuildRoster m_roster;
    net::WhoList m_whoList;
    std::vector<net::FriendEntry> m_friendEntries;
};

} // namespace vd::game
