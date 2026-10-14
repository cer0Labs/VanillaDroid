#include "game/Social.h"

#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

bool due(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Channel names compare without case (the server lowercases them too).
bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

bool isMembershipEvent(net::GuildEventKind kind) {
    switch (kind) {
    case net::GuildEventKind::Promotion:
    case net::GuildEventKind::Demotion:
    case net::GuildEventKind::Joined:
    case net::GuildEventKind::Left:
    case net::GuildEventKind::Removed:
    case net::GuildEventKind::LeaderChanged:
    case net::GuildEventKind::RosterUpdate:
    case net::GuildEventKind::SignedOn:
    case net::GuildEventKind::SignedOff: return true;
    default: return false;
    }
}

} // namespace

Social::Social(const EntityStore& entities, QueryCache& queries) : m_entities(entities), m_queries(queries) {
    m_body.reserve(128);
    m_group.members.reserve(net::kMaxRaidSize);
    m_previousMembers.reserve(net::kMaxRaidSize);
    m_groupList.members.reserve(net::kMaxRaidSize);
    m_friends.reserve(64);
    m_ignored.reserve(64);
    m_who.entries.reserve(50);
    m_whoList.entries.reserve(50);
}

// -------------------------------------------------------------------------------------------------------
// Session.

void Social::enterWorld(u64 player, bool newLogin, u32 nowMs) {
    m_nowMs = nowMs;
    m_inWorld = true;
    if (newLogin || player != m_player) {
        // Another character: the server sends the group, lists and guild again; channels are joined anew.
        m_player = player;
        m_group = GroupState{};
        m_group.members.reserve(net::kMaxRaidSize);
        for (ChatChannel& c : m_channels) {
            c = ChatChannel{};
        }
        m_reserved = {};
        m_zoneId = 0;
        m_friends.clear();
        m_ignored.clear();
        m_who.entries.clear();
        m_who.online = 0;
        m_who.pending = false;
        m_guild = GuildState{};
        m_rosterAsked = false;
        m_guildInfoAsked = false;
        m_duel = DuelState{};
        m_challengeTarget = 0;
        m_flagsKnown = false;
    }
}

void Social::leaveWorld() {
    m_inWorld = false;
    m_who.pending = false;
}

void Social::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    updateAway();
    updateGuild();
    updateDuel(nowMs);
    // Members out of sight: their stats once (the server keeps them coming while anything changes).
    for (GroupMember& m : m_group.members) {
        if (const Entity* e = m_entities.find(m.guid); e && e->values.size() >= uf::UNIT_END) {
            for (u32 slot = 0; slot < kMaxAuras; ++slot) { // parity (auras): what it had when it left sight
                m.auras[slot] = static_cast<u16>(e->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot)));
            }
        }
        if (!m.statsRequested && (m.status & net::kMemberOnline) && m.statsMask == 0 && !m_entities.find(m.guid)) {
            m.statsRequested = true;
            sendGuid(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS, m.guid);
        }
        // Phase 16 (raids): GetRaidRosterInfo knows every member's class; the group list names them only, so the
        // class of those never seen comes from a name query (asked once).
        if (m_group.raid && !m_queries.findPlayerInfo(m.guid) && !m_entities.find(m.guid)) {
            m_queries.playerInfo(m.guid);
        }
    }
}

void Social::updateAway() {
    const Entity* player = me();
    if (!player) {
        return;
    }
    const u32 flags = player->value(uf::PLAYER_FLAGS) & (kPlayerFlagAfk | kPlayerFlagDnd);
    if (m_flagsKnown && flags != m_playerFlags) {
        SocialEvent& e = event(SocialEventKind::AwayChanged);
        e.value[0] = (flags & kPlayerFlagAfk) ? 1 : 0;
        e.value[1] = (flags & kPlayerFlagDnd) ? 1 : 0;
    }
    m_playerFlags = flags;
    m_flagsKnown = true;
}

void Social::updateGuild() {
    const Entity* player = me();
    if (!player) {
        return;
    }
    const u32 id = player->value(uf::PLAYER_GUILDID);
    m_guild.rank = player->value(uf::PLAYER_GUILDRANK);
    if (id == m_guild.id) {
        return;
    }
    // Joined, left or another guild: start over.
    const std::string inviter = std::move(m_guild.pendingInviter);
    const std::string invitedTo = std::move(m_guild.pendingGuild);
    const u32 rank = m_guild.rank;
    m_guild = GuildState{};
    m_guild.id = id;
    m_guild.rank = rank;
    m_rosterAsked = false;
    m_guildInfoAsked = false;
    if (id == 0) {
        m_guild.pendingInviter = inviter;
        m_guild.pendingGuild = invitedTo;
        return;
    }
    guildName(id); // name and ranks
    requestRoster();
}

void Social::updateDuel(u32 nowMs) {
    if (m_duel.phase == DuelPhase::None) {
        return;
    }
    if (m_duel.phase == DuelPhase::Countdown && due(nowMs, m_duel.startsAtMs)) {
        m_duel.phase = DuelPhase::Fighting;
    }
    // The opponent is the other player under the same flag (PLAYER_DUEL_ARBITER is public).
    if (m_duel.arbiter != 0) {
        m_entities.forEach([&](const Entity& e) {
            if (e.type == ObjectType::Player && e.guid != m_player &&
                guidValue(e, uf::PLAYER_DUEL_ARBITER) == m_duel.arbiter) {
                m_duel.opponent = e.guid;
            }
        });
    }
}

// -------------------------------------------------------------------------------------------------------
// Sending.

bool Social::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool Social::send(Opcode opcode) {
    m_body.clear();
    return sendBody(opcode);
}

bool Social::sendName(Opcode opcode, std::string_view name) {
    m_body.clear();
    net::writeCString(m_body, name);
    return sendBody(opcode);
}

bool Social::sendGuid(Opcode opcode, u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    return sendBody(opcode);
}

SocialEvent& Social::event(SocialEventKind kind) {
    SocialEvent& e = m_events.push();
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.guid = 0;
    e.other = 0;
    e.value = {};
    e.name.clear(); // keeps the capacity
    e.text.clear();
    e.extra.clear();
    return e;
}

// -------------------------------------------------------------------------------------------------------
// Packets.

Social::Handled Social::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    bool ok = true;
    switch (opcode) {
    // Groups.
    case Opcode::SMSG_GROUP_INVITE: {
        std::string_view inviter;
        ok = net::parseCString(p, inviter);
        if (ok) {
            m_group.pendingInviter.assign(inviter);
            event(SocialEventKind::GroupInvite).name.assign(inviter);
        }
        break;
    }
    case Opcode::SMSG_GROUP_DECLINE: {
        std::string_view name;
        ok = net::parseCString(p, name);
        if (ok) {
            event(SocialEventKind::GroupDeclined).name.assign(name);
        }
        break;
    }
    case Opcode::SMSG_GROUP_SET_LEADER: {
        std::string_view name;
        ok = net::parseCString(p, name);
        if (ok) {
            for (const GroupMember& m : m_group.members) {
                if (m.name == name) {
                    m_group.leader = m.guid;
                }
            }
            const std::string* own = m_queries.findPlayerName(m_player);
            if (own && *own == name) {
                m_group.leader = m_player;
            }
            event(SocialEventKind::LeaderChanged).name.assign(name);
        }
        break;
    }
    case Opcode::SMSG_GROUP_UNINVITE: event(SocialEventKind::GroupUninvited); break;
    case Opcode::SMSG_GROUP_DESTROYED: event(SocialEventKind::GroupDestroyed); break;
    case Opcode::SMSG_GROUP_LIST:
        ok = net::parseGroupList(p, m_groupList);
        if (ok) {
            onGroupList(m_groupList);
        }
        break;
    case Opcode::SMSG_PARTY_MEMBER_STATS:
    case Opcode::SMSG_PARTY_MEMBER_STATS_FULL:
        ok = net::parsePartyMemberStats(p, m_partyStats);
        if (ok) {
            onPartyStats(m_partyStats, opcode == Opcode::SMSG_PARTY_MEMBER_STATS_FULL);
        }
        break;
    case Opcode::SMSG_PARTY_COMMAND_RESULT: {
        net::PartyCommandResult m;
        ok = net::parsePartyCommandResult(p, m);
        if (ok) {
            SocialEvent& e = event(SocialEventKind::PartyResult);
            e.value[0] = static_cast<i32>(m.operation);
            e.value[1] = static_cast<i32>(m.result);
            e.name.assign(m.name);
        }
        break;
    }

    // Channels.
    case Opcode::SMSG_CHANNEL_NOTIFY: {
        net::ChannelNotify m;
        ok = net::parseChannelNotify(p, m);
        if (ok) {
            onChannelNotify(m);
        }
        break;
    }
    case Opcode::SMSG_CHANNEL_LIST:
        ok = net::parseChannelList(p, m_channelList);
        if (ok) {
            for (const net::ChannelList::Member& m : m_channelList.members) {
                m_queries.playerName(m.guid);
            }
            SocialEvent& e = event(SocialEventKind::ChannelMembers);
            e.name.assign(m_channelList.channel);
            e.value[0] = static_cast<i32>(m_channelList.members.size());
        }
        break;

    // Friends and ignore list.
    case Opcode::SMSG_FRIEND_LIST:
        ok = net::parseFriendList(p, m_friendEntries);
        if (ok) {
            m_friends.clear();
            for (const net::FriendEntry& f : m_friendEntries) {
                m_friends.push_back({f.guid, f.status, f.area, f.level, f.cls});
                m_queries.playerName(f.guid);
            }
        }
        break;
    case Opcode::SMSG_IGNORE_LIST:
        ok = net::parseIgnoreList(p, m_ignored);
        if (ok) {
            for (const u64 guid : m_ignored) {
                m_queries.playerName(guid);
            }
        }
        break;
    case Opcode::SMSG_FRIEND_STATUS: {
        net::FriendStatus m;
        ok = net::parseFriendStatus(p, m);
        if (!ok) {
            break;
        }
        const auto result = static_cast<net::FriendResult>(m.result);
        auto found = std::find_if(m_friends.begin(), m_friends.end(),
                                  [&](const FriendInfo& f) { return f.guid == m.guid; });
        std::string_view name;
        if (const std::string* known = m_queries.findPlayerName(m.guid)) {
            name = *known;
        }
        switch (result) {
        case net::FriendResult::AddedOnline:
        case net::FriendResult::AddedOffline:
        case net::FriendResult::Online:
        case net::FriendResult::Offline: {
            if (found == m_friends.end()) {
                m_friends.push_back({m.guid});
                found = m_friends.end() - 1;
            }
            found->status = m.hasInfo ? std::max<u8>(m.status, net::kFriendOnline) : net::kFriendOffline;
            found->area = m.area;
            found->level = m.level;
            found->cls = m.cls;
            if ((result == net::FriendResult::AddedOnline || result == net::FriendResult::AddedOffline) &&
                name.empty() && !m_lastFriendName.empty()) {
                net::NameQueryInfo info; // what the player typed is the name (the server found it by it)
                info.guid = m.guid;
                info.name = m_lastFriendName;
                m_queries.onName(std::move(info));
                name = m_lastFriendName;
            }
            m_queries.playerName(m.guid);
            break;
        }
        case net::FriendResult::Removed:
            if (found != m_friends.end()) {
                m_friends.erase(found);
            }
            break;
        case net::FriendResult::IgnoreAdded:
            if (std::find(m_ignored.begin(), m_ignored.end(), m.guid) == m_ignored.end()) {
                m_ignored.push_back(m.guid);
            }
            if (name.empty() && !m_lastFriendName.empty()) {
                net::NameQueryInfo info;
                info.guid = m.guid;
                info.name = m_lastFriendName;
                m_queries.onName(std::move(info));
                name = m_lastFriendName;
            }
            break;
        case net::FriendResult::IgnoreRemoved: std::erase(m_ignored, m.guid); break;
        default: break; // errors: only the event
        }
        SocialEvent& e = event(SocialEventKind::FriendStatus);
        e.guid = m.guid;
        e.value = {static_cast<i32>(m.result), m.status, static_cast<i32>(m.area)};
        e.name.assign(name.empty() && m.guid == 0 ? std::string_view(m_lastFriendName) : name);
        break;
    }

    // Who.
    case Opcode::SMSG_WHO:
        ok = net::parseWhoList(p, m_whoList);
        if (ok) {
            m_who.entries.resize(m_whoList.entries.size());
            for (usize i = 0; i < m_whoList.entries.size(); ++i) {
                const net::WhoList::Entry& in = m_whoList.entries[i];
                WhoEntry& out = m_who.entries[i];
                out.name.assign(in.name);
                out.guild.assign(in.guild);
                out.level = in.level;
                out.cls = in.cls;
                out.race = in.race;
                out.zone = in.zone;
            }
            m_who.online = m_whoList.online;
            m_who.pending = false;
            ++m_who.version;
            SocialEvent& e = event(SocialEventKind::WhoResults);
            e.value[0] = static_cast<i32>(m_who.entries.size());
            e.value[1] = static_cast<i32>(m_who.online);
        }
        break;

    // Guild.
    case Opcode::SMSG_GUILD_QUERY_RESPONSE: {
        net::GuildQueryResponse m;
        ok = net::parseGuildQueryResponse(p, m);
        if (ok) {
            m_guildNames[m.guildId].assign(m.name);
            // Phase 15 (guild): the guild's tabard design (its members' guild tabards show it).
            GuildEmblem emblem;
            emblem.emblemStyle = m.emblemStyle;
            emblem.emblemColor = m.emblemColor;
            emblem.borderStyle = m.borderStyle;
            emblem.borderColor = m.borderColor;
            emblem.backgroundColor = m.backgroundColor;
            GuildEmblem& known = m_guildEmblems[m.guildId];
            if (known != emblem) {
                known = emblem;
                ++m_emblemVersion;
            }
            if (m.guildId == m_guild.id && m.guildId != 0) {
                m_guild.name.assign(m.name);
                for (u32 i = 0; i < net::kGuildRankCount; ++i) {
                    m_guild.ranks[i].assign(m.ranks[i]);
                }
                m_guild.known = true;
            }
        }
        break;
    }
    case Opcode::SMSG_GUILD_ROSTER:
        ok = net::parseGuildRoster(p, m_roster);
        if (ok) {
            onGuildRoster(m_roster);
        }
        break;
    case Opcode::SMSG_GUILD_EVENT: {
        net::GuildEvent m;
        ok = net::parseGuildEvent(p, m);
        if (!ok) {
            break;
        }
        const auto kind = static_cast<net::GuildEventKind>(m.event);
        if (kind == net::GuildEventKind::Motd && m.count > 0) {
            m_guild.motd.assign(m.params[0]);
        }
        if (isMembershipEvent(kind) && m_rosterAsked) {
            send(Opcode::CMSG_GUILD_ROSTER); // keep the open roster current
        }
        SocialEvent& e = event(SocialEventKind::GuildEvent);
        e.value[0] = m.event;
        e.value[1] = static_cast<i32>(m.count);
        e.guid = m.guid;
        e.name.assign(m.params[0]);
        e.text.assign(m.params[1]);
        e.extra.assign(m.params[2]);
        break;
    }
    case Opcode::SMSG_GUILD_INVITE: {
        net::GuildInvite m;
        ok = net::parseGuildInvite(p, m);
        if (ok) {
            m_guild.pendingInviter.assign(m.inviter);
            m_guild.pendingGuild.assign(m.guild);
            SocialEvent& e = event(SocialEventKind::GuildInvite);
            e.name.assign(m.inviter);
            e.text.assign(m.guild);
        }
        break;
    }
    case Opcode::SMSG_GUILD_DECLINE: {
        std::string_view name;
        ok = net::parseCString(p, name);
        if (ok) {
            event(SocialEventKind::GuildDeclined).name.assign(name);
        }
        break;
    }
    case Opcode::SMSG_GUILD_COMMAND_RESULT: {
        net::GuildCommandResult m;
        ok = net::parseGuildCommandResult(p, m);
        if (ok) {
            SocialEvent& e = event(SocialEventKind::GuildCommandResult);
            e.value[0] = static_cast<i32>(m.command);
            e.value[1] = static_cast<i32>(m.result);
            e.name.assign(m.name);
        }
        break;
    }
    case Opcode::SMSG_GUILD_INFO: {
        net::GuildInfo m;
        ok = net::parseGuildInfo(p, m);
        if (ok) {
            m_guild.createdDay = m.day;
            m_guild.createdMonth = m.month;
            m_guild.createdYear = m.year;
            m_guild.accounts = m.accounts;
            m_guild.memberCount = m.members; // phase 13 (social): /ginfo's lines
            SocialEvent& e = event(SocialEventKind::GuildInfo);
            e.name.assign(m.name);
            e.value = {static_cast<i32>(m.month), static_cast<i32>(m.day), static_cast<i32>(m.year)};
        }
        break;
    }

    // Duels.
    case Opcode::SMSG_DUEL_REQUESTED: {
        net::DuelRequested m;
        ok = net::parseDuelRequested(p, m);
        if (ok) {
            const bool mine = m.initiator == m_player;
            m_duel = DuelState{};
            m_duel.phase = mine ? DuelPhase::Challenging : DuelPhase::Challenged;
            m_duel.arbiter = m.arbiter;
            m_duel.initiator = m.initiator;
            m_duel.opponent = mine ? m_challengeTarget : m.initiator;
            if (!mine) {
                m_queries.playerName(m.initiator);
            }
            SocialEvent& e = event(SocialEventKind::DuelRequested);
            e.guid = m.initiator;
            e.other = m.arbiter;
            e.value[0] = mine ? 1 : 0;
        }
        break;
    }
    case Opcode::SMSG_DUEL_COUNTDOWN: {
        u32 ms = 0;
        ok = net::parseU32(p, ms);
        if (ok) {
            m_duel.phase = DuelPhase::Countdown;
            m_duel.startsAtMs = nowMs + ms;
            event(SocialEventKind::DuelCountdown).value[0] = static_cast<i32>(ms);
        }
        break;
    }
    case Opcode::SMSG_DUEL_OUTOFBOUNDS:
        m_duel.outOfBounds = true;
        event(SocialEventKind::DuelOutOfBounds);
        break;
    case Opcode::SMSG_DUEL_INBOUNDS:
        m_duel.outOfBounds = false;
        event(SocialEventKind::DuelInBounds);
        break;
    case Opcode::SMSG_DUEL_COMPLETE: {
        u8 fought = 0;
        ok = net::parseU8(p, fought);
        if (ok) {
            m_duel = DuelState{};
            m_challengeTarget = 0;
            event(SocialEventKind::DuelComplete).value[0] = fought ? 1 : 0;
        }
        break;
    }
    case Opcode::SMSG_DUEL_WINNER: {
        net::DuelWinner m;
        ok = net::parseDuelWinner(p, m);
        if (ok) {
            const std::string* own = m_queries.findPlayerName(m_player);
            SocialEvent& e = event(SocialEventKind::DuelWinner);
            e.name.assign(m.winner);
            e.text.assign(m.loser);
            e.value[0] = m.fled ? 1 : 0;
            e.value[1] = own && (*own == m.winner || *own == m.loser) ? 1 : 0;
        }
        break;
    }

    case Opcode::MSG_RANDOM_ROLL: {
        net::RandomRoll m;
        ok = net::parseRandomRoll(p, m);
        if (ok) {
            SocialEvent& e = event(SocialEventKind::Roll);
            e.guid = m.guid;
            e.value = {static_cast<i32>(m.roll), static_cast<i32>(m.minimum), static_cast<i32>(m.maximum)};
            if (const std::string* name = m_queries.playerName(m.guid)) {
                e.name.assign(*name);
            }
        }
        break;
    }

    default: return Handled::No;
    }
    return ok ? Handled::Yes : Handled::Malformed;
}

// -------------------------------------------------------------------------------------------------------
// Groups.

void Social::onGroupList(const net::GroupList& list) {
    const bool wasActive = m_group.active;
    if (list.members.empty()) {
        if (wasActive) {
            leftGroup();
        }
        return;
    }
    m_previousMembers.clear();
    for (const GroupMember& m : m_group.members) {
        m_previousMembers.push_back(m.guid);
    }
    const u8 oldMethod = m_group.lootMethod;
    const u8 oldThreshold = m_group.lootThreshold;
    const u64 oldLooter = m_group.looter;
    m_group.active = true;
    m_group.raid = (list.groupType & net::kGroupTypeRaid) != 0;
    m_group.leader = list.leader;
    m_group.lootMethod = list.lootMethod;
    m_group.looter = list.looter;
    m_group.lootThreshold = list.lootThreshold;
    m_group.ownSubgroup = list.ownSubgroup;
    m_group.ownAssistant = list.ownAssistant;
    m_group.pendingInviter.clear();

    // Members in the list's order, each keeping the stats it had.
    std::vector<GroupMember>& members = m_group.members;
    for (usize i = 0; i < list.members.size(); ++i) {
        const net::GroupList::Member& in = list.members[i];
        usize at = i;
        while (at < members.size() && members[at].guid != in.guid) {
            ++at;
        }
        if (at < members.size()) {
            std::swap(members[i], members[at]);
        } else {
            members.insert(members.begin() + static_cast<std::ptrdiff_t>(i), GroupMember{});
            members[i].guid = in.guid;
        }
        GroupMember& m = members[i];
        m.name.assign(in.name);
        m.status = in.status;
        m.subgroup = in.subgroup;
        m.assistant = in.assistant;
        if (!(in.status & net::kMemberOnline)) {
            m.statsRequested = false; // ask again when it comes back
        }
        net::NameQueryInfo info; // the list names everyone: no name queries needed for them
        if (!m_queries.findPlayerName(in.guid)) {
            info.guid = in.guid;
            info.name.assign(in.name);
            m_queries.onName(std::move(info));
        }
    }
    members.resize(list.members.size());

    if (!wasActive) {
        event(SocialEventKind::GroupJoined);
        return;
    }
    for (const GroupMember& m : members) {
        if (std::find(m_previousMembers.begin(), m_previousMembers.end(), m.guid) == m_previousMembers.end()) {
            SocialEvent& e = event(SocialEventKind::MemberJoined);
            e.guid = m.guid;
            e.name.assign(m.name);
        }
    }
    for (const u64 guid : m_previousMembers) {
        if (!inGroupWith(guid)) {
            SocialEvent& e = event(SocialEventKind::MemberLeft);
            e.guid = guid;
            if (const std::string* name = m_queries.findPlayerName(guid)) {
                e.name.assign(*name);
            }
        }
    }
    if (oldMethod != m_group.lootMethod || oldThreshold != m_group.lootThreshold || oldLooter != m_group.looter) {
        SocialEvent& e = event(SocialEventKind::LootMethodChanged);
        e.value[0] = m_group.lootMethod;
        e.value[1] = m_group.lootThreshold;
        e.other = m_group.looter;
    }
}

void Social::leftGroup() {
    m_group.active = false;
    m_group.raid = false;
    m_group.leader = 0;
    m_group.looter = 0;
    m_group.lootMethod = 0;
    m_group.lootThreshold = 0;
    m_group.members.clear();
    event(SocialEventKind::GroupLeft);
}

void Social::onPartyStats(const net::PartyMemberStats& s, bool full) {
    for (GroupMember& m : m_group.members) {
        if (m.guid != s.guid) {
            continue;
        }
        m.statsMask |= s.mask;
        if (s.mask & net::kGroupUpdateStatus) {
            m.status = s.status;
        }
        if (s.mask & net::kGroupUpdateCurHp) {
            m.health = s.health;
        }
        if (s.mask & net::kGroupUpdateMaxHp) {
            m.maxHealth = s.maxHealth;
        }
        if (s.mask & net::kGroupUpdatePowerType) {
            m.powerType = s.powerType;
        }
        if (s.mask & net::kGroupUpdateCurPower) {
            m.power = s.power;
        }
        if (s.mask & net::kGroupUpdateMaxPower) {
            m.maxPower = s.maxPower;
        }
        if (s.mask & net::kGroupUpdateLevel) {
            m.level = s.level;
        }
        if (s.mask & net::kGroupUpdateZone) {
            m.zone = s.zone;
        }
        if (s.mask & net::kGroupUpdatePosition) {
            m.x = s.x;
            m.y = s.y;
        }
        // Auras (WorldSession::BuildPartyMemberStatsPacket): the full packet names every occupied slot, the others
        // the slots that changed (0: emptied).
        if (full && (s.mask & (net::kGroupUpdateAuras | net::kGroupUpdateAurasNegative))) {
            m.auras.fill(0);
        }
        for (u32 i = 0; i < 32 && (s.mask & net::kGroupUpdateAuras); ++i) {
            if (s.auraMask & (1u << i)) {
                m.auras[i] = s.auras[i];
            }
        }
        for (u32 i = 0; i < 16 && (s.mask & net::kGroupUpdateAurasNegative); ++i) {
            if (s.negativeAuraMask & (1u << i)) {
                m.auras[kMaxPositiveAuras + i] = s.negativeAuras[i];
            }
        }
        onPartyPetStats(m, s, full); // phase 15 (pets)
        return;
    }
}

void Social::onPartyPetStats(GroupMember& m, const net::PartyMemberStats& s, bool full) {
    // Phase 15 (pets): the GROUP_UPDATE_FLAG_PET_* fields (BuildPartyMemberStatsPacket writes zeros and an empty name
    // once the pet is gone), the auras as the member's.
    if (s.mask & net::kGroupUpdatePetGuid) {
        if (s.petGuid != m.petGuid) {
            m.petAuras.fill(0); // another pet, or none
        }
        m.petGuid = s.petGuid;
    }
    if (s.mask & net::kGroupUpdatePetName) {
        m.petName.assign(s.petName);
    }
    if (s.mask & net::kGroupUpdatePetModel) {
        m.petDisplay = s.petDisplay;
    }
    if (s.mask & net::kGroupUpdatePetCurHp) {
        m.petHealth = s.petHealth;
    }
    if (s.mask & net::kGroupUpdatePetMaxHp) {
        m.petMaxHealth = s.petMaxHealth;
    }
    if (s.mask & net::kGroupUpdatePetPowerType) {
        m.petPowerType = s.petPowerType;
    }
    if (s.mask & net::kGroupUpdatePetCurPower) {
        m.petPower = s.petPower;
    }
    if (s.mask & net::kGroupUpdatePetMaxPower) {
        m.petMaxPower = s.petMaxPower;
    }
    if (full && (s.mask & (net::kGroupUpdatePetAuras | net::kGroupUpdatePetAurasNegative))) {
        m.petAuras.fill(0);
    }
    for (u32 i = 0; i < 32 && (s.mask & net::kGroupUpdatePetAuras); ++i) {
        if (s.petAuraMask & (1u << i)) {
            m.petAuras[i] = s.petAuras[i];
        }
    }
    for (u32 i = 0; i < 16 && (s.mask & net::kGroupUpdatePetAurasNegative); ++i) {
        if (s.petNegativeAuraMask & (1u << i)) {
            m.petAuras[kMaxPositiveAuras + i] = s.petNegativeAuras[i];
        }
    }
}

u32 Social::memberAuras(u64 guid, std::span<AuraInfo> out) const {
    for (const GroupMember& m : m_group.members) {
        // Phase 15 (pets): a member's pet out of sight has its auras in the same stats.
        const bool pet = guid != 0 && m.petGuid == guid;
        if (m.guid != guid && !pet) {
            continue;
        }
        const std::array<u16, kMaxAuras>& auras = pet ? m.petAuras : m.auras;
        u32 count = 0;
        for (u32 slot = 0; slot < auras.size() && count < out.size(); ++slot) {
            if (auras[slot] != 0) {
                AuraInfo& a = out[count++];
                a = AuraInfo{};
                a.spellId = auras[slot];
                a.slot = static_cast<u8>(slot);
            }
        }
        return count;
    }
    return 0;
}

bool Social::inGroupWith(u64 guid) const {
    for (const GroupMember& m : m_group.members) {
        if (m.guid == guid) {
            return true;
        }
    }
    return false;
}

bool Social::inviteToGroup(std::string_view name) {
    return !name.empty() && sendName(Opcode::CMSG_GROUP_INVITE, name);
}

void Social::acceptGroupInvite() {
    if (!m_group.pendingInviter.empty()) {
        send(Opcode::CMSG_GROUP_ACCEPT);
        m_group.pendingInviter.clear();
    }
}

void Social::declineGroupInvite() {
    if (!m_group.pendingInviter.empty()) {
        send(Opcode::CMSG_GROUP_DECLINE);
        m_group.pendingInviter.clear();
    }
}

void Social::uninvite(u64 guid) {
    if (guid != 0 && guid != m_player) {
        sendGuid(Opcode::CMSG_GROUP_UNINVITE_GUID, guid);
    }
}

void Social::leaveGroup() {
    if (m_group.active) {
        send(Opcode::CMSG_GROUP_DISBAND);
    }
}

void Social::promoteToLeader(u64 guid) {
    if (isLeader() && inGroupWith(guid)) {
        sendGuid(Opcode::CMSG_GROUP_SET_LEADER, guid);
    }
}

void Social::setLootMethod(net::LootMethod method, u64 master, u32 threshold) {
    if (!isLeader()) {
        return;
    }
    m_body.clear();
    net::writeLootMethod(m_body, static_cast<u32>(method), method == net::LootMethod::MasterLoot ? master : 0,
                         threshold);
    sendBody(Opcode::CMSG_LOOT_METHOD);
}

void Social::convertToRaid() {
    if (isLeader() && !m_group.raid) {
        send(Opcode::CMSG_GROUP_RAID_CONVERT);
    }
}

u32 Social::partyMembers(std::span<PartyMemberView> out) const {
    // Phase 16 (pullouts): WoW.exe compares the list's flags without the assistant bit (& 0x7F) with the player's own;
    // a fifth match never gets a slot.
    u32 count = 0;
    for (const GroupMember& m : m_group.members) {
        if (count == out.size() || count == net::kMaxGroupSize - 1) {
            break;
        }
        if (m.subgroup == m_group.ownSubgroup) {
            memberView(m, out[count++]);
        }
    }
    return count;
}

u32 Social::partyMemberCount() const {
    u32 count = 0;
    for (const GroupMember& m : m_group.members) {
        count += m.subgroup == m_group.ownSubgroup && count < net::kMaxGroupSize - 1 ? 1 : 0;
    }
    return count;
}

bool Social::inPartyWith(u64 guid) const {
    u32 count = 0;
    for (const GroupMember& m : m_group.members) {
        if (m.subgroup != m_group.ownSubgroup) {
            continue;
        }
        if (count++ == net::kMaxGroupSize - 1) {
            break;
        }
        if (m.guid == guid) {
            return guid != 0;
        }
    }
    return false;
}

u32 Social::groupMembers(std::span<PartyMemberView> out) const {
    u32 count = 0;
    for (const GroupMember& m : m_group.members) {
        if (count == out.size()) {
            break;
        }
        memberView(m, out[count++]);
    }
    return count;
}

void Social::memberView(const GroupMember& m, PartyMemberView& v) const {
    v = PartyMemberView{};
    v.guid = m.guid;
    v.name = m.name;
    v.subgroup = m.subgroup;
    v.online = (m.status & net::kMemberOnline) != 0;
    v.dead = (m.status & net::kMemberDead) != 0;
    v.ghost = (m.status & net::kMemberGhost) != 0;
    v.afk = (m.status & net::kMemberAfk) != 0;
    v.dnd = (m.status & net::kMemberDnd) != 0;
    v.leader = m.guid == m_group.leader;
    v.assistant = m.assistant;
    v.zone = m.zone;
    const Entity* e = m_entities.find(m.guid);
    if (e && e->type == ObjectType::Player) {
        v.inSight = true;
        v.online = true;
        v.level = e->value(uf::UNIT_FIELD_LEVEL);
        v.health = e->value(uf::UNIT_FIELD_HEALTH);
        v.maxHealth = e->value(uf::UNIT_FIELD_MAXHEALTH);
        v.race = e->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
        v.cls = e->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        v.gender = e->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
        v.powerType = std::min<u8>(e->byteValue(uf::UNIT_FIELD_BYTES_0, 3), 4);
        v.power = e->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + v.powerType));
        v.maxPower = e->value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + v.powerType));
        v.ghost = isGhost(*e);
        v.dead = v.ghost || isDead(*e);
        const u32 flags = e->value(uf::PLAYER_FLAGS);
        v.afk = (flags & kPlayerFlagAfk) != 0;
        v.dnd = (flags & kPlayerFlagDnd) != 0;
        if (e->hasPosition) {
            const glm::vec3 p = e->positionAt(m_nowMs);
            v.hasPosition = true;
            v.x = p.x;
            v.y = p.y;
        }
        partyPet(m, e, v); // phase 15 (pets)
        return;
    }
    // Phase 16 (raids): the class out of sight, as GetRaidRosterInfo has it, from the name query (update() asks).
    if (const PlayerInfo* info = m_queries.findPlayerInfo(m.guid)) {
        v.race = info->race;
        v.cls = info->cls;
        v.gender = info->gender;
    }
    v.level = m.level;
    v.health = m.health;
    v.maxHealth = m.maxHealth;
    v.powerType = std::min<u8>(m.powerType, 4);
    v.power = m.power;
    v.maxPower = m.maxPower;
    if (m.statsMask & net::kGroupUpdatePosition) {
        v.hasPosition = true;
        v.x = m.x;
        v.y = m.y;
    }
    partyPet(m, nullptr, v); // phase 15 (pets)
}

void Social::selfView(PartyMemberView& v) const {
    v = PartyMemberView{};
    v.guid = m_player;
    v.name = memberName(m_player);
    v.self = true;
    v.subgroup = m_group.ownSubgroup;
    v.leader = m_group.leader == m_player;
    v.assistant = m_group.ownAssistant;
    v.online = true;
    v.inSight = true;
    v.zone = m_zoneId;
    const Entity* e = me();
    if (!e || e->values.size() < uf::UNIT_END) {
        return;
    }
    v.level = e->value(uf::UNIT_FIELD_LEVEL);
    v.health = e->value(uf::UNIT_FIELD_HEALTH);
    v.maxHealth = e->value(uf::UNIT_FIELD_MAXHEALTH);
    v.race = e->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    v.cls = e->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    v.gender = e->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    v.powerType = std::min<u8>(e->byteValue(uf::UNIT_FIELD_BYTES_0, 3), 4);
    v.power = e->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + v.powerType));
    v.maxPower = e->value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + v.powerType));
    v.ghost = isGhost(*e);
    v.dead = v.ghost || isDead(*e);
    const u32 flags = e->value(uf::PLAYER_FLAGS);
    v.afk = (flags & kPlayerFlagAfk) != 0;
    v.dnd = (flags & kPlayerFlagDnd) != 0;
    if (e->hasPosition) {
        const glm::vec3 p = e->positionAt(m_nowMs);
        v.hasPosition = true;
        v.x = p.x;
        v.y = p.y;
    }
    v.petGuid = guidValue(*e, uf::UNIT_FIELD_SUMMON);
}

std::string_view Social::memberName(u64 guid) const {
    if (guid != m_player) {
        for (const GroupMember& m : m_group.members) {
            if (m.guid == guid) {
                return m.name;
            }
        }
    }
    const std::string* name = m_queries.findPlayerName(guid);
    return name ? std::string_view(*name) : std::string_view{};
}

u32 Social::raidMembers(std::span<PartyMemberView> out) const {
    if (raidMemberCount() == 0) {
        return 0;
    }
    u32 count = 0;
    for (const GroupMember& m : m_group.members) {
        if (count == out.size()) {
            return count;
        }
        memberView(m, out[count++]);
    }
    if (count < out.size()) {
        selfView(out[count++]);
    }
    return count;
}

i32 Social::subgroupOf(u64 guid) const {
    if (!m_group.active) {
        return -1;
    }
    if (guid == m_player) {
        return m_group.ownSubgroup;
    }
    for (const GroupMember& m : m_group.members) {
        if (m.guid == guid) {
            return m.subgroup;
        }
    }
    return -1;
}

bool Social::setRaidSubgroup(u64 member, u8 subgroup) {
    const i32 current = subgroupOf(member);
    const std::string_view name = memberName(member);
    if (!isRaidOfficer() || current < 0 || current == subgroup || subgroup >= net::kRaidSubgroups || name.empty()) {
        return false;
    }
    m_body.clear();
    net::writeGroupChangeSubGroup(m_body, name, subgroup);
    return sendBody(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP);
}

bool Social::swapRaidSubgroups(u64 member, u64 other) {
    const i32 a = subgroupOf(member);
    const i32 b = subgroupOf(other);
    const std::string_view nameA = memberName(member);
    const std::string_view nameB = memberName(other);
    if (!isRaidOfficer() || a < 0 || b < 0 || a == b || nameA.empty() || nameB.empty()) {
        return false;
    }
    m_body.clear();
    net::writeGroupSwapSubGroup(m_body, nameA, nameB);
    return sendBody(Opcode::CMSG_GROUP_SWAP_SUB_GROUP);
}

void Social::partyPet(const GroupMember& m, const Entity* member, PartyMemberView& v) const {
    // Phase 15 (pets): UnitExists("partypetN"): the member's summon while the client sees the member (the server sends
    // stats only to those out of sight), the stats' pet otherwise; its health from its own fields when it is in sight.
    v.petGuid = member ? guidValue(*member, uf::UNIT_FIELD_SUMMON) : m.petGuid;
    if (v.petGuid == 0) {
        return;
    }
    v.petName = m.petGuid == v.petGuid ? std::string_view(m.petName) : std::string_view{};
    const Entity* pet = m_entities.find(v.petGuid);
    if (pet && pet->values.size() >= uf::UNIT_END) {
        v.petInSight = true;
        v.petHealth = pet->value(uf::UNIT_FIELD_HEALTH);
        v.petMaxHealth = pet->value(uf::UNIT_FIELD_MAXHEALTH);
    } else if (m.petGuid == v.petGuid && (m.statsMask & net::kGroupUpdatePetMaxHp) && m.petMaxHealth > 0) {
        v.petHealth = m.petHealth;
        v.petMaxHealth = m.petMaxHealth;
    } else {
        v.petGuid = 0; // nothing known of it yet: no frame
    }
}

// -------------------------------------------------------------------------------------------------------
// Channels.

i32 Social::findChannel(std::string_view name) const {
    for (u32 i = 0; i < kMaxChatChannels; ++i) {
        if (!m_channels[i].name.empty() && sameName(m_channels[i].name, name)) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 Social::allocateChannel(u32 zoneChannel) {
    if (zoneChannel != 0) {
        for (u32 i = 0; i < kMaxChatChannels; ++i) {
            if (m_reserved[i] == zoneChannel && m_channels[i].name.empty()) {
                return static_cast<i32>(i);
            }
        }
    }
    for (u32 i = 0; i < kMaxChatChannels; ++i) {
        if (m_channels[i].name.empty() && m_reserved[i] == 0) {
            return static_cast<i32>(i);
        }
    }
    for (u32 i = 0; i < kMaxChatChannels; ++i) { // every free number is kept for a zone kind: take one
        if (m_channels[i].name.empty()) {
            m_reserved[i] = 0;
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void Social::freeChannel(u32 index) {
    ChatChannel& c = m_channels[index];
    c.name.clear();
    c.zoneChannel = 0;
    c.flags = 0;
    c.joined = false;
}

u32 Social::channelNumber(std::string_view name) const {
    const i32 i = findChannel(name);
    return i >= 0 ? static_cast<u32>(i) + 1 : 0;
}

const ChatChannel* Social::channelByNumber(u32 number) const {
    if (number == 0 || number > kMaxChatChannels || m_channels[number - 1].name.empty()) {
        return nullptr;
    }
    return &m_channels[number - 1];
}

bool Social::joinChannel(std::string_view name, std::string_view password) {
    if (!m_inWorld || name.empty()) {
        return false;
    }
    if (findChannel(name) >= 0) {
        return true;
    }
    const i32 i = allocateChannel(0);
    if (i < 0) {
        return false; // ten channels already
    }
    m_channels[static_cast<u32>(i)].name.assign(name);
    m_body.clear();
    net::writeJoinChannel(m_body, name, password);
    return sendBody(Opcode::CMSG_JOIN_CHANNEL);
}

bool Social::leaveChannel(std::string_view name) {
    const i32 i = findChannel(name);
    if (!m_inWorld || i < 0) {
        return false;
    }
    sendName(Opcode::CMSG_LEAVE_CHANNEL, m_channels[static_cast<u32>(i)].name);
    freeChannel(static_cast<u32>(i)); // a zone kind keeps its number for when it comes back
    return true;
}

void Social::listChannel(std::string_view name) {
    sendName(Opcode::CMSG_CHANNEL_LIST, name);
}

void Social::setZone(u32 zoneId, std::span<const ZoneChannel> wanted) {
    if (!m_inWorld) {
        return;
    }
    m_zoneId = zoneId;
    // Every kind keeps the number it got first, in the order given (General 1, Trade 2...), joined or not.
    for (const ZoneChannel& w : wanted) {
        if (std::find(m_reserved.begin(), m_reserved.end(), w.id) != m_reserved.end()) {
            continue;
        }
        for (u32 i = 0; i < kMaxChatChannels; ++i) {
            if (m_reserved[i] == 0 && m_channels[i].name.empty()) {
                m_reserved[i] = w.id;
                break;
            }
        }
    }
    // Zone kinds the new zone has no channel of (Trade outside cities): left.
    for (u32 i = 0; i < kMaxChatChannels; ++i) {
        ChatChannel& c = m_channels[i];
        if (c.name.empty() || c.zoneChannel == 0) {
            continue;
        }
        const bool keep = std::any_of(wanted.begin(), wanted.end(), [&](const ZoneChannel& w) {
            return w.id == c.zoneChannel && !w.name.empty();
        });
        if (!keep) {
            sendName(Opcode::CMSG_LEAVE_CHANNEL, c.name);
            freeChannel(i);
        }
    }
    for (const ZoneChannel& w : wanted) {
        if (w.name.empty()) {
            continue; // only its number
        }
        i32 at = -1;
        for (u32 i = 0; i < kMaxChatChannels; ++i) {
            if (!m_channels[i].name.empty() && m_channels[i].zoneChannel == w.id) {
                at = static_cast<i32>(i);
            }
        }
        if (at >= 0) {
            ChatChannel& c = m_channels[static_cast<u32>(at)];
            if (sameName(c.name, w.name)) {
                continue;
            }
            sendName(Opcode::CMSG_LEAVE_CHANNEL, c.name); // the same kind in the new zone keeps the number
            c.name.assign(w.name);
            c.joined = false;
        } else {
            if (findChannel(w.name) >= 0) {
                continue; // joined by hand already
            }
            at = allocateChannel(w.id);
            if (at < 0) {
                continue;
            }
            ChatChannel& c = m_channels[static_cast<u32>(at)];
            c.name.assign(w.name);
            c.zoneChannel = w.id;
            m_reserved[static_cast<u32>(at)] = w.id;
        }
        m_body.clear();
        net::writeJoinChannel(m_body, w.name, {});
        sendBody(Opcode::CMSG_JOIN_CHANNEL);
    }
}

void Social::onChannelNotify(const net::ChannelNotify& n) {
    i32 at = findChannel(n.channel);
    switch (n.notice) {
    case net::ChannelNotice::YouJoined:
        if (at < 0) {
            at = allocateChannel(0); // joined by the server (or by an older request)
            if (at >= 0) {
                m_channels[static_cast<u32>(at)].name.assign(n.channel);
            }
        }
        if (at >= 0) {
            m_channels[static_cast<u32>(at)].joined = true;
            m_channels[static_cast<u32>(at)].flags = n.flags;
        }
        break;
    case net::ChannelNotice::YouLeft:
    case net::ChannelNotice::WrongPassword:
    case net::ChannelNotice::NotMember:
    case net::ChannelNotice::Banned:
    case net::ChannelNotice::WrongFaction:
    case net::ChannelNotice::InvalidName:
        if (at >= 0) {
            freeChannel(static_cast<u32>(at));
        }
        break;
    case net::ChannelNotice::Joined:
    case net::ChannelNotice::Left:
    case net::ChannelNotice::OwnerChanged:
    case net::ChannelNotice::Invite:
    case net::ChannelNotice::PlayerAlreadyMember:
        m_queries.playerName(n.guid);
        break;
    case net::ChannelNotice::PlayerKicked:
    case net::ChannelNotice::PlayerBanned:
    case net::ChannelNotice::PlayerUnbanned:
        m_queries.playerName(n.guid);
        m_queries.playerName(n.source);
        break;
    default: break;
    }
    SocialEvent& e = event(SocialEventKind::ChannelNotice);
    e.value[0] = static_cast<i32>(n.notice);
    e.value[1] = at >= 0 ? at + 1 : 0;
    e.name.assign(n.channel);
    e.guid = n.guid;
    e.other = n.source;
    e.text.assign(n.name);
}

// -------------------------------------------------------------------------------------------------------
// AFK, DND, emotes.

bool Social::afk() const {
    const Entity* player = me();
    return player && (player->value(uf::PLAYER_FLAGS) & kPlayerFlagAfk) != 0;
}

bool Social::dnd() const {
    const Entity* player = me();
    return player && (player->value(uf::PLAYER_FLAGS) & kPlayerFlagDnd) != 0;
}

void Social::setAfk(std::string_view message) {
    // CHAT_MSG_AFK in LANG_UNIVERSAL (the only type the server accepts it for): toggles, or changes the message.
    m_body.clear();
    net::writeMessageChat(m_body, net::ChatType::Afk, net::Language::Universal, {}, message);
    sendBody(Opcode::CMSG_MESSAGECHAT);
}

void Social::setDnd(std::string_view message) {
    m_body.clear();
    net::writeMessageChat(m_body, net::ChatType::Dnd, net::Language::Universal, {}, message);
    sendBody(Opcode::CMSG_MESSAGECHAT);
}

void Social::textEmote(u32 textEmote, u64 target) {
    if (textEmote == 0) {
        return;
    }
    m_body.clear();
    net::writeTextEmote(m_body, textEmote, 0, target); // the number is only echoed back
    sendBody(Opcode::CMSG_TEXT_EMOTE);
}

void Social::roll(u32 minimum, u32 maximum) {
    if (minimum > maximum || maximum > 1000000) {
        return; // the server ignores it too
    }
    m_body.clear();
    net::writeRandomRoll(m_body, minimum, maximum);
    sendBody(Opcode::MSG_RANDOM_ROLL);
}

// -------------------------------------------------------------------------------------------------------
// Friends and ignore list.

bool Social::isFriend(u64 guid) const {
    return std::any_of(m_friends.begin(), m_friends.end(), [&](const FriendInfo& f) { return f.guid == guid; });
}

bool Social::isIgnored(u64 guid) const {
    return guid != 0 && std::find(m_ignored.begin(), m_ignored.end(), guid) != m_ignored.end();
}

void Social::addFriend(std::string_view name) {
    if (!name.empty() && sendName(Opcode::CMSG_ADD_FRIEND, name)) {
        m_lastFriendName.assign(name);
    }
}

void Social::removeFriend(u64 guid) {
    sendGuid(Opcode::CMSG_DEL_FRIEND, guid);
}

void Social::addIgnore(std::string_view name) {
    if (!name.empty() && sendName(Opcode::CMSG_ADD_IGNORE, name)) {
        m_lastFriendName.assign(name);
    }
}

void Social::removeIgnore(u64 guid) {
    sendGuid(Opcode::CMSG_DEL_IGNORE, guid);
}

void Social::refreshFriends() {
    send(Opcode::CMSG_FRIEND_LIST);
}

// -------------------------------------------------------------------------------------------------------
// Who.

void Social::who(const net::WhoRequest& request) {
    m_body.clear();
    net::writeWho(m_body, request);
    if (sendBody(Opcode::CMSG_WHO)) {
        m_who.pending = true;
    }
}

// -------------------------------------------------------------------------------------------------------
// Guild.

const GuildEmblem* Social::guildEmblem(u32 guildId) const {
    const auto it = m_guildEmblems.find(guildId);
    return guildId != 0 && it != m_guildEmblems.end() ? &it->second : nullptr;
}

std::string_view Social::guildName(u32 guildId) {
    if (guildId == 0) {
        return {};
    }
    if (const auto it = m_guildNames.find(guildId); it != m_guildNames.end()) {
        return it->second;
    }
    if (m_inWorld && !m_guildAsked[guildId]) {
        m_body.clear();
        net::writeU32(m_body, guildId);
        m_guildAsked[guildId] = sendBody(Opcode::CMSG_GUILD_QUERY);
    }
    return {};
}

void Social::requestRoster() {
    if (m_guild.id == 0) {
        return;
    }
    if (send(Opcode::CMSG_GUILD_ROSTER)) {
        m_rosterAsked = true;
    }
    if (!m_guildInfoAsked) {
        m_guildInfoAsked = send(Opcode::CMSG_GUILD_INFO);
    }
}

void Social::onGuildRoster(const net::GuildRoster& roster) {
    m_guild.motd.assign(roster.motd);
    m_guild.info.assign(roster.info);
    m_guild.rankRights.assign(roster.rankRights.begin(), roster.rankRights.end());
    m_guild.members.resize(roster.members.size());
    for (usize i = 0; i < roster.members.size(); ++i) {
        const net::GuildRoster::Member& in = roster.members[i];
        GuildMember& out = m_guild.members[i];
        out.guid = in.guid;
        out.name.assign(in.name);
        out.presence = in.presence;
        out.rank = in.rank;
        out.level = in.level;
        out.cls = in.cls;
        out.zone = in.zone;
        out.offlineDays = in.offlineDays;
        out.note.assign(in.note);
        out.officerNote.assign(in.officerNote);
    }
    m_guild.rosterKnown = true;
    ++m_guild.rosterVersion;
}

void Social::guildInvite(std::string_view name) {
    if (!name.empty()) {
        sendName(Opcode::CMSG_GUILD_INVITE, name);
    }
}

void Social::acceptGuildInvite() {
    if (!m_guild.pendingInviter.empty()) {
        send(Opcode::CMSG_GUILD_ACCEPT);
        m_guild.pendingInviter.clear();
        m_guild.pendingGuild.clear();
    }
}

void Social::declineGuildInvite() {
    if (!m_guild.pendingInviter.empty()) {
        send(Opcode::CMSG_GUILD_DECLINE);
        m_guild.pendingInviter.clear();
        m_guild.pendingGuild.clear();
    }
}

void Social::leaveGuild() {
    if (m_guild.id != 0) {
        send(Opcode::CMSG_GUILD_LEAVE);
    }
}

void Social::guildPromote(std::string_view name) {
    sendName(Opcode::CMSG_GUILD_PROMOTE, name);
}

void Social::guildDemote(std::string_view name) {
    sendName(Opcode::CMSG_GUILD_DEMOTE, name);
}

void Social::guildRemove(std::string_view name) {
    sendName(Opcode::CMSG_GUILD_REMOVE, name);
}

void Social::setGuildMotd(std::string_view motd) {
    sendName(Opcode::CMSG_GUILD_MOTD, motd);
}

// -------------------------------------------------------------------------------------------------------
// Duels.

void Social::acceptDuel() {
    if (m_duel.phase == DuelPhase::Challenged) {
        sendGuid(Opcode::CMSG_DUEL_ACCEPTED, m_duel.arbiter);
    }
}

void Social::declineDuel() {
    if (m_duel.phase != DuelPhase::None) {
        sendGuid(Opcode::CMSG_DUEL_CANCELLED, m_duel.arbiter);
    }
}

} // namespace vd::game
